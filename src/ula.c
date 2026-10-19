/* ula.c - the Ferranti ULA 6C001 (Mark 7) of the ZX Spectrum 48K, at its
 * pins.
 *
 * The ULA makes the video picture, fetching the screen from the lower 16K
 * of RAM (the 4116s) through its own address and RAS/CAS pins, makes the
 * CPU clock and stops it (contention) when the CPU wants that RAM while
 * the screen is being fetched, decodes the CPU's accesses to the lower
 * 16K and the ROM, and is the I/O port 0xFE: keyboard, border, MIC, EAR.
 *
 * Where this comes from. The logic is that of the 6C001 as recovered from
 * its die by the emu-russia/ula project (a netlist of 661 NOR gates, CC0),
 * and the timing of every pin was taken from a gate level simulation of
 * that netlist. The behavior is then written here directly, in half
 * periods of the 14 MHz oscillator, as simulating the gates themselves
 * would be too slow. tools/ulacheck.c keeps it honest: it runs the gates
 * on the inputs this model saw (ZXHW_ULALOG) and compares every output
 * at every half period, and every pixel. On a booting Spectrum, a running
 * BASIC program and the ROM's editor, all of them are the same, except
 * three simplifications, all harmless for the parts on the board:
 *
 * - the address pins go from row to column half a period after /RAS
 *   falls, and the gates half a period earlier, while /RAS is still
 *   falling (the 4116 latches the row on the edge either way);
 * - the gates glitch /RAS high for a gate delay between the bitmap and
 *   the attribute fetch, which a 4116 ignores;
 * - when the CPU ends a port read, the model lets the data pins go in the
 *   same half period (the gates a half period later: nobody reads them).
 *
 * Time. A pixel tick (7 MHz) is two periods of the 14 MHz oscillator, so
 * four half periods; C counts the ticks of a line (0..447), V the lines
 * of a frame (0..311). The screen is fetched during C 0..255 of lines
 * 0..191; the interrupt is at line 248 (so 64 lines before the screen).
 *
 * The screen fetch. In every block of 16 ticks, ticks 8..15 fetch two
 * character cells: bitmap and attribute of cell 2k at ticks 8..11, of
 * cell 2k+1 at 12..15. The two bytes of a cell are in the same DRAM row
 * (that is what the odd screen layout is for), so each pair is one /RAS
 * cycle with two /CAS cycles (page mode):
 *
 *   tick       8      9      10     11     (half periods 0..3 of each)
 *   /RAS       -___   ____   ____   _---
 *   /CAS       --__   ___-   -___   ___-
 *   A0-A6      RR CC  CCCC   CAAA   ARRR   (Row, bitmap Col, Attribute col)
 *   latch             data ^        attr ^ (closed just before /CAS rises)
 *
 * The picture. The bitmap byte goes to a shift register at the start of
 * each cell, the attribute to an output latch; the border colour goes
 * through the same latch, so a change of the border shows from the next
 * cell boundary (every 8 pixels), even in the border.
 *
 * The CPU clock is high for 5 half periods and low for 3 (C odd, and the
 * first half of C even). Contention: when the clock is about to fall in
 * ticks 4..15 of a block on a screen line, and the CPU puts out an address
 * of 0x4000-0x7FFF without /MREQ yet (the first T state of a memory cycle)
 * or starts an I/O cycle, the ULA keeps the clock high until the block
 * ends: the CPU waits 6,5,4,3,2,1,0,0 T states depending on where it is. */
#include "ula.h"
#include <stdio.h>
#include <string.h>

/* Pin numbers. */
enum {
    P_CAS = 1, P_WR = 2, P_RD = 3, P_WE = 4, P_A0 = 5, P_INT = 12, P_D0 = 18, P_KB0 = 19, P_KB1 = 20,
    P_D1 = 21, P_D2 = 22, P_KB2 = 23, P_KB3 = 24, P_D3 = 25, P_KB4 = 26, P_D4 = 27, P_SOUND = 28,
    P_D5 = 29, P_D6 = 30, P_D7 = 31, P_PHICPU = 32, P_IOREQ = 33, P_ROMCS = 34, P_RAS = 35, P_A14 = 36,
    P_A15 = 37, P_MREQ = 38, P_OSC = 39
};
static const int addr_pins[7] = { 5, 6, 7, 8, 9, 10, 11 };
static const int data_pins[8] = { P_D0, P_D1, P_D2, P_D3, P_D4, P_D5, P_D6, P_D7 };
static const int kb_pins[5] = { P_KB0, P_KB1, P_KB2, P_KB3, P_KB4 };

/* The screen addresses of character cell k (0..31) of pixel line v, as
 * the 14 address bits of the lower 16K: bitmap 010 v7v6 v2v1v0 v5v4v3 k,
 * attribute 0110 v7v6 v5v4v3 k (the leading 01 is A15/A14). The DRAM
 * row is the low 7 bits, the column the high 7. */
static unsigned bitmap_addr(unsigned v, unsigned k) {
    return ((v & 0xC0) << 5) | ((v & 7) << 8) | ((v & 0x38) << 2) | k;
}
static unsigned attr_addr(unsigned v, unsigned k) { return 0x1800 | ((v & 0xF8) << 2) | k; }

static bool on_screen(const ula_state_t *u) { return u->c < 256 && u->v < 192; }

/* What the ULA drives on its pins, from its state and its inputs (the
 * paths that don't wait for the clock: RAS for the CPU, ROMCS, WE, the
 * port read). */
static void ula_eval(sim_t *s, part_t *p) {
    ula_state_t *u = p->state;
    bool mreq = !pin_in(s, p, P_MREQ), a14 = pin_in(s, p, P_A14), a15 = pin_in(s, p, P_A15);
    bool rd = !pin_in(s, p, P_RD), wr = !pin_in(s, p, P_WR), iorq = !pin_in(s, p, P_IOREQ);
    bool ram16 = a14 && !a15 && mreq;                      /* the CPU wants 0x4000-0x7FFF */
    bool ras = u->ras || ram16;
    /* /RAS is let go when the CPU accesses the ROM in the border: then the
     * Z80's /RFSH (through R57) refreshes the lower RAM. */
    bool ras_oe = !(!a15 && !a14 && !on_screen(u) && mreq);
    pin_out(s, p, P_RAS, ras_oe ? DRV_BIT(!ras) : DRV_Z);
    pin_out(s, p, P_CAS, DRV_BIT(!(u->cas || u->cpu_cas)));
    pin_out(s, p, P_WE, DRV_BIT(!(ram16 && wr)));
    pin_out(s, p, P_ROMCS, DRV_BIT(a15 || a14));
    pin_out(s, p, P_INT, DRV_BIT(!u->irq));
    /* The CPU clock: an inverting open collector output (R73 pulls it up,
     * TR3 inverts it again for the Z80). */
    pin_out(s, p, P_PHICPU, u->cpuclk ? DRV_LO : DRV_Z);
    part_drive_bus(s, p, addr_pins, 7, u->addr, u->ae);
    /* The SOUND pin: the speaker bit (an analog level on the real chip: the
     * MIC bit only moves it a little). The EAR input is the same pin, read
     * by a comparator: on this board it reads back the speaker bit. */
    pin_out(s, p, P_SOUND, DRV_BIT(u->speaker));
    /* Port 0xFE read: keyboard on D0-D4, EAR on D6. The data pins are open
     * collector: they only pull low; bits 5 and 7 are never driven. */
    bool port_rd = iorq && rd && !pin_in(s, p, P_A0) && u->ioreqt2;
    uint8_t low = 0;
    if (port_rd) {
        for (int i = 0; i < 5; i++) if (!pin_in(s, p, kb_pins[i])) low |= (uint8_t)(1 << i);
        if (!u->ear_in && !pin_in(s, p, P_SOUND)) low |= 0x40;
    }
    for (int i = 0; i < 8; i++) pin_out(s, p, data_pins[i], (low >> i) & 1 ? DRV_LO : DRV_Z);
    u->dlow = low;      /* only for ula_log() */
}

uint32_t ula_color(int index, bool bright) {
    uint32_t lvl = bright ? 0xFF : 0xD7;
    uint32_t r = (index & 2) ? lvl : 0, g = (index & 4) ? lvl : 0, b = (index & 1) ? lvl : 0;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* The pixel of the current tick, into the picture. Cell k of a line is
 * shown 16 ticks after the start of its block's fetch, from what the
 * latches captured. */
static void put_pixel(ula_state_t *u) {
    /* The border colour goes through the latch of the attribute shown,
     * which loads once per character cell: a new border colour shows
     * from the next 8 pixel boundary. */
    if ((u->c & 7) == 0) u->border_shown = u->border_ao;
    if (u->v < 192 && u->c >= 16 && u->c < 272) {
        int px = u->c - 16, k = px >> 3;
        uint8_t a = u->line_attr[k], b = u->line_bitmap[k];
        bool ink = (b >> (7 - (px & 7))) & 1;
        if ((a & 0x80) && ((u->frames >> 4) & 1)) ink = !ink;      /* FLASH */
        u->pix = (uint8_t)((ink ? (a & 7) : ((a >> 3) & 7)) | ((a & 0x40) >> 3));
    } else {
        u->pix = u->border_shown;
    }
    int x = ((int)u->c - 16 + 48 + ULA_TICKS_PER_LINE) % ULA_TICKS_PER_LINE;
    int y = u->v < 248 ? (int)u->v + 48 : (int)u->v - 264;
    if (!u->fb || x >= ULA_SCREEN_W || y < 0 || y >= ULA_SCREEN_H) return;
    u->fb[y * ULA_SCREEN_W + x] = ula_color(u->pix & 7, u->pix & 8);
}

void ula_step(sim_t *s, part_t *p) {
    ula_state_t *u = p->state;
    if (p->dead || !u->running) return;
    /* Time. */
    u->half = (u->half + 1) & 3;
    if (u->half == 0) {
        if (++u->c == ULA_TICKS_PER_LINE) {
            u->c = 0;
            if (++u->v == ULA_LINES) { u->v = 0; u->frames++; }
        }
        put_pixel(u);
    }
    unsigned c = u->c, h = u->half, blk = c & 15;
    bool screen = on_screen(u);
    /* Inputs, as the board settled them. */
    bool mreq = !pin_in(s, p, P_MREQ), iorq = !pin_in(s, p, P_IOREQ);
    bool rd = !pin_in(s, p, P_RD), wr = !pin_in(s, p, P_WR);
    bool a14 = pin_in(s, p, P_A14), a15 = pin_in(s, p, P_A15);
    u->inputs = (uint32_t)(!mreq) | (uint32_t)(!iorq) << 1 | (uint32_t)(!rd) << 2 | (uint32_t)(!wr) << 3 |
                (uint32_t)a14 << 4 | (uint32_t)a15 << 5 | (uint32_t)pin_in(s, p, P_A0) << 6 |
                (uint32_t)(u->ear_in || pin_in(s, p, P_SOUND)) << 7 |
                part_read_bus(s, p, data_pins, 8) << 8 | part_read_bus(s, p, kb_pins, 5) << 16;
    /* The screen fetch (see the table at the top). */
    bool ras = false, cas = false, ae = false;
    uint8_t addr = u->addr;
    /* The address pins are driven from the second half of tick 7 to the
     * first half of the next block (when that is still on screen). */
    if (blk == 0 && h == 0 && c > 0 && c <= 256 && u->v < 192) ae = true;
    if (screen && (blk >= 8 || (blk == 7 && h >= 1))) {
        unsigned k = ((c >> 4) << 1) | (blk >= 12), sub = blk & 3;
        unsigned row = bitmap_addr(u->v, k) & 0x7F;
        unsigned bcol = bitmap_addr(u->v, k) >> 7, acol = attr_addr(u->v, k) >> 7;
        ae = true;
        if (blk == 7) {
            addr = (uint8_t)row;
        } else {
            ras = (sub == 0 && h >= 1) || sub == 1 || sub == 2 || (sub == 3 && h == 0);
            cas = (sub == 0 && h >= 2) || (sub == 1 && h <= 2) || (sub == 2 && h >= 1) || (sub == 3 && h <= 2);
            if (sub == 0) addr = (uint8_t)(h <= 1 ? row : bcol);
            else if (sub == 1) addr = (uint8_t)bcol;
            else if (sub == 2) addr = (uint8_t)(h == 0 ? bcol : acol);
            else if (h == 0) addr = (uint8_t)acol;
            else {      /* the row of the other cell of the pair */
                addr = (uint8_t)(bitmap_addr(u->v, k ^ 1) & 0x7F);
            }
            /* The latches close just before /CAS rises. */
            if (h == 3 && (sub == 1 || sub == 3)) {
                uint8_t d = (uint8_t)part_read_bus(s, p, data_pins, 8);
                if (sub == 1) u->data = d;
                else { u->attr = d; u->line_bitmap[k] = u->data; u->line_attr[k] = u->attr; }
            }
        }
    }
    /* Port 0xFE write: the latch is open while the CPU writes. */
    if (iorq && wr && !pin_in(s, p, P_A0) && u->ioreqt2) {
        uint8_t d = (uint8_t)part_read_bus(s, p, data_pins, 8);
        u->border = d & 7;
        u->mic = (d >> 3) & 1;
        if (u->speaker != ((d >> 4) & 1)) {     /* the SOUND pin changes */
            u->speaker = (d >> 4) & 1;
            sim_mark_dirty(s, (int)(p - s->parts));
        }
    }
    if ((c & 7) == 5) u->border_ao = u->border;
    /* The CPU clock and contention (gates g383..g405 of the netlist). The
     * /MREQ and /IOREQ latches are open while the clock is low (from the
     * half period in which it goes low). */
    bool border = !screen, c23 = (c & 12) != 0;
    bool base = (c & 1) || h == 0;
    bool w362 = !u->ioreqt2 && !border && u->cpuclk && !u->mreqt2;
    bool mem = (a14 || iorq) && (iorq || !a15) && c23 && w362;
    bool io = !border && u->cpuclk && iorq && c23 && !u->ioreqt2;
    bool cpuclk = base || mem || io;
    if (!cpuclk) { u->mreqt2 = mreq; u->ioreqt2 = iorq; }
    /* /INT: 64 ticks (32 T states) at the start of line 248, half a
     * period behind the counter like everything decoded from it. */
    bool irq = u->v == 248 && ((c == 0 && h >= 1) || (c >= 1 && c < 64) || (c == 64 && h == 0));
    /* The CAS of a CPU cycle comes through a long chain of gates: half a
     * period after the request is seen, a period after it begins. */
    bool cpu_cas = u->cpu_cas_req;
    u->cpu_cas_req = (rd || wr) && a14 && !a15 && mreq;
    if (cpuclk != u->cpuclk || ras != u->ras || cas != u->cas || ae != u->ae || addr != u->addr ||
        irq != u->irq || cpu_cas != u->cpu_cas) {
        u->cpuclk = cpuclk; u->ras = ras; u->cas = cas; u->ae = ae; u->addr = addr;
        u->irq = irq; u->cpu_cas = cpu_cas;
        sim_mark_dirty(s, (int)(p - s->parts));
    }
}

void ula_log(const ula_state_t *u, uint64_t step, char *buf, int len) {
    snprintf(buf, len, "%llu %u %u %u in=%06X ras=%d cas=%d ae=%d a=%02X clk=%d int=%d ccas=%d dlow=%02X pix=%X",
             (unsigned long long)step, u->c, u->v, u->half, (unsigned)u->inputs, u->ras, u->cas, u->ae, u->addr,
             u->cpuclk, u->irq, u->cpu_cas, u->dlow, u->pix);
}

static void ula_reset(sim_t *s, part_t *p) {
    (void)s;
    ula_state_t *u = p->state;
    uint32_t *fb = u->fb;
    bool ear = u->ear_in, running = u->running;
    memset(u, 0, sizeof(*u));
    u->fb = fb;
    u->ear_in = ear;
    u->running = running;
    u->cpuclk = true;
}

const part_type_t PT_ULA = { "ULA 6C001",
    "/CAS:O /WR:T /RD:T /WE:O A0:X A1:O A2:O A3:O A4:O A5:O A6:O /INT:O VCC2:P VCC1:P U:O V:O /Y:O "
    "D0:B KB0:T KB1:T D1:B D2:B KB2:T KB3:T D3:B KB4:T D4:B SOUND:X D5:m D6:B D7:m /PHICPU:O "
    "/IOREQ:T /ROMCS:O /RAS:O A14:T A15:T /MREQ:T OSC:m GND:P",
    ula_eval, 0, ula_reset, sizeof(ula_state_t), TECH_TTL };
