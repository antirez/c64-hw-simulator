/* zx48.c - the Sinclair ZX Spectrum 48K, board Issue 3B.
 *
 * HOW TIME MOVES
 *
 * Everything on this board is timed by the ULA: its 14 MHz oscillator (X1)
 * is the board clock. We simulate every half period of it (8 per CPU T
 * state): the ULA advances, the board settles, the Z80 sees an edge of its
 * clock if there was one (the ULA makes the clock, and stops it to keep
 * the CPU off the lower RAM while it reads the screen), then the memories
 * see their strobes (a commit) and the board settles again.
 *
 * What is on the board, as on the real one: the CPU and the ULA share the
 * lower 16K (eight 4116 DRAMs) through 470 ohm resistors on the data bus
 * and 330 ohm resistors after the 74LS157 address multiplexers, so either
 * side can drive its own half of the bus; the upper 32K (eight 4532, half
 * good 64K DRAMs) has its own glue (IC23-IC26) with two RC delays timing
 * its RAS and CAS; the ROM is selected by the ULA. The keyboard half rows
 * are the address lines A8-A15, through D1-D8.
 *
 * Board: liveboxandy's KiCad Issue 3B (CC0, from PABB's gerbers), checked
 * against Timex's Issue 3 schematic (356-201891, June 1983). */
#include "zx48.h"
#include "zx48-rom.h"
#include "sokol/sokol_app.h"   /* key codes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Z80 pins (the address pins A0..A15, the data pins D0..D7). */
static const int z80_addr_pins[16] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5 };
static const int z80_data_pins[8] = { 14, 15, 12, 8, 7, 9, 10, 13 };
enum { Z_CLK = 6, Z_INT = 16, Z_NMI = 17, Z_HALT = 18, Z_MREQ = 19, Z_IORQ = 20, Z_RD = 21, Z_WR = 22,
       Z_BUSAK = 23, Z_WAIT = 24, Z_BUSRQ = 25, Z_RESET = 26, Z_M1 = 27, Z_RFSH = 28 };

/* ------------------------------------------------------------------------ */
/* Z80 (IC2)                                                                 */
/* ------------------------------------------------------------------------ */

/* ZXHW_CONTENTION=1: for every T state in which the CPU puts out an
 * address in 0x4000-0x7FFF without /MREQ (the first T state of a memory
 * cycle, or an internal one), print when it started (T states since /INT
 * went low) and how many T states the ULA held the clock. */
static bool contention_log;
static uint64_t frame_step, rise_step;
static bool rise_contended;

/* The outputs of the CPU, from the core's pins: the address and the bus
 * control lines while AOE says the CPU drives them (not in reset, not
 * while it gave the bus away), the data bus while DOE says so. */
static void cpu_eval(sim_t *s, part_t *p) {
    zx48_t *z = ZX(p->user);
    uint64_t pins = z->cpu_pins;
    bool aoe = (pins & Z80_AOE) != 0;
    part_drive_bus_cached(s, p, z80_addr_pins, 16, Z80_GET_ADDR(pins), aoe, &z->cache_addr);
    pin_out(s, p, Z_MREQ, aoe ? DRV_BIT(!(pins & Z80_MREQ)) : DRV_Z);
    pin_out(s, p, Z_IORQ, aoe ? DRV_BIT(!(pins & Z80_IORQ)) : DRV_Z);
    pin_out(s, p, Z_RD, aoe ? DRV_BIT(!(pins & Z80_RD)) : DRV_Z);
    pin_out(s, p, Z_WR, aoe ? DRV_BIT(!(pins & Z80_WR)) : DRV_Z);
    pin_out(s, p, Z_M1, DRV_BIT(!(pins & Z80_M1)));
    pin_out(s, p, Z_RFSH, DRV_BIT(!(pins & Z80_RFSH)));
    pin_out(s, p, Z_HALT, DRV_BIT(!(pins & Z80_HALT)));
    pin_out(s, p, Z_BUSAK, DRV_BIT(!(pins & Z80_BUSAK)));
    part_drive_bus_cached(s, p, z80_data_pins, 8, Z80_GET_DATA(pins), (pins & Z80_DOE) != 0, &z->cache_data);
}

/* An edge of the CPU clock: the core (z80.h, stepped on every edge with
 * the Zilog bus timing) gets the clock level and what it sees on its input
 * pins, and puts out its pins for the half cycle that starts. */
static void cpu_edge(board_t *b, int clk) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    part_t *p = &s->parts[z->p_cpu];
    z->last_edge = s->cycle;
    if (p->dead) return;
    if (contention_log) {
        if (!clk && rise_contended) {
            uint64_t high = frame_step - rise_step;
            printf("contend t=%llu delay=%llu\n", (unsigned long long)(rise_step / 8),
                   (unsigned long long)((high - 5) / 8));
        }
        if (clk) rise_step = frame_step;
    }
    uint64_t pins = clk ? Z80_CLK : 0;
    Z80_SET_DATA(pins, (uint64_t)part_read_bus(s, p, z80_data_pins, 8));
    if (!pin_in(s, p, Z_INT)) pins |= Z80_INT;
    if (!pin_in(s, p, Z_NMI)) pins |= Z80_NMI;
    if (!pin_in(s, p, Z_WAIT)) pins |= Z80_WAIT;
    if (!pin_in(s, p, Z_RESET)) pins |= Z80_RES;
    if (!pin_in(s, p, Z_BUSRQ)) pins |= Z80_BUSRQ;
    z->in_reset = (pins & Z80_RES) != 0;
    uint64_t out = z80_tick(&z->cpu, pins);
    if (out != z->cpu_pins) { z->cpu_pins = out; sim_mark_dirty(s, z->p_cpu); }
    if (contention_log && clk) {
        uint16_t a = Z80_GET_ADDR(out);
        rise_contended = (out & Z80_AOE) && (a & 0xC000) == 0x4000 && !(out & (Z80_MREQ | Z80_IORQ));
    }
}

static const part_type_t PT_Z80 = { "Z80A",
    "A11:O A12:O A13:O A14:O A15:O CLK:m D4:B D3:B D5:B D6:B VCC:P D2:B D7:B D0:B D1:B /INT:m /NMI:m "
    "/HALT:O /MREQ:O /IORQ:O /RD:O /WR:O /BUSAK:O /WAIT:m /BUSRQ:m /RESET:m /M1:O /RFSH:O GND:P A0:O "
    "A1:O A2:O A3:O A4:O A5:O A6:O A7:O A8:O A9:O A10:O", cpu_eval, 0, 0, 0, TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

/* ZXHW_TRACE=FROM:COUNT prints the lower RAM signals at every half
 * period of T states FROM..FROM+COUNT (for debugging the timing). */
static uint64_t trace_from, trace_to;
static FILE *ula_log_file;      /* ZXHW_ULALOG=path[:from]: the ULA at every step */
static uint64_t ula_log_step, ula_log_from;     /* (from T state 'from' on) */
static int netlvl(sim_t *s, const char *name) {
    int n = sim_find_net(s, name);
    if (n < 0) return -1;
    const net_t *r = &s->nets[s->nets[n].root];
    return r->strength == S_Z ? 2 : r->level;
}
static unsigned netbus(sim_t *s, const char *fmt, int n) {
    unsigned v = 0;
    char name[32];
    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof(name), fmt, i);
        v |= (unsigned)(netlvl(s, name) == 1) << i;
    }
    return v;
}
static void trace_step(board_t *b, int k) {
    sim_t *s = &b->sim;
    zx48_t *z = ZX(b);
    ula_state_t *u = s->parts[z->p_ula].state;
    printf("%llu.%d C=%3u.%u V=%3u clk=%d mreq=%d rd=%d wr=%d A=%04X D=%02X | LRAS=%d S=%d LCAS=%d WE=%d LA=%02X UD=%02X ae=%d\n",
           (unsigned long long)s->cycle, k, u->c, u->half, u->v, netlvl(s, "Net-(IC2-/CLK)"), netlvl(s, "/MREQ"),
           netlvl(s, "/RD"), netlvl(s, "/WR"), netbus(s, "A%d", 16), netbus(s, "D%d", 8), netlvl(s, "/LRAS"),
           netlvl(s, "Net-(IC3-S)"), netlvl(s, "/LCAS"), netlvl(s, "LDRAM_WE"), netbus(s, "LA%d", 7),
           netbus(s, "Net-(IC1-D%d)", 8), u->ae);
}

/* One half period of the oscillator. */
static void zx_step(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    ula_step(s, &s->parts[z->p_ula]);
    if (contention_log) {
        static bool irq_prev;
        bool irq = ((ula_state_t *)s->parts[z->p_ula].state)->irq;
        frame_step = irq && !irq_prev ? 0 : frame_step + 1;
        irq_prev = irq;
    }
    sim_settle(s);
    int clk = sim_read(s, z->pin_clk);
    if (clk != z->clk) {
        z->clk = clk;
        cpu_edge(b, clk);
        sim_settle(s);
    }
    /* The memories and delays only act on edges: if no net changed since
     * the last commit there is nothing to see. */
    if (s->changes != z->changes_seen) {
        sim_commit(s);
        sim_settle(s);
        z->changes_seen = s->changes;
    }
    board_sample(b);
    if (ula_log_file && s->cycle >= ula_log_from) {
        char line[160];
        ula_log(s->parts[z->p_ula].state, ula_log_step++, line, sizeof(line));
        fprintf(ula_log_file, "%s\n", line);
    }
}

/* The power on reset: C27 discharged at power on keeps /RESET low until
 * R31 charges it. */
static void reset_tick(board_t *b) {
    zx48_t *z = ZX(b);
    if (z->pin_reset_cap < 0) return;
    sim_drive(&b->sim, z->pin_reset_cap, b->sim.cycle < z->reset_until ? DRV_LO : DRV_Z);
}

static void zx_run(board_t *b, int cycles) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    for (int i = 0; i < cycles; i++) {
        s->cycle++;
        reset_tick(b);
        for (int k = 0; k < ZX_STEPS_PER_CYCLE; k++) {
            zx_step(b);
            if (s->cycle >= trace_from && s->cycle < trace_to) trace_step(b, k);
        }
        z->cpu_running = s->cycle - z->last_edge < 1000;
    }
}

/* ------------------------------------------------------------------------ */
/* The board                                                                 */
/* ------------------------------------------------------------------------ */

static int zx_rail_of(const char *name) {
    if (!strcmp(name, "GND")) return RAIL_GND;
    if (!strcmp(name, "+5V")) return RAIL_VCC;
    return RAIL_NONE;
}

static int zx_kind_of(const char *ref) {
    if (!strncmp(ref, "IC", 2)) return PART_IC;
    if (!strncmp(ref, "TR", 2) || !strncmp(ref, "TV", 2)) return !strncmp(ref, "TV", 2) ? PART_CONNECTOR : PART_OTHER;
    if (ref[0] == 'R' && ref[1] >= '0' && ref[1] <= '9') return PART_RESISTOR;
    if (!strncmp(ref, "JROM", 4) || !strncmp(ref, "TI", 2) || !strncmp(ref, "OKI", 3)) return PART_LINK;
    if (!strncmp(ref, "KB", 2) || !strncmp(ref, "EAR", 3) || !strncmp(ref, "MIC", 3) || !strcmp(ref, "E1") ||
        ref[0] == 'J') return PART_CONNECTOR;
    return PART_OTHER;
}

static const part_def_t part_defs[] = {
    { "IC1", &PT_ULA, "ULA", ROLE_CHIP, "6C001\nvideo, I/O",
      "Ferranti ULA 6C001: video, CPU clock and contention, lower RAM control, port 0xFE" },
    { "IC2", &PT_Z80, "CPU", ROLE_CHIP, "Z80A", "Z80A CPU, 3.5 MHz" },
    { "IC5", &PT_23128, "ROM", ROLE_ROM, "23128\n$0000-$3FFF", "23128 ROM, $0000-$3FFF" },
    { "IC6", &PT_4116, "LRAM0", ROLE_VRAM, "4116\nbit D0\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D0 of $4000-$7FFF (the screen is here)" },
    { "IC7", &PT_4116, "LRAM1", ROLE_VRAM, "4116\nbit D1\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D1 of $4000-$7FFF (the screen is here)" },
    { "IC8", &PT_4116, "LRAM2", ROLE_VRAM, "4116\nbit D2\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D2 of $4000-$7FFF (the screen is here)" },
    { "IC9", &PT_4116, "LRAM3", ROLE_VRAM, "4116\nbit D3\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D3 of $4000-$7FFF (the screen is here)" },
    { "IC10", &PT_4116, "LRAM4", ROLE_VRAM, "4116\nbit D4\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D4 of $4000-$7FFF (the screen is here)" },
    { "IC11", &PT_4116, "LRAM5", ROLE_VRAM, "4116\nbit D5\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D5 of $4000-$7FFF (the screen is here)" },
    { "IC12", &PT_4116, "LRAM6", ROLE_VRAM, "4116\nbit D6\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D6 of $4000-$7FFF (the screen is here)" },
    { "IC13", &PT_4116, "LRAM7", ROLE_VRAM, "4116\nbit D7\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D7 of $4000-$7FFF (the screen is here)" },
    { "IC15", &PT_4532, "URAM0", ROLE_RAM, "4532\nbit D0\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D0 of $8000-$FFFF" },
    { "IC16", &PT_4532, "URAM1", ROLE_RAM, "4532\nbit D1\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D1 of $8000-$FFFF" },
    { "IC17", &PT_4532, "URAM2", ROLE_RAM, "4532\nbit D2\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D2 of $8000-$FFFF" },
    { "IC18", &PT_4532, "URAM3", ROLE_RAM, "4532\nbit D3\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D3 of $8000-$FFFF" },
    { "IC19", &PT_4532, "URAM4", ROLE_RAM, "4532\nbit D4\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D4 of $8000-$FFFF" },
    { "IC20", &PT_4532, "URAM5", ROLE_RAM, "4532\nbit D5\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D5 of $8000-$FFFF" },
    { "IC21", &PT_4532, "URAM6", ROLE_RAM, "4532\nbit D6\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D6 of $8000-$FFFF" },
    { "IC22", &PT_4532, "URAM7", ROLE_RAM, "4532\nbit D7\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D7 of $8000-$FFFF" },
    { "IC3", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 lower RAM address mux: CPU A0-A3 (row), A7-A10 (column), switched by /RAS" },
    { "IC4", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 lower RAM address mux: CPU A4-A6 (row), A11-A13 (column), switched by /RAS" },
    { "IC25", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157", "74LS157 upper RAM address mux (row, column)" },
    { "IC26", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 upper RAM address mux (row, column; A7 set by the TI/OKI links)" },
    { "IC23", &PT_74LS32, "OR", ROLE_GLUE, "74LS32", "74LS32 OR gates: upper RAM /RAS, /CAS, mux select, /WRITE" },
    { "IC24", &PT_74LS00, "NAND", ROLE_GLUE, "74LS00", "74LS00 NAND gates: upper RAM select (A15 and RD or WR)" },
    { "IC14", &PT_LM1889, "PAL", ROLE_OTHER, "LM1889\nnot\nsimulated",
      "LM1889 PAL video modulator (analog, not simulated: the monitor shows the ULA's picture)" },
};

/* Series resistors between logic nets: the 470 ohm on the data bus, the
 * 330 ohm on the lower RAM address lines, /INT, /IORQ to the ULA, the mux
 * select, /ROMCS and /RFSH to /RAS (the lower RAM refresh in the border). */
static const char *const series[] = {
    "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R17", "R18", "R19", "R20", "R21", "R22", "R23",
    "R26", "R27", "R33", "R57",
};
/* Links fitted: the ROM select (JROM1: /CE to /MREQ, OE1 to /ROMCS) and
 * the RAM type, here TI 4532-3 (A14 into the column, low row A7). */
static const char *const links_off[] = { "TI-4", "OKI1", "OKI-L1", "OKI-H1" };

/* The connectors: the keyboard ribbons, the sockets, the edge connector
 * (its pins named after their nets, see setup_edge). */
static const conn_def_t conn_defs[] = {
    { "KB1", "KEYBOARD LINES", "1=KB0 2=KB1 3=KB2 4=KB3 5=KB4" },
    { "KB2", "KEYBOARD HALF ROWS", "1=A11 2=A10 3=A9 4=A12 5=A13 6=A8 7=A14 8=A15" },
    { "EAR1", "EAR", "1=GND 2=EAR_IN" },
    { "MIC1", "MIC", "1=GND 2=MIC_OUT" },
    { "TV1", "TV", "1=RF_SUPPLY 2=VIDEO 3=GND" },
    { "J7", "9V DC", "1=GND 2=+9V 3=NC" },
    { "E1", "EDGE CONNECTOR", "" },
};

static part_t *find(board_t *b, const char *ref) {
    int i = sim_find_part(&b->sim, ref);
    return i >= 0 ? &b->sim.parts[i] : NULL;
}

/* Turn a two pin part into one active element (diodes, transistors). */
static void make_element(board_t *b, const char *ref, int pin_a, int pin_b, int type) {
    part_t *p = find(b, ref);
    if (!p || part_pin(p, pin_a) < 0 || part_pin(p, pin_b) < 0) return;
    resistor_state_t *st = p->state;
    if (!st) p->state = st = calloc(1, sizeof(*st));
    p->kind = PART_RESISTOR;
    st->nel = 1;
    st->el[0] = (element_t){ part_pin(p, pin_a), part_pin(p, pin_b), EL_ANALOG };
    element_set_type(&b->sim, p, 0, type);
}

/* The edge connector: each pin's function is the name of its net. */
static void setup_edge(board_t *b) {
    sim_t *s = &b->sim;
    part_t *e1 = find(b, "E1");
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k, net = s->pins[pin].net;
        if (net < 0 || b->pin_func[pin]) continue;
        const char *n = s->nets[net].name;
        if (strncmp(n, "Net-(", 5) && strncmp(n, "unconnected", 11)) b->pin_func[pin] = n;
    }
}

static void init_keymap(zx48_t *z) {
    /* As floooh's zx.h: column = address line A8+n, line = KB0..KB4. */
    kbd_init(&z->kbd, 1);
    kbd_register_modifier(&z->kbd, 0, 0, 0);     /* caps shift */
    kbd_register_modifier(&z->kbd, 1, 7, 1);     /* symbol shift */
    const char *keymap =
        " zxcv" "asdfg" "qwert" "12345" "09876" "poiuy" " lkjh" "  mnb"
        " ZXCV" "ASDFG" "QWERT" "     " "     " "POIUY" " LKJH" "  MNB"
        " : ?/" "     " "   <>" "!@#$%" "_)('&" "\";   " " =+-^" "  .,*";
    for (int layer = 0; layer < 3; layer++)
        for (int column = 0; column < 8; column++)
            for (int line = 0; line < 5; line++) {
                int c = keymap[layer * 40 + column * 5 + line];
                if (c != ' ') kbd_register_key(&z->kbd, c, column, line, layer > 0 ? 1 << (layer - 1) : 0);
            }
    kbd_register_key(&z->kbd, ' ', 7, 0, 0);
    kbd_register_key(&z->kbd, 0x0F, 7, 1, 0);    /* symbol shift */
    kbd_register_key(&z->kbd, 0x0E, 0, 0, 0);    /* caps shift */
    kbd_register_key(&z->kbd, 0x08, 3, 4, 1);    /* cursor left: caps shift + 5 */
    kbd_register_key(&z->kbd, 0x0A, 4, 4, 1);    /* down: caps shift + 6 */
    kbd_register_key(&z->kbd, 0x0B, 4, 3, 1);    /* up: caps shift + 7 */
    kbd_register_key(&z->kbd, 0x09, 4, 2, 1);    /* right: caps shift + 8 */
    kbd_register_key(&z->kbd, 0x07, 3, 0, 1);    /* edit: caps shift + 1 */
    kbd_register_key(&z->kbd, 0x0C, 4, 0, 1);    /* delete: caps shift + 0 */
    kbd_register_key(&z->kbd, 0x03, 7, 0, 1);    /* break: caps shift + space */
    kbd_register_key(&z->kbd, 0x0D, 6, 0, 0);    /* enter */
}

static int pin_of(board_t *b, const char *ref, int number) {
    part_t *p = find(b, ref);
    return p ? part_pin(p, number) : -1;
}

static void zx_create(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    contention_log = getenv("ZXHW_CONTENTION") != NULL;
    const char *ul = getenv("ZXHW_ULALOG");
    if (ul && !ula_log_file) {
        char path[1024];
        snprintf(path, sizeof(path), "%s", ul);
        char *colon = strrchr(path, ':');
        if (colon) { ula_log_from = strtoull(colon + 1, NULL, 10); *colon = 0; }
        ula_log_file = fopen(path, "w");
    }
    const char *tr = getenv("ZXHW_TRACE");
    if (tr) {
        unsigned long long from = 0, count = 0;
        sscanf(tr, "%llu:%llu", &from, &count);
        trace_from = from;
        trace_to = from + count;
    }
    z->p_cpu = sim_find_part(s, "IC2");
    z->p_ula = sim_find_part(s, "IC1");
    z->p_rom = sim_find_part(s, "IC5");
    z->pin_clk = part_pin(&s->parts[z->p_cpu], Z_CLK);
    ((rom_state_t *)s->parts[z->p_rom].state)->data = zx48_rom;
    ((ula_state_t *)s->parts[z->p_ula].state)->fb = b->screen;
    /* Resistors and links that do logic. */
    for (size_t i = 0; i < sizeof(series) / sizeof(series[0]); i++) {
        part_t *p = find(b, series[i]);
        if (p && p->state && ((resistor_state_t *)p->state)->nel) element_set_type(s, p, 0, EL_SERIES);
    }
    part_t *r24 = find(b, "R24");                   /* /PHICPU to TR3's base */
    if (r24 && r24->state) ((resistor_state_t *)r24->state)->el[0].type = EL_LINK;
    /* RC delays, from pin 2 to pin 1: R32 and the inputs of the lower RAM
     * multiplexers (/RAS switches them to the column only after the 4116s
     * latched the row), R70/C64 and R71/C63 (the upper RAM timing). */
    static const char *const delays[] = { "R32", "R70", "R71" };
    for (size_t i = 0; i < 3; i++) {
        part_t *p = find(b, delays[i]);
        if (!p || !p->state) continue;
        element_t *e = &((resistor_state_t *)p->state)->el[0];
        e->pin_a = part_pin(p, 2);
        e->pin_b = part_pin(p, 1);
        element_set_type(s, p, 0, EL_DELAY);
    }
    for (size_t i = 0; i < sizeof(links_off) / sizeof(links_off[0]); i++) {
        part_t *p = find(b, links_off[i]);
        if (p && p->state) ((resistor_state_t *)p->state)->nel = 0;
    }
    part_t *jrom = find(b, "JROM1");
    if (jrom && jrom->state) {
        resistor_state_t *st = jrom->state;
        st->nel = 2;
        st->el[0] = (element_t){ part_pin(jrom, 1), part_pin(jrom, 2), EL_LINK };
        st->el[1] = (element_t){ part_pin(jrom, 3), part_pin(jrom, 4), EL_LINK };
    }
    /* The keyboard diodes (anode pin 2, cathode pin 1 on A8-A15), TR3 (the
     * clock inverter: base to collector) and TR6 (emitter follower: A0
     * keeps the ULA's /IOREQ high). */
    for (int i = 1; i <= 8; i++) {
        char ref[8];
        snprintf(ref, sizeof(ref), "D%d", i);
        make_element(b, ref, 2, 1, EL_DIODE);
    }
    make_element(b, "TR3", 2, 1, EL_NPN);
    make_element(b, "TR6", 2, 3, EL_FOLLOWER);
    /* Which DRAM holds which bit: the one whose D pin is on Dn (upper) or
     * on the ULA's Dn (lower). */
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        int net = part_pin(p, 2) >= 0 ? s->pins[part_pin(p, 2)].net : -1;
        const char *name = net >= 0 ? s->nets[net].name : "";
        if (p->reset == PT_4532.reset && p->commit == PT_4532.commit && name[0] == 'D')
            z->uram_part[name[1] - '0'] = i;
        if (p->commit == PT_4116.commit && !strncmp(name, "Net-(IC1-D", 10))
            z->lram_part[name[10] - '0'] = i;
    }
    /* Keyboard pins: the KB2 pad on the anode of the diode of each address
     * line, the KB1 pads 1-5. */
    for (int c = 0; c < 8; c++) {
        z->pin_kb_row[c] = -1;
        char an[8];
        snprintf(an, sizeof(an), "A%d", 8 + c);
        for (int i = 1; i <= 8; i++) {
            char ref[8];
            snprintf(ref, sizeof(ref), "D%d", i);
            int k = pin_of(b, ref, 1), a = pin_of(b, ref, 2);
            if (k < 0 || a < 0 || s->pins[k].net < 0 || strcmp(s->nets[s->pins[k].net].name, an)) continue;
            part_t *kb2 = find(b, "KB2");
            for (int j = 0; kb2 && j < kb2->npins; j++)
                if (s->pins[kb2->first_pin + j].net == s->pins[a].net && atoi(s->bd->pads[kb2->first_pin + j].name) < 10)
                    z->pin_kb_row[c] = kb2->first_pin + j;
        }
    }
    for (int l = 0; l < 5; l++) z->pin_kb_line[l] = pin_of(b, "KB1", l + 1);
    init_keymap(z);
    /* Buses, for the bus bitmap (speed only). */
    char name[32];
    int bit = 0;
    static const struct { const char *fmt; int n; } buses[] = {
        { "A%d", 16 }, { "D%d", 8 }, { "LA%d", 7 }, { "UA%d", 8 }, { "Net-(IC1-D%d)", 8 } };
    for (size_t k = 0; k < sizeof(buses) / sizeof(buses[0]); k++)
        for (int i = 0; i < buses[k].n; i++) {
            snprintf(name, sizeof(name), buses[k].fmt, i);
            sim_set_bus_bit(s, sim_find_net(s, name), bit++);
        }
    /* Power, reset, the RESET button (edge connector /RESET to GND). */
    z->pin_9v = pin_of(b, "J7", 2);
    z->pin_12v = pin_of(b, "D15", 1);
    z->pin_m5v = pin_of(b, "D12", 2);
    z->pin_reset_cap = pin_of(b, "C27", 1);
    part_t *e1 = find(b, "E1");
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k, net = s->pins[pin].net;
        if (net < 0) continue;
        if (!strcmp(s->nets[net].name, "/RESET") && b->pin_reset < 0) b->pin_reset = pin;
        if (!strcmp(s->nets[net].name, "GND") && b->pin_reset_gnd < 0) b->pin_reset_gnd = pin;
    }
    setup_edge(b);
}

static void zx_power_on(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    z->cpu_pins = z80_init(&z->cpu);
    z->in_reset = true;
    z->clk = 0;
    z->last_edge = 0;
    z->reset_until = ZX_FREQ * 7 / 100;     /* 70 ms: R31 (220K) charging C27 (1 uF) */
    memset(b->screen, 0, ULA_SCREEN_W * ULA_SCREEN_H * sizeof(uint32_t));
    sim_drive(s, z->pin_9v, DRIVE(S_SUPPLY, 1));     /* the 9 V input (see zx_power) */
}

/* The power supply, from what is broken. The 7805 makes +5V from the 9 V
 * input; the DC-DC converter (TR4, TR5 and the coil T1 oscillating) makes
 * +12V through D15 and -5V through the charge pump C46, D11, D12, R55 and
 * the zener D16. The 4116s need all three: without one they don't work
 * (and without -5V the real ones overheat and die). The crystal X1 is the
 * ULA's oscillator: without it there is no clock and no picture. */
static bool broken(board_t *b, const char *ref) {
    part_t *p = find(b, ref);
    return p && (p->removed || p->rfault != RF_OK);
}

static void zx_power(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    bool v5 = !broken(b, "REG1");
    bool osc = v5 && !broken(b, "TR4") && !broken(b, "TR5") && !broken(b, "T1") && !broken(b, "R58") &&
               !broken(b, "R59") && !broken(b, "R60") && !broken(b, "R61");
    bool p12 = osc && !broken(b, "D15");
    bool m5 = osc && !broken(b, "D11") && !broken(b, "D12") && !broken(b, "D16") && !broken(b, "R55");
    sim_drive(s, z->pin_12v, p12 ? DRIVE(S_SUPPLY, 1) : DRV_Z);
    sim_drive(s, z->pin_m5v, m5 ? DRIVE(S_SUPPLY, 0) : DRV_Z);
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if (p->kind != PART_IC) continue;
        p->no_supply = !v5 || (p->commit == PT_4116.commit && (!p12 || !m5));
    }
    ((ula_state_t *)s->parts[z->p_ula].state)->running = !broken(b, "X1");
}

/* Pressed keys: a key joins its half row (after the diode) and its line.
 * And the supplies (before the power state of the chips is computed). */
static void zx_joins(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    zx_power(b);
    for (int l = 0; l < 5; l++)
        for (int c = 0; c < 8; c++) {
            if (!((z->key_matrix[l] >> c) & 1)) continue;
            int r = z->pin_kb_row[c], k = z->pin_kb_line[l];
            if (r < 0 || k < 0 || s->pins[r].fault == PF_OPEN || s->pins[k].fault == PF_OPEN) continue;
            sim_join(s, s->pins[r].net, s->pins[k].net);
        }
}

static void sync_keys(board_t *b) {
    zx48_t *z = ZX(b);
    bool changed = false;
    for (int l = 0; l < 5; l++) {
        uint16_t m = z->kbd.scanout_column_masks[l];
        if (m != z->key_matrix[l]) { z->key_matrix[l] = m; changed = true; }
    }
    if (changed) board_rebuild(b);
}

static void zx_key_down(board_t *b, int key) { kbd_key_down(&ZX(b)->kbd, key); sync_keys(b); }
static void zx_key_up(board_t *b, int key) { kbd_key_up(&ZX(b)->kbd, key); sync_keys(b); }
static void zx_frame_update(board_t *b, uint32_t us) { kbd_update(&ZX(b)->kbd, us); sync_keys(b); }

static int zx_host_key(int keycode) {
    switch (keycode) {
    case SAPP_KEYCODE_ENTER: return 0x0D;
    case SAPP_KEYCODE_BACKSPACE: return 0x0C;
    case SAPP_KEYCODE_LEFT: return 0x08;
    case SAPP_KEYCODE_RIGHT: return 0x09;
    case SAPP_KEYCODE_UP: return 0x0B;
    case SAPP_KEYCODE_DOWN: return 0x0A;
    case SAPP_KEYCODE_ESCAPE: return 0x03;
    case SAPP_KEYCODE_TAB: return 0x0F;
    case SAPP_KEYCODE_LEFT_SHIFT: return 0x0E;
    default: return 0;
    }
}

static int zx_host_char(int c) { return c; }

static void zx_monitor(board_t *b, uint32_t *out) {
    zx48_t *z = ZX(b);
    part_t *ula = &b->sim.parts[z->p_ula];
    static uint32_t seed = 12345;
    for (int i = 0; i < ULA_SCREEN_W * ULA_SCREEN_H; i++) {
        uint32_t c = b->screen[i];
        if (ula->dead) {    /* no video signal: snow */
            seed = seed * 1103515245u + 12345u;
            uint32_t g = ((seed >> 24) & 0xFF) * 3 / 4;
            c = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
        out[i] = c;
    }
}

/* ------------------------------------------------------------------------ */
/* Loading snapshots                                                         */
/* ------------------------------------------------------------------------ */

/* Which CPU address bit a net carries, following links ("A14" joined to
 * a mux input by the TI1 link): 0..15, or -2 for GND, -3 for +5V. */
static int addr_bit_of_net(sim_t *s, int net) {
    int root = s->nets[net].root;
    for (int i = 0; i < s->nnets; i++) {
        if (s->nets[i].root != root) continue;
        const char *n = s->nets[i].name;
        if (n[0] == 'A' && n[1] >= '0' && n[1] <= '9') return atoi(n + 1);
        if (!strcmp(n, "GND")) return -2;
        if (!strcmp(n, "+5V")) return -3;
    }
    return -1;
}

/* The row and column address bit a DRAM address pin gets, from the board:
 * its net comes from a 74LS157 output (maybe through a series resistor);
 * that section's B input is the row (the mux select is high before /RAS
 * falls), its A input the column. */
static void dram_pin_bits(board_t *b, int pin, int *row, int *col) {
    sim_t *s = &b->sim;
    static const int sec[4][3] = { {4, 2, 3}, {7, 5, 6}, {9, 11, 10}, {12, 14, 13} };   /* Y, A, B */
    *row = *col = -1;
    int net = s->pins[pin].net;
    for (int hop = 0; hop < 2 && net >= 0; hop++) {
        int next = -1;
        for (int i = 0; i < s->npins; i++) {
            if (s->pins[i].net != net) continue;
            part_t *p = &s->parts[s->pins[i].part];
            if (p->eval == PT_74LS157.eval) {
                for (int k = 0; k < 4; k++)
                    if (part_pin(p, sec[k][0]) == i) {
                        *col = addr_bit_of_net(s, s->pins[part_pin(p, sec[k][1])].net);
                        *row = addr_bit_of_net(s, s->pins[part_pin(p, sec[k][2])].net);
                        return;
                    }
            }
            if (p->kind == PART_RESISTOR && p->state && ((resistor_state_t *)p->state)->nel) {
                element_t *e = &((resistor_state_t *)p->state)->el[0];
                if (e->type == EL_SERIES) next = s->pins[e->pin_a == i ? e->pin_b : e->pin_a].net;
            }
        }
        net = next;
    }
}

/* The DRAM cell of a CPU address, as the board wires it. */
static int dram_cell(board_t *b, int part, uint16_t addr) {
    static const int addr_pins[8] = { 5, 7, 6, 12, 11, 10, 13, 9 };
    part_t *p = &b->sim.parts[part];
    int bits = p->commit == PT_4116.commit ? 7 : 8, row = 0, col = 0;
    for (int i = 0; i < bits; i++) {
        int rb, cb;
        dram_pin_bits(b, part_pin(p, addr_pins[i]), &rb, &cb);
        int r = rb >= 0 ? (addr >> rb) & 1 : rb == -3, c = cb >= 0 ? (addr >> cb) & 1 : cb == -3;
        row |= r << i;
        col |= c << i;
    }
    return (row << 8) | col;
}

/* Write a byte straight into the DRAM chips (a backdoor, like the C64's
 * program loading: it works even on a broken board). The cell of each
 * address is computed once: the wiring doesn't change. */
static void ram_poke(board_t *b, uint16_t addr, uint8_t v) {
    zx48_t *z = ZX(b);
    static int *cells[2];
    static const board_t *cells_board;
    if (addr < 0x4000) return;
    int upper = addr >= 0x8000;
    if (cells_board != b) {
        for (int u = 0; u < 2; u++) {
            free(cells[u]);
            cells[u] = calloc(u ? 32768 : 16384, sizeof(int));
            for (int a = 0; a < (u ? 32768 : 16384); a++)
                cells[u][a] = dram_cell(b, u ? z->uram_part[0] : z->lram_part[0], (uint16_t)((u ? 0x8000 : 0x4000) + a));
        }
        cells_board = b;
    }
    int cell = cells[upper][addr - (upper ? 0x8000 : 0x4000)];
    for (int bit = 0; bit < 8; bit++) {
        dram_state_t *st = b->sim.parts[upper ? z->uram_part[bit] : z->lram_part[bit]].state;
        st->cells[cell] = (v >> bit) & 1;
    }
}

static uint8_t ram_peek(board_t *b, uint16_t addr) {
    zx48_t *z = ZX(b);
    if (addr < 0x4000) return zx48_rom[addr];
    int upper = addr >= 0x8000;
    int cell = dram_cell(b, upper ? z->uram_part[0] : z->lram_part[0], addr);
    uint8_t v = 0;
    for (int bit = 0; bit < 8; bit++)
        v |= (uint8_t)(((dram_state_t *)b->sim.parts[upper ? z->uram_part[bit] : z->lram_part[bit]].state)->cells[cell] << bit);
    return v;
}

/* Start the CPU at pc with the registers of a snapshot. */
static void cpu_start(board_t *b, uint16_t pc) {
    zx48_t *z = ZX(b);
    z->cpu_pins = z80_prefetch(&z->cpu, pc);
    sim_mark_dirty(&b->sim, z->p_cpu);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* .sna (48K): 27 bytes of registers, then the RAM from 0x4000; PC is on
 * the stack (the snapshot was taken inside an interrupt). */
static bool load_sna(board_t *b, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx48_t *z = ZX(b);
    if (n != 27 + 49152) { snprintf(msg, msglen, "not a 48K .sna (%zu bytes)", n); return false; }
    for (int a = 0; a < 49152; a++) ram_poke(b, (uint16_t)(0x4000 + a), d[27 + a]);
    z80_t *c = &z->cpu;
    c->i = d[0];
    c->hl2 = le16(d + 1); c->de2 = le16(d + 3); c->bc2 = le16(d + 5); c->af2 = le16(d + 7);
    c->hl = le16(d + 9); c->de = le16(d + 11); c->bc = le16(d + 13);
    c->iy = le16(d + 15); c->ix = le16(d + 17);
    c->iff1 = c->iff2 = (d[19] & 4) != 0;
    c->r = d[20];
    c->af = le16(d + 21);
    uint16_t sp = le16(d + 23);
    c->im = d[25] & 3;
    ((ula_state_t *)b->sim.parts[z->p_ula].state)->border = d[26] & 7;
    uint16_t pc = (uint16_t)(ram_peek(b, sp) | ram_peek(b, (uint16_t)(sp + 1)) << 8);
    c->sp = (uint16_t)(sp + 2);
    cpu_start(b, pc);
    return true;
}

/* .z80, versions 1 to 3, 48K: a header, then the memory, maybe compressed
 * (ED ED count byte). */
static size_t z80_unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t max, bool v1) {
    size_t i = 0, o = 0;
    while (i < n && o < max) {
        if (v1 && i + 3 < n && src[i] == 0 && src[i + 1] == 0xED && src[i + 2] == 0xED && src[i + 3] == 0) break;
        if (i + 3 < n && src[i] == 0xED && src[i + 1] == 0xED) {
            for (int k = 0; k < src[i + 2] && o < max; k++) dst[o++] = src[i + 3];
            i += 4;
        } else {
            dst[o++] = src[i++];
        }
    }
    return o;
}

static bool load_z80(board_t *b, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx48_t *z = ZX(b);
    if (n < 30) { snprintf(msg, msglen, "too short for a .z80"); return false; }
    static uint8_t mem[49152];
    uint16_t pc = le16(d + 6);
    uint8_t flags = d[12] == 255 ? 1 : d[12];
    if (pc) {           /* version 1: the 48K follow */
        size_t got = (flags & 0x20) ? z80_unpack(d + 30, n - 30, mem, sizeof(mem), true)
                                    : (n - 30 < sizeof(mem) ? n - 30 : sizeof(mem));
        if (!(flags & 0x20)) memcpy(mem, d + 30, got);
        if (got != sizeof(mem)) { snprintf(msg, msglen, ".z80: memory is %zu bytes, not 48K", got); return false; }
    } else {            /* versions 2 and 3: pages of 16K */
        size_t hlen = le16(d + 30), at = 32 + hlen;
        pc = le16(d + 32);
        int hw = d[34];
        bool v2 = hlen == 23;
        if ((v2 && hw > 1) || (!v2 && hw != 0 && hw != 1 && hw != 3)) {
            snprintf(msg, msglen, ".z80 for another machine (hardware %d): only the 48K", hw);
            return false;
        }
        int pages = 0;
        while (at + 3 <= n) {
            size_t len = le16(d + at);
            int page = d[at + 2];
            at += 3;
            size_t in = len == 0xFFFF ? 16384 : len;
            if (at + in > n) break;
            int base = page == 8 ? 0 : page == 4 ? 16384 : page == 5 ? 32768 : -1;
            if (base >= 0) {
                if (len == 0xFFFF) memcpy(mem + base, d + at, 16384);
                else z80_unpack(d + at, in, mem + base, 16384, false);
                pages++;
            }
            at += in;
        }
        if (pages != 3) { snprintf(msg, msglen, ".z80: %d pages of the 48K found, not 3", pages); return false; }
    }
    for (int a = 0; a < 49152; a++) ram_poke(b, (uint16_t)(0x4000 + a), mem[a]);
    z80_t *c = &z->cpu;
    c->a = d[0]; c->f = d[1]; c->bc = le16(d + 2); c->hl = le16(d + 4); c->sp = le16(d + 8);
    c->i = d[10]; c->r = (uint8_t)((d[11] & 0x7F) | ((flags & 1) << 7));
    c->de = le16(d + 13); c->bc2 = le16(d + 15); c->de2 = le16(d + 17); c->hl2 = le16(d + 19);
    c->af2 = (uint16_t)(d[21] << 8 | d[22]);
    c->iy = le16(d + 23); c->ix = le16(d + 25);
    c->iff1 = d[27] != 0; c->iff2 = d[28] != 0;
    c->im = d[29] & 3;
    ((ula_state_t *)b->sim.parts[z->p_ula].state)->border = (flags >> 1) & 7;
    cpu_start(b, pc);
    return true;
}

/* ------------------------------------------------------------------------ */
/* ROM cartridges                                                            */
/* ------------------------------------------------------------------------ */

/* A 16K ROM in the edge connector, like the Interface 2 cartridges and the
 * diagnostic ROM boards: it holds /ROMCS high (stronger than the ULA,
 * which drives it through R33) so the internal ROM stays quiet, and puts
 * its own byte on the data bus for memory reads below 0x4000. Its pins are
 * the edge connector's: a bad contact there is a bad contact for it. */
typedef struct {
    char name[64];
    uint8_t rom[16384];
    int pin_romcs, pin_mreq, pin_rd, pin_a[16], pin_d[8];
} zxcart_t;

static void zxcart_eval(sim_t *s, part_t *p) {
    zxcart_t *c = p->state;
    bool sel = !sim_read(s, c->pin_mreq) && !sim_read(s, c->pin_rd) && !sim_read(s, c->pin_a[14]) &&
               !sim_read(s, c->pin_a[15]);
    unsigned a = 0;
    for (int i = 0; sel && i < 14; i++) a |= (unsigned)sim_read(s, c->pin_a[i]) << i;
    for (int i = 0; i < 8; i++) sim_drive(s, c->pin_d[i], sel ? DRV_BIT((c->rom[a] >> i) & 1) : DRV_Z);
    sim_drive(s, c->pin_romcs, DRV_HI);
}

/* The edge connector pin on a net, or -1. */
static int edge_pin(board_t *b, const char *net) {
    sim_t *s = &b->sim;
    part_t *e1 = find(b, "E1");
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k;
        if (s->pins[pin].net >= 0 && !strcmp(s->nets[s->pins[pin].net].name, net)) return pin;
    }
    return -1;
}

static void cart_pins(board_t *b, zxcart_t *c, bool plugged) {
    sim_t *s = &b->sim;
    int ins[18] = { c->pin_mreq, c->pin_rd }, nin = 2;
    for (int i = 0; i < 16; i++) ins[nin++] = c->pin_a[i];
    for (int i = 0; i < nin; i++) if (ins[i] >= 0) s->pins[ins[i]].flags = plugged ? PIN_IN | PIN_SENSE : 0;
    int outs[9] = { c->pin_romcs };
    for (int i = 0; i < 8; i++) outs[1 + i] = c->pin_d[i];
    for (int i = 0; i < 9; i++) if (outs[i] >= 0) { s->pins[outs[i]].flags = plugged ? PIN_OUT : 0; s->pins[outs[i]].drive = DRV_Z; }
}

static void zx_remove_cart(board_t *b) {
    zx48_t *z = ZX(b);
    part_t *e1 = find(b, "E1");
    if (!z->cart || !e1) return;
    cart_pins(b, z->cart, false);
    free(z->cart);
    z->cart = e1->state = NULL;
    e1->eval = NULL;
    board_rebuild(b);
    board_power_on(b);
}

static const char *zx_cart_name(board_t *b) { return ZX(b)->cart ? ((zxcart_t *)ZX(b)->cart)->name : NULL; }

static bool plug_rom(board_t *b, const char *path, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx48_t *z = ZX(b);
    part_t *e1 = find(b, "E1");
    if (n != 16384 || !e1) { snprintf(msg, msglen, "a ROM cartridge is 16384 bytes, this is %zu", n); return false; }
    zx_remove_cart(b);
    zxcart_t *c = calloc(1, sizeof(*c));
    const char *base = strrchr(path, '/');
    snprintf(c->name, sizeof(c->name), "%s", base ? base + 1 : path);
    memcpy(c->rom, d, 16384);
    c->pin_romcs = edge_pin(b, "/ROMCS");
    c->pin_mreq = edge_pin(b, "/MREQ");
    c->pin_rd = edge_pin(b, "/RD");
    char name[8];
    for (int i = 0; i < 16; i++) { snprintf(name, sizeof(name), "A%d", i); c->pin_a[i] = edge_pin(b, name); }
    for (int i = 0; i < 8; i++) { snprintf(name, sizeof(name), "D%d", i); c->pin_d[i] = edge_pin(b, name); }
    cart_pins(b, c, true);
    z->cart = e1->state = c;
    e1->eval = zxcart_eval;
    /* In with the power off: switch the Spectrum on again. */
    board_rebuild(b);
    board_power_on(b);
    snprintf(msg, msglen, "ROM cartridge %s plugged into the edge connector", c->name);
    return true;
}

/* Snapshots: what the header says. */
static const file_kind_t zx_snapshot = { "snapshot", "Load the snapshot", false, -1 };
static const file_kind_t zx_rom = { "ROM cartridge", "Plug the ROM", false, ROLE_ROM };

static const file_kind_t *zx_file_kind(const char *path, char *info, int len, bool *bad) {
    size_t pl = strlen(path);
    bool sna = pl > 4 && !strcasecmp(path + pl - 4, ".sna"), z80 = pl > 4 && !strcasecmp(path + pl - 4, ".z80");
    bool rom = pl > 4 && !strcasecmp(path + pl - 4, ".rom");
    if (!sna && !z80 && !rom) return NULL;
    FILE *f = fopen(path, "rb");
    uint8_t h[40] = {0};
    size_t n = f ? fread(h, 1, sizeof(h), f) : 0;
    long size = 0;
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    *bad = false;
    if (rom) {
        if (size == 16384) snprintf(info, len, "16K ROM for the edge connector");
        else { snprintf(info, len, "%ld bytes: not a 16K ROM", size); *bad = true; }
        return &zx_rom;
    }
    if (sna) {
        if (size == 27 + 49152) snprintf(info, len, "48K snapshot");
        else { snprintf(info, len, "%ld bytes: not a 48K snapshot", size); *bad = true; }
        return &zx_snapshot;
    }
    if (n < 30) { snprintf(info, len, "too short for a snapshot"); *bad = true; return &zx_snapshot; }
    if (h[6] | h[7]) { snprintf(info, len, "48K snapshot (version 1)"); return &zx_snapshot; }
    unsigned hlen = (unsigned)(h[30] | h[31] << 8), hw = h[34];
    bool v2 = hlen == 23, ok = v2 ? hw <= 1 : hw == 0 || hw == 1 || hw == 3;
    if (ok) snprintf(info, len, "48K snapshot (version %d)", v2 ? 2 : 3);
    else { snprintf(info, len, "for another model (hardware %u): only the 48K", hw); *bad = true; }
    return &zx_snapshot;
}

static bool zx_load(board_t *b, const char *path, bool run, char *msg, int msglen) {
    (void)run;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return false; }
    static uint8_t buf[256 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(path);
    bool ok;
    if (len > 4 && !strcasecmp(path + len - 4, ".rom")) return plug_rom(b, path, buf, n, msg, msglen);
    if (len > 4 && !strcasecmp(path + len - 4, ".sna")) ok = load_sna(b, buf, n, msg, msglen);
    else if (len > 4 && !strcasecmp(path + len - 4, ".z80")) ok = load_z80(b, buf, n, msg, msglen);
    else { snprintf(msg, msglen, "%s: the Spectrum takes .sna and .z80 snapshots, .rom cartridges", path); return false; }
    if (ok) snprintf(msg, msglen, "snapshot %s loaded", path);
    (void)base;
    return ok;
}

static const char *zx_cpu_status(board_t *b) {
    zx48_t *z = ZX(b);
    return !z->cpu_running ? "stopped (no clock)" : z->in_reset ? "in reset" : "running";
}

static void zx_cpu_regs(board_t *b, char *buf, int len) {
    zx48_t *z = ZX(b);
    snprintf(buf, len, "PC=%04X AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X IR=%04X IM%d %s", z->cpu.pc,
             z->cpu.af, z->cpu.bc, z->cpu.de, z->cpu.hl, z->cpu.sp, z->cpu.ir, z->cpu.im,
             !z->cpu_running ? "(no clock)" : z->in_reset ? "(in reset)" : "");
}

/* The text on the picture, matched against the ROM's character set (at
 * 0x3D00, characters 32-127), cell by cell like a person reading it. */
static void zx_print_text(board_t *b) {
    const uint32_t *px = b->screen;
    for (int row = 0; row < 24; row++) {
        char line[64];
        int n = 0;
        for (int col = 0; col < 32; col++) {
            int x0 = 48 + col * 8, y0 = 48 + row * 8;
            uint32_t bg = px[y0 * ULA_SCREEN_W + x0];
            uint8_t cell[8];
            for (int y = 0; y < 8; y++) {
                cell[y] = 0;
                for (int x = 0; x < 8; x++)
                    if (px[(y0 + y) * ULA_SCREEN_W + x0 + x] != bg) cell[y] |= (uint8_t)(0x80 >> x);
            }
            int ch = '?';
            for (int c = 32; c < 128; c++) {
                const uint8_t *g = &zx48_rom[0x3D00 + (c - 32) * 8];
                bool same = true, inv = true;
                for (int y = 0; y < 8; y++) {
                    if (g[y] != cell[y]) same = false;
                    if ((uint8_t)~g[y] != cell[y]) inv = false;
                }
                if (same || inv) { ch = c; break; }
            }
            if (ch == 0x7F) { line[n++] = '('; line[n++] = 'c'; ch = ')'; }    /* copyright sign */
            line[n++] = (char)ch;
        }
        line[n] = 0;
        while (n > 0 && line[n - 1] == ' ') line[--n] = 0;
        printf("|%s\n", line);
    }
}

const machine_t machine_zx48 = {
    .id = "zx48",
    .name = "ZX Spectrum 48K",
    .board_name = "ZX Spectrum 48K Issue 3B",
    .size = sizeof(zx48_t),
    .data = &bd_zx48,
    .sim = { .rail_of = zx_rail_of, .kind_of = zx_kind_of, .cycles_per_us = 3.5 },
    .cpu_hz = ZX_FREQ,
    .cycles_per_frame = ZX_CYCLES_PER_FRAME,
    .steps_per_cycle = ZX_STEPS_PER_CYCLE,
    .screen_w = ULA_SCREEN_W, .screen_h = ULA_SCREEN_H,
    .ready_cycles = 3 * ZX_FREQ,
    .parts = part_defs, .nparts = sizeof(part_defs) / sizeof(part_defs[0]),
    .conns = conn_defs, .nconns = sizeof(conn_defs) / sizeof(conn_defs[0]),
    .link_resistors = "",
    .crystals = "X1 REG1 TR4 TR5 T1 D11 D12 D15 D16 R55 R58 R59 R60 R61",
    .keys_help = "Keys: ESC=BREAK  TAB=SYMBOL SHIFT  left SHIFT=CAPS SHIFT",
    .reset_help = "Hold to press RESET: shorts /RESET to GND on the edge connector",
    .create = zx_create,
    .power_on = zx_power_on,
    .run = zx_run,
    .joins = zx_joins,
    .key_down = zx_key_down,
    .key_up = zx_key_up,
    .frame_update = zx_frame_update,
    .host_key = zx_host_key,
    .host_char = zx_host_char,
    .monitor = zx_monitor,
    .files_title = "Open a snapshot (.sna, .z80) or a ROM (.rom)",
    .cart_slot = "E1",
    .cart_name = zx_cart_name,
    .remove_cart = zx_remove_cart,
    .file_kind = zx_file_kind,
    .load = zx_load,
    .cpu_status = zx_cpu_status,
    .cpu_regs = zx_cpu_regs,
    .print_text = zx_print_text,
};
