/* board.c - the C64 ASSY 250407 board.
 *
 * HOW A BUS CYCLE IS SIMULATED
 *
 * A C64 cycle (about 1 microsecond) has two halves. In the first half (phi1,
 * PHI0 low) the VIC-II reads memory; in the second half (phi2, PHI0 high) the
 * CPU does, unless the VIC "steals" it (badlines, sprites). In both halves
 * the VIC runs a DRAM cycle: it sets the row address, pulls /RAS low, then
 * switches to the column address and pulls /CAS low. We simulate each half
 * in three steps, letting the whole netlist settle after each one:
 *
 *   STEP_ROW  /RAS and /CAS high, row address on the multiplexed lines
 *   STEP_RAS  /RAS falls: the DRAMs and the 74LS373 latch the row
 *   STEP_CAS  /CAS falls: column address, the PLA asserts /CASRAM if the
 *             DRAM is selected, ROMs and RAMs put data on the bus
 *
 * At the end of phi2 the chips with a core (VIC registers, CIAs, SID) see
 * the bus, perform register reads/writes and advance one clock; finally the
 * CPU samples the data bus. Nothing here knows the C64 memory map: chip
 * selects, banking, the address multiplexing, everything comes from the
 * netlist and the glue chip models. */
#include "board.h"
#include "board_data.h"
#include "c64-roms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pin numbers of the big chips, from their datasheets. */
static const int cpu_addr_pins[16] = { 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23 };
static const int cpu_data_pins[8] = { 37, 36, 35, 34, 33, 32, 31, 30 };
static const int vic_mux_pins[6] = { 24, 25, 26, 27, 28, 29 };    /* A0/A8 .. A5/A13 */
static const int vic_data_pins[12] = { 7, 6, 5, 4, 3, 2, 1, 39, 38, 37, 36, 35 };
static const int cia_data_pins[8] = { 33, 32, 31, 30, 29, 28, 27, 26 };
static const int cia_rs_pins[4] = { 38, 37, 36, 35 };
static const int sid_addr_pins[5] = { 9, 10, 11, 12, 13 };
static const int sid_data_pins[8] = { 15, 16, 17, 18, 19, 20, 21, 22 };

enum { DOUT_VIC, DOUT_CIA1, DOUT_CIA2, DOUT_SID };

static part_t *P(board_t *b, int idx) { return &b->sim.parts[idx]; }

/* The part that owns a register read data slot. */
static int dout_part(board_t *b, int i) {
    if (i == DOUT_VIC) return b->p_vic;
    if (i == DOUT_SID) return b->p_sid;
    return b->p_cia[i - DOUT_CIA1];
}

/* ------------------------------------------------------------------------ */
/* 6510 CPU (U7)                                                             */
/* ------------------------------------------------------------------------ */

/* The CPU core runs one cycle at the start of each bus cycle: it consumes
 * the data it sampled at the end of the previous phi2 and produces the next
 * address. Its pins are driven by cpu_eval() during phi2. */
static void cpu_tick(board_t *b) {
    sim_t *s = &b->sim;
    part_t *p = P(b, b->p_cpu);
    b->cpu_ticked = false;
    /* No PHI0 clock from the VIC: the CPU is frozen. */
    b->cpu_running = !p->dead && b->seen_phi0[0] == 0 && b->seen_phi0[1] == 1;
    if (!b->cpu_running) return;
    /* /RES is a level: the CPU is held while it is low, and runs its reset
     * sequence when it goes high. */
    if (!pin_in(s, p, 40)) { b->cpu_in_reset = true; return; }
    if (b->cpu_in_reset) {
        b->cpu_in_reset = false;
        b->cpu_pins |= M6502_RES;
        b->cpu.io_ddr = 0;          /* the 6510 port comes out of reset as input */
        b->cpu.io_out = 0;
    }
    uint64_t pins = b->cpu_pins & ~(M6502_IRQ | M6502_NMI | M6502_RDY);
    if (!pin_in(s, p, 3)) pins |= M6502_IRQ;     /* /IRQ low */
    if (!pin_in(s, p, 4)) pins |= M6502_NMI;     /* /NMI low (edge detected by the core) */
    if (!pin_in(s, p, 2)) pins |= M6502_RDY;     /* RDY low: halt on the next read */
    b->cpu_pins = m6502_tick(&b->cpu, pins);
    b->cpu_ticked = true;
    sim_mark_dirty(s, b->p_cpu);
}

static void cpu_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    /* PHI2 output follows the PHI0 input: it clocks the CIAs and the SID. */
    pin_out(s, p, 39, DRV_BIT(pin_in(s, p, 1)));
    /* I/O port P0..P5 (pins 29..24): driven where the DDR says output. */
    int32_t port_key = (b->cpu.io_ddr << 8) | b->cpu.io_out;
    if (b->cache_cpu_port != port_key + 1) {
        b->cache_cpu_port = port_key + 1;
        for (int i = 0; i < 6; i++) {
            bool out = (b->cpu.io_ddr >> i) & 1;
            pin_out(s, p, 29 - i, out ? DRV_BIT((b->cpu.io_out >> i) & 1) : DRV_Z);
        }
    }
    /* Address, R/W and data only while AEC is high: when the VIC owns the
     * bus the 6510 puts its outputs in high impedance. */
    bool bus = pin_in(s, p, 5);
    uint64_t pins = b->cpu_pins;
    uint16_t addr = M6502_GET_ADDR(pins);
    bool write = !(pins & M6502_RW);
    part_drive_bus_cached(s, p, cpu_addr_pins, 16, addr, bus, &b->cache_cpu_addr);
    pin_out(s, p, 38, bus ? DRV_BIT(!write) : DRV_Z);
    /* Writes to $00/$01 go to the internal port: the 6510 doesn't drive the
     * data bus, so the RAM below gets whatever charge is on the bus. */
    part_drive_bus_cached(s, p, cpu_data_pins, 8, M6502_GET_DATA(pins), bus && write && addr > 1,
                          &b->cache_cpu_data);
}

/* Self-check oracle, for tests only (C64HW_CHECK=1): on a healthy board
 * the CPU must read the KERNAL exactly, and the VIC must see in bank 0 the
 * character ROM at $1000-$1FFF and the DRAM contents elsewhere. This code
 * knows the memory map; the simulation itself doesn't use it. */
static uint8_t dram_peek(board_t *b, uint16_t addr) {
    int cell = ((addr & 0xFF) << 8) | (addr >> 8);
    uint8_t v = 0;
    for (int bit = 0; bit < 8; bit++)
        v |= (uint8_t)(((dram_state_t *)b->sim.parts[b->dram_part[bit]].state)->cells[cell] << bit);
    return v;
}

static void check_report(board_t *b, const char *what, uint16_t addr, uint8_t got, uint8_t want) {
    if (b->check_errors++ < 10)
        printf("CHECK: %s $%04X read $%02X, expected $%02X (cycle %llu)\n", what, addr, got, want,
               (unsigned long long)b->sim.cycle);
}

static void check_cpu_read(board_t *b, uint64_t pins) {
    uint16_t a = M6502_GET_ADDR(pins);
    if (!(pins & M6502_RW) || a < 0xE000 || (b->cpu.io_out & b->cpu.io_ddr & 2) == 0) return;
    uint8_t want = dump_c64_kernalv3_bin[a - 0xE000];
    if (M6502_GET_DATA(pins) != want) check_report(b, "CPU", a, M6502_GET_DATA(pins), want);
}

static void check_vic_fetch(board_t *b, uint16_t a, uint8_t got) {
    uint8_t want = (a & 0x3000) == 0x1000 ? dump_c64_char_bin[a & 0xFFF] : dram_peek(b, a);
    if (got != want) check_report(b, "VIC", a, got, want);
}

/* End of phi2: the CPU latches the data bus. */
static void cpu_sample(board_t *b) {
    if (!b->cpu_ticked) return;
    sim_t *s = &b->sim;
    uint64_t pins = b->cpu_pins;
    if (pins & M6502_RW) {
        uint8_t data = (uint8_t)part_read_bus(s, P(b, b->p_cpu), cpu_data_pins, 8);
        M6502_SET_DATA(pins, data);
    }
    if (M6510_CHECK_IO(pins)) pins = m6510_iorq(&b->cpu, pins);
    if (b->check && !b->vic_steal) check_cpu_read(b, pins);
    b->cpu_pins = pins;
    sim_mark_dirty(s, b->p_cpu);
}

/* The 6510 port reads the real pin levels (pull-ups on LORAM/HIRAM/CHAREN
 * and cassette sense make them read 1 when not driven). */
static uint8_t cpu_port_in(void *user) {
    board_t *b = user;
    sim_t *s = &b->sim;
    uint8_t v = 0;
    for (int i = 0; i < 6; i++) v |= (uint8_t)(pin_in(s, P(b, b->p_cpu), 29 - i) << i);
    return v;
}
static void cpu_port_out(uint8_t data, void *user) { (void)data; (void)user; }

/* ------------------------------------------------------------------------ */
/* Bus half cycles                                                           */
/* ------------------------------------------------------------------------ */

static void scope_and_stats(board_t *b) {
    sim_t *s = &b->sim;
    if (b->scope_pin >= 0) {
        const pin_t *pin = &s->pins[b->scope_pin];
        uint8_t v = 0;
        if (pin->root >= 0) {
            const net_t *n = &s->nets[pin->root];
            v = n->level ? SC_LEVEL : 0;
            if (n->strength == S_Z) v |= SC_FLOATING;
            if (n->contention) v |= SC_FIGHT;
        }
        b->scope[b->scope_pos++ & (SCOPE_SAMPLES - 1)] = v;
    }
    /* Logic probe statistics: sample every net once in a while. */
    if (--b->st_countdown > 0) return;
    b->st_countdown = 97;
    /* Inputs reading an erratic level (a floating net near the threshold, a
     * lifted MOS leg) can change without any event: look at them again. */
    sim_wake_erratic(s);
    for (int i = 0; i < s->nnets; i++) {
        const net_t *n = &s->nets[s->nets[i].root];
        if (n->contention) b->st_fight[i]++;
        else if (n->strength == S_Z) b->st_float[i]++;
        else if (n->level) b->st_hi[i]++;
        else b->st_lo[i]++;
    }
}

static void bus_step(board_t *b, int step) {
    sim_t *s = &b->sim;
    b->step = step;
    s->clock_level = b->phase == 2;
    /* The VIC drives /RAS, /CAS and the address; the clock oscillator
     * changes level with the phase. */
    sim_mark_dirty(s, b->p_vic);
    if (step == STEP_ROW) sim_mark_dirty(s, b->p_u31);
    sim_settle(s);
    sim_commit(s);      /* latches and memories see the edge */
    sim_settle(s);      /* and their outputs propagate */
    scope_and_stats(b);
}

static void bus_half(board_t *b, int phase, bool vic_owns, uint16_t vic_addr) {
    sim_t *s = &b->sim;
    b->phase = phase;
    b->vic_owns = vic_owns;
    b->vic_addr = vic_addr;
    /* Register read data of the previous phi2 leaves the bus. */
    for (int i = 0; i < 4; i++) {
        if (!b->dout_en[i]) continue;
        b->dout_en[i] = false;
        sim_mark_dirty(s, dout_part(b, i));
    }
    bus_step(b, STEP_ROW);
    bus_step(b, STEP_RAS);
    bus_step(b, STEP_CAS);
    /* Clock detection: remember the clock pin levels in this half. */
    int h = phase - 1;
    b->seen_phi0[h] = (uint8_t)pin_in(s, P(b, b->p_cpu), 1);
    b->seen_dot[h] = (uint8_t)pin_in(s, P(b, b->p_vic), 22);
    b->seen_color[h] = (uint8_t)pin_in(s, P(b, b->p_vic), 21);
    b->seen_phi2[0][h] = (uint8_t)pin_in(s, P(b, b->p_cia[0]), 25);
    b->seen_phi2[1][h] = (uint8_t)pin_in(s, P(b, b->p_cia[1]), 25);
    b->seen_phi2[2][h] = (uint8_t)pin_in(s, P(b, b->p_sid), 6);
}

static void set_dout(board_t *b, int idx, int part, uint8_t val) {
    b->dout_en[idx] = true;
    b->dout_val[idx] = val;
    sim_mark_dirty(&b->sim, part);
}

/* ------------------------------------------------------------------------ */
/* 6569 VIC-II (U19)                                                         */
/* ------------------------------------------------------------------------ */

/* Memory fetch callback of the VIC core: every fetch is a real bus half. */
static uint16_t vic_fetch(uint16_t addr, void *user) {
    board_t *b = user;
    int phase = (b->vic_fetches > 0 && b->vic_steal) ? 2 : 1;
    b->vic_fetches++;
    bus_half(b, phase, true, addr & 0x3FFF);
    /* D0-D7 from the main bus, D8-D11 from the color RAM. */
    static const int vic_color_pins[4] = { 38, 37, 36, 35 };     /* D8-D11 */
    uint16_t data = (uint16_t)(part_read_bus(&b->sim, P(b, b->p_vic), vic_data_pins, 8) |
                               part_read_bus(&b->sim, P(b, b->p_vic), vic_color_pins, 4) << 8);
    if (b->check) check_vic_fetch(b, addr & 0x3FFF, data & 0xFF);
    return data;
}

static void vic_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    bool run = b->vic_running;
    uint16_t a = b->vic_addr;
    bool own = run && b->vic_owns;
    pin_out(s, p, 17, run ? DRV_BIT(b->phase == 2) : DRV_Z);          /* PHI0 */
    pin_out(s, p, 18, run ? DRV_BIT(b->step == STEP_ROW) : DRV_Z);    /* /RAS */
    pin_out(s, p, 19, run ? DRV_BIT(b->step != STEP_CAS) : DRV_Z);    /* /CAS */
    pin_out(s, p, 16, run ? DRV_BIT(!b->vic_owns) : DRV_Z);           /* AEC */
    pin_out(s, p, 12, run ? DRV_BIT(!b->vic_ba_low) : DRV_Z);         /* BA */
    pin_out(s, p, 8, run && b->vic_irq ? DRV_LO : DRV_Z);             /* /IRQ, open drain */
    /* Multiplexed address: row (A0-A5) then column (A8-A13). */
    unsigned mux = b->step == STEP_CAS ? (a >> 8) & 63 : a & 63;
    part_drive_bus_cached(s, p, vic_mux_pins, 6, mux, own, &b->cache_vic_mux);
    /* Static address pins A6-A7 (to U14) and A8-A11 (on the address bus). */
    static const int vic_a67_pins[2] = { 30, 31 }, vic_a811_pins[4] = { 32, 33, 34, 23 };
    if (b->cache_vic_addr != (own ? (int32_t)((a >> 6) & 63) + 1 : -1)) {
        b->cache_vic_addr = own ? (int32_t)((a >> 6) & 63) + 1 : -1;
        part_drive_bus(s, p, vic_a67_pins, 2, (a >> 6) & 3, own);
        part_drive_bus(s, p, vic_a811_pins, 4, (a >> 8) & 15, own);
    }
    /* Register read: data on D0-D7. */
    part_drive_bus_cached(s, p, vic_data_pins, 8, b->dout_val[DOUT_VIC], b->dout_en[DOUT_VIC],
                          &b->cache_vic_data);
}

/* The register address comes from the multiplexed pins, latched by the VIC
 * when /RAS falls in a CPU half. */
static void vic_commit(sim_t *s, part_t *p) {
    board_t *b = p->user;
    if (b->step == STEP_RAS && !b->vic_owns)
        b->vic_reg = (uint8_t)part_read_bus(s, p, vic_mux_pins, 6);
}

static void vic_tick(board_t *b) {
    sim_t *s = &b->sim;
    part_t *p = P(b, b->p_vic);
    b->vic_fetches = 0;
    /* The VIC needs its dot clock (from U31) to do anything. */
    b->vic_running = !p->dead && b->seen_dot[0] != b->seen_dot[1];
    /* Estimate for the fetches of this cycle: AEC goes low (the VIC takes
     * phi2 too) three cycles after BA went low. */
    b->vic_steal = b->vic_running && b->vic_ba_low && b->ba_streak >= 3;
    if (b->vic_running) {
        uint64_t pins = m6569_tick_fetch(&b->vic, 0);   /* calls vic_fetch() */
        b->vic_ba_low = (pins & M6569_BA) != 0;
        b->vic_irq = (pins & M6569_IRQ) != 0;
        b->vic_last_tick = s->cycle;
    } else {
        b->vic_ba_low = b->vic_irq = false;
    }
    b->ba_streak = b->vic_ba_low ? b->ba_streak + 1 : 0;
    b->vic_steal = b->vic_ba_low && b->ba_streak >= 4;
    sim_mark_dirty(s, b->p_vic);
    /* Every phi1 has a VIC access: when the VIC core had nothing to fetch
     * this is a DRAM refresh (address $3Fxx, an 8 bit counter). */
    if (b->vic_fetches == 0) bus_half(b, 1, b->vic_running, (uint16_t)(0x3F00 | b->vic_refresh--));
}

static void vic_finish(board_t *b) {
    if (!b->vic_running) return;
    sim_t *s = &b->sim;
    part_t *p = P(b, b->p_vic);
    uint64_t pins = 0;
    if (!pin_in(s, p, 10)) {                         /* /CS: $D000-$D3FF */
        pins |= M6569_CS | (b->vic_reg & 63);
        if (pin_in(s, p, 11)) pins |= M6569_RW;
        else M6569_SET_DATA(pins, part_read_bus(s, p, vic_data_pins, 8));
    }
    pins = m6569_tick_finish(&b->vic, pins);
    if ((pins & (M6569_CS | M6569_RW)) == (M6569_CS | M6569_RW))
        set_dout(b, DOUT_VIC, b->p_vic, M6569_GET_DATA(pins));
}

static void crt_set_pixel(void *fb, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    ((uint32_t *)fb)[y * SCREEN_W + x] = color;
}

/* ------------------------------------------------------------------------ */
/* 6526 CIA (U1, U2) and 6581 SID (U18)                                      */
/* ------------------------------------------------------------------------ */

static bool clock_toggles(const uint8_t seen[2]) { return seen[0] == 0 && seen[1] == 1; }

static void cia_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    int i = (int)(p - s->parts) == b->p_cia[0] ? 0 : 1;
    m6526_t *c = &b->cia[i];
    /* Port lines: an output 0 pulls down strongly, everything else is a
     * weak pull-up (so a key or a joystick can pull an output 1 low). */
    uint8_t lows[2] = { (uint8_t)(c->pa.ddr & ~c->pa.reg), (uint8_t)(c->pb.ddr & ~c->pb.reg) };
    int32_t *cache[2] = { &b->cache_cia_pa[i], &b->cache_cia_pb[i] };
    for (int port = 0; port < 2; port++) {
        if (*cache[port] == lows[port] + 1) continue;
        *cache[port] = lows[port] + 1;
        for (int bit = 0; bit < 8; bit++)
            pin_out(s, p, 2 + port * 8 + bit, (lows[port] >> bit) & 1 ? DRV_LO : DRV_PULLUP);
    }
    pin_out(s, p, 21, b->cia_irq[i] ? DRV_LO : DRV_Z);               /* /IRQ */
    part_drive_bus_cached(s, p, cia_data_pins, 8, b->dout_val[DOUT_CIA1 + i], b->dout_en[DOUT_CIA1 + i],
                          &b->cache_cia_data[i]);
}

static void cia_tick(board_t *b, int i) {
    sim_t *s = &b->sim;
    part_t *p = P(b, b->p_cia[i]);
    if (p->dead || !clock_toggles(b->seen_phi2[i])) return;
    m6526_t *c = &b->cia[i];
    if (!pin_in(s, p, 34)) {                     /* /RES held low */
        m6526_reset(c);
        b->cia_irq[i] = false;
        sim_mark_dirty(s, b->p_cia[i]);
        return;
    }
    uint64_t pins = 0;
    if (!pin_in(s, p, 23)) {                    /* /CS: a register access */
        pins = M6526_CS | part_read_bus(s, p, cia_rs_pins, 4);
        if (pin_in(s, p, 22)) pins |= M6526_RW;
        M6526_SET_DATA(pins, part_read_bus(s, p, cia_data_pins, 8));
        /* The port lines are only used when the CPU reads a port register:
         * sample them then (the same values the chip would have). */
        uint8_t pa = 0, pb = 0;
        for (int bit = 0; bit < 8; bit++) {
            pa |= (uint8_t)(pin_in(s, p, 2 + bit) << bit);
            pb |= (uint8_t)(pin_in(s, p, 10 + bit) << bit);
        }
        M6526_SET_PAB(pins, pa, pb);
    } else {
        M6526_SET_PAB(pins, c->pa.inp, c->pb.inp);
    }
    if (pin_in(s, p, 24)) pins |= M6526_FLAG;
    pins = m6526_tick(c, pins);
    b->cia_irq[i] = (pins & M6526_IRQ) != 0;
    if ((pins & (M6526_CS | M6526_RW)) == (M6526_CS | M6526_RW))
        set_dout(b, DOUT_CIA1 + i, b->p_cia[i], M6526_GET_DATA(pins));
    sim_mark_dirty(s, b->p_cia[i]);
}

static void sid_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    part_drive_bus_cached(s, p, sid_data_pins, 8, b->dout_val[DOUT_SID], b->dout_en[DOUT_SID],
                          &b->cache_sid_data);
}

static void sid_tick(board_t *b) {
    sim_t *s = &b->sim;
    part_t *p = P(b, b->p_sid);
    if (p->dead || !clock_toggles(b->seen_phi2[2])) return;
    if (!pin_in(s, p, 5)) { m6581_reset(&b->sid); return; }
    uint64_t pins = 0;
    if (!pin_in(s, p, 8)) {                     /* /CS: a register access */
        pins = M6581_CS | part_read_bus(s, p, sid_addr_pins, 5);
        if (pin_in(s, p, 7)) pins |= M6581_RW;
        M6502_SET_DATA(pins, part_read_bus(s, p, sid_data_pins, 8));
    }
    pins = m6581_tick(&b->sid, pins);
    if ((pins & (M6581_CS | M6581_RW)) == (M6581_CS | M6581_RW))
        set_dout(b, DOUT_SID, b->p_sid, M6502_GET_DATA(pins));
}

/* ------------------------------------------------------------------------ */
/* One cycle                                                                 */
/* ------------------------------------------------------------------------ */

static void board_cycle(board_t *b) {
    sim_t *s = &b->sim;
    s->cycle++;
    cpu_tick(b);
    vic_tick(b);                                    /* phi1 (and maybe a stolen phi2) */
    if (!b->vic_steal) bus_half(b, 2, false, 0);    /* phi2: the CPU's access */
    else if (b->vic_fetches < 2) bus_half(b, 2, true, 0x3FFF);
    /* End of phi2: the chips with a core see the bus, then the CPU. */
    vic_finish(b);
    cia_tick(b, 0);
    cia_tick(b, 1);
    sid_tick(b);
    sim_settle(s);
    cpu_sample(b);
}

void board_run(board_t *b, int cycles) {
    for (int i = 0; i < cycles; i++) board_cycle(b);
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

/* Every chip of the board: its model, a short name drawn on the chip, a few
 * short lines shown under the name when zooming in, and a description.
 * The eight DRAMs don't split the memory by address: each one stores one bit
 * of every byte, so RAMn holds data bit n-1 of all of $0000-$FFFF. */
typedef struct {
    const char *ref;
    const part_type_t *type;
    const char *label;      /* short, drawn on the chip */
    int role;               /* ROLE_*, for the color of the label */
    const char *info;       /* lines under the label, separated by '\n' */
    const char *detail;     /* one line description */
} part_def_t;

static const part_def_t part_defs[] = {
    { "U21", &PT_4164, "RAM1", ROLE_RAM, "4164\nbit D0\n$0000-$FFFF", "4164 DRAM, holds data bit D0 of every byte, $0000-$FFFF" },
    { "U9",  &PT_4164, "RAM2", ROLE_RAM, "4164\nbit D1\n$0000-$FFFF", "4164 DRAM, holds data bit D1 of every byte, $0000-$FFFF" },
    { "U22", &PT_4164, "RAM3", ROLE_RAM, "4164\nbit D2\n$0000-$FFFF", "4164 DRAM, holds data bit D2 of every byte, $0000-$FFFF" },
    { "U10", &PT_4164, "RAM4", ROLE_RAM, "4164\nbit D3\n$0000-$FFFF", "4164 DRAM, holds data bit D3 of every byte, $0000-$FFFF" },
    { "U23", &PT_4164, "RAM5", ROLE_RAM, "4164\nbit D4\n$0000-$FFFF", "4164 DRAM, holds data bit D4 of every byte, $0000-$FFFF" },
    { "U11", &PT_4164, "RAM6", ROLE_RAM, "4164\nbit D5\n$0000-$FFFF", "4164 DRAM, holds data bit D5 of every byte, $0000-$FFFF" },
    { "U24", &PT_4164, "RAM7", ROLE_RAM, "4164\nbit D6\n$0000-$FFFF", "4164 DRAM, holds data bit D6 of every byte, $0000-$FFFF" },
    { "U12", &PT_4164, "RAM8", ROLE_RAM, "4164\nbit D7\n$0000-$FFFF", "4164 DRAM, holds data bit D7 of every byte, $0000-$FFFF" },
    { "U6",  &PT_2114, "VRAM", ROLE_VRAM, "2114\ncolor RAM\n$D800-$DBFF",
      "2114 color RAM (video), $D800-$DBFF, 4 bits, read by the VIC every character" },
    { "U3",  &PT_2364, "BASIC", ROLE_ROM, "2364 ROM\n$A000-$BFFF", "2364 BASIC ROM, $A000-$BFFF" },
    { "U4",  &PT_2364, "KERNAL", ROLE_ROM, "2364 ROM\n$E000-$FFFF", "2364 KERNAL ROM, $E000-$FFFF" },
    { "U5",  &PT_2332, "CHAR", ROLE_ROM, "2332 ROM\n$D000-$DFFF",
      "2332 character ROM, $D000-$DFFF (the VIC sees it at $1000 and $9000)" },
    { "U17", &PT_82S100, "PLA", ROLE_PLA, "906114-01\nmemory map",
      "906114-01 PLA (82S100): the memory map, decodes all the chip selects" },
    { "U7",  &PT_6510, "CPU", ROLE_CHIP, "6510", "6510 CPU" },
    { "U19", &PT_6569, "VIC", ROLE_CHIP, "6569 VIC-II\n$D000-$D3FF",
      "6569 VIC-II video chip, $D000-$D3FF (PAL); also makes the system clock" },
    { "U18", &PT_6581, "SID", ROLE_CHIP, "6581\n$D400-$D7FF", "6581 SID sound chip, $D400-$D7FF" },
    { "U1",  &PT_6526, "CIA1", ROLE_CHIP, "6526\n$DC00-$DCFF", "6526 CIA #1, $DC00-$DCFF: keyboard, joysticks, IRQ" },
    { "U2",  &PT_6526, "CIA2", ROLE_CHIP, "6526\n$DD00-$DDFF",
      "6526 CIA #2, $DD00-$DDFF: serial bus, user port, VIC bank, NMI" },
    { "U25", &PT_74LS257, "MUX", ROLE_GLUE, "74LS257", "74LS257 DRAM address mux: CPU A0-A3 (row), A8-A11 (column)" },
    { "U13", &PT_74LS257, "MUX", ROLE_GLUE, "74LS257", "74LS257 DRAM address mux: CPU A4-A7 (row), A12-A15 (column)" },
    { "U14", &PT_74LS258, "MUX", ROLE_GLUE, "74LS258", "74LS258 VIC address mux: VIC A6-A7 (row), bank bits VA14-VA15 (column)" },
    { "U26", &PT_74LS373, "LATCH", ROLE_GLUE, "74LS373", "74LS373 latch: puts the VIC address on A0-A7" },
    { "U15", &PT_74LS139, "I/O", ROLE_GLUE, "74LS139",
      "74LS139 I/O decoder: VIC, SID, color RAM, CIA1, CIA2, I/O1, I/O2" },
    { "U27", &PT_74LS08, "AND", ROLE_GLUE, "74LS08", "74LS08 AND gates: CPU AEC, RDY, color RAM select, TOD clock" },
    { "U8",  &PT_7406, "INV", ROLE_GLUE, "7406", "7406 open collector inverters: /AEC, /RESET, /NMI, serial bus" },
    { "U16", &PT_4066, "SW", ROLE_GLUE, "4066", "4066 switch: connects the color RAM to D0-D3 in CPU cycles" },
    { "U28", &PT_4066, "SW", ROLE_GLUE, "4066", "4066 switch: selects the paddles read by the SID" },
    { "U20", &PT_556, "RST", ROLE_GLUE, "556", "556 timer: power-on reset, RESTORE key NMI" },
    { "U31", &PT_74LS629, "OSC", ROLE_CLOCK, "74LS629", "74LS629 oscillators: dot clock and color clock" },
    { "U29", &PT_74LS74, "PLL", ROLE_CLOCK, "74LS74", "74LS74, part of the clock PLL (not simulated)" },
    { "U30", &PT_74LS193, "PLL", ROLE_CLOCK, "74LS193", "74LS193, part of the clock PLL (not simulated)" },
    { "U32", &PT_MC4044, "PLL", ROLE_CLOCK, "MC4044", "MC4044 phase detector, part of the clock PLL (not simulated)" },
};

static const part_def_t *part_def(board_t *b, int part) {
    const char *ref = b->sim.parts[part].ref;
    for (size_t i = 0; i < sizeof(part_defs) / sizeof(part_defs[0]); i++)
        if (!strcmp(part_defs[i].ref, ref)) return &part_defs[i];
    return NULL;
}

const char *board_part_label(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->label : b->sim.parts[part].name;
}

const char *board_part_detail(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->detail : b->sim.parts[part].name;
}

const char *board_part_info(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->info : "";
}

int board_part_role(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->role : ROLE_OTHER;
}

/* Find which elements a resistor (or pack, or ferrite bead) is made of. */
static void setup_passive(board_t *b, int idx) {
    sim_t *s = &b->sim;
    part_t *p = &s->parts[idx];
    resistor_state_t *st = calloc(1, sizeof(*st));
    p->state = st;
    int pairs[16][2], npairs = 0;
    bool bussed = strstr(p->name, "Bussed") != NULL;
    bool isolated = strstr(p->name, "isolated") != NULL;
    if (bussed) {
        for (int n = 2; n < 48 && npairs < 16; n++)
            if (p->pinmap[n] >= 0) { pairs[npairs][0] = p->pinmap[n]; pairs[npairs][1] = p->pinmap[1]; npairs++; }
    } else if (isolated) {
        for (int n = 1; n + 1 < 48 && npairs < 16; n += 2)
            if (p->pinmap[n] >= 0 && p->pinmap[n + 1] >= 0) {
                pairs[npairs][0] = p->pinmap[n]; pairs[npairs][1] = p->pinmap[n + 1]; npairs++;
            }
    } else if (p->pinmap[1] >= 0 && p->pinmap[2] >= 0) {
        pairs[0][0] = p->pinmap[1]; pairs[0][1] = p->pinmap[2]; npairs = 1;
    }
    /* A "dashed" jumper is an option not fitted on this board (E2 selects
     * NTSC): it connects nothing. */
    if (strstr(p->name, "DASHED")) npairs = 0;
    /* Series resistors that are part of the logic (the others are analog). */
    bool series = !strcmp(p->ref, "RP1") || !strcmp(p->ref, "RP2") || !strcmp(p->ref, "R42");
    for (int i = 0; i < npairs; i++) {
        element_t *e = &st->el[st->nel++];
        int a = pairs[i][0], c = pairs[i][1];
        int ra = s->pins[a].net >= 0 ? s->nets[s->pins[a].net].rail : RAIL_NONE;
        int rc = s->pins[c].net >= 0 ? s->nets[s->pins[c].net].rail : RAIL_NONE;
        if (ra && !rc) { int t = a; a = c; c = t; t = ra; ra = rc; rc = t; }
        e->pin_a = a; e->pin_b = c;
        if (p->kind == PART_LINK) e->type = EL_LINK;
        else if (rc == RAIL_VCC && !ra) e->type = EL_PULLUP;
        else if (rc == RAIL_GND && !ra) e->type = EL_PULLDOWN;
        else if (series) e->type = EL_LINK;
        else e->type = EL_ANALOG;
        if (e->type == EL_PULLUP || e->type == EL_PULLDOWN) p->eval = resistor_eval;
    }
}

bool board_part_simulated(board_t *b, int part) {
    part_t *p = &b->sim.parts[part];
    if (p->kind == PART_IC || p->kind == PART_CONNECTOR) return true;
    if (!strcmp(p->ref, "Y1")) return true;
    if (p->kind == PART_RESISTOR || p->kind == PART_LINK) {
        resistor_state_t *st = p->state;
        for (int i = 0; st && i < st->nel; i++) if (st->el[i].type != EL_ANALOG) return true;
    }
    return false;
}

/* Rebuild the electrical network after a change: links, keys, faults. */
static void board_rebuild(board_t *b) {
    sim_t *s = &b->sim;
    sim_join_begin(s);
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if ((p->kind != PART_RESISTOR && p->kind != PART_LINK) || !p->state) continue;
        if (p->rfault == RF_OPEN) continue;
        resistor_state_t *st = p->state;
        for (int k = 0; k < st->nel; k++) {
            element_t *e = &st->el[k];
            if (e->type != EL_LINK) continue;
            if (s->pins[e->pin_a].fault == PF_OPEN || s->pins[e->pin_b].fault == PF_OPEN) continue;
            sim_join(s, s->pins[e->pin_a].net, s->pins[e->pin_b].net);
        }
    }
    /* Keyboard: a pressed key connects a PA line to a PB line through the
     * keyboard connector CN1. RESTORE connects its line to ground. */
    for (int line = 0; line < 8; line++)
        for (int col = 0; col < 8; col++) {
            if (!((b->key_matrix[line] >> col) & 1)) continue;
            int pa = b->pin_cn1_pa[line], pb = b->pin_cn1_pb[col];
            if (pa < 0 || pb < 0 || s->pins[pa].fault == PF_OPEN || s->pins[pb].fault == PF_OPEN) continue;
            sim_join(s, s->pins[pa].net, s->pins[pb].net);
        }
    if ((b->key_matrix[8] & 1) && b->pin_restore >= 0 && s->pins[b->pin_restore].fault != PF_OPEN)
        sim_join(s, s->pins[b->pin_restore].net, b->net_gnd);
    for (int i = 0; i < s->nparts; i++) sim_update_part_power(s, i);
    vco_state_t *vco = s->parts[b->p_u31].state;
    vco->crystal_ok = b->p_y1 < 0 || s->parts[b->p_y1].rfault != RF_OPEN;
    sim_rebuild(s);
}

static void init_keymap(board_t *b) {
    /* Same matrix as c64-llm/c64.h: line = CIA1 PA bit, column = PB bit. */
    kbd_init(&b->kbd, 2);
    const char *keymap =
        "        3WA4ZSE 5RD6CFTX7YG8BHUV9IJ0MKON+PL-.:@,~*;  = /1  2  Q "
        "        #wa$zse %rd&cftx'yg(bhuv)ij0mkon pl >[ <$ ]    ?!  \"  q ";
    kbd_register_modifier(&b->kbd, 0, 7, 1);    /* shift */
    kbd_register_modifier(&b->kbd, 1, 2, 7);    /* ctrl */
    for (int shift = 0; shift < 2; shift++)
        for (int column = 0; column < 8; column++)
            for (int line = 0; line < 8; line++) {
                int c = keymap[shift * 64 + line * 8 + column];
                if (c != 0x20) kbd_register_key(&b->kbd, c, column, line, shift ? 1 : 0);
            }
    kbd_register_key(&b->kbd, C64_KEY_SPACE, 4, 7, 0);
    kbd_register_key(&b->kbd, C64_KEY_CSRLEFT, 2, 0, 1);
    kbd_register_key(&b->kbd, C64_KEY_CSRRIGHT, 2, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_CSRDOWN, 7, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_CSRUP, 7, 0, 1);
    kbd_register_key(&b->kbd, C64_KEY_DEL, 0, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_INST, 0, 0, 1);
    kbd_register_key(&b->kbd, C64_KEY_HOME, 3, 6, 0);
    kbd_register_key(&b->kbd, C64_KEY_CLR, 3, 6, 1);
    kbd_register_key(&b->kbd, C64_KEY_RETURN, 1, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_CTRL, 2, 7, 0);
    kbd_register_key(&b->kbd, C64_KEY_CBM, 5, 7, 0);
    kbd_register_key(&b->kbd, C64_KEY_RESTORE, 0, 8, 0);
    kbd_register_key(&b->kbd, C64_KEY_STOP, 7, 7, 0);
    kbd_register_key(&b->kbd, C64_KEY_F1, 4, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_F3, 5, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_F5, 6, 0, 0);
    kbd_register_key(&b->kbd, C64_KEY_F7, 3, 0, 0);
}

/* The keyboard connector pin that shares a net with the given CIA pin. */
static int cn1_pin_on_net_of(board_t *b, int part, int number) {
    sim_t *s = &b->sim;
    int net = s->pins[part_pin(&s->parts[part], number)].net;
    part_t *cn1 = &s->parts[b->p_cn1];
    for (int i = 0; i < cn1->npins; i++)
        if (s->pins[cn1->first_pin + i].net == net) return cn1->first_pin + i;
    return -1;
}

board_t *board_create(void) {
    board_t *b = calloc(1, sizeof(*b));
    sim_t *s = &b->sim;
    sim_init(s);
    for (int i = 0; i < s->nparts; i++) s->parts[i].user = b;
    for (size_t i = 0; i < sizeof(part_defs) / sizeof(part_defs[0]); i++) {
        int idx = sim_find_part(s, part_defs[i].ref);
        if (idx >= 0) part_set_type(s, idx, part_defs[i].type);
    }
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].kind == PART_RESISTOR || s->parts[i].kind == PART_LINK) setup_passive(b, i);
    b->p_cpu = sim_find_part(s, "U7");
    b->p_vic = sim_find_part(s, "U19");
    b->p_cia[0] = sim_find_part(s, "U1");
    b->p_cia[1] = sim_find_part(s, "U2");
    b->p_sid = sim_find_part(s, "U18");
    b->p_u31 = sim_find_part(s, "U31");
    b->p_y1 = sim_find_part(s, "Y1");
    b->p_cn1 = sim_find_part(s, "CN1");
    b->net_gnd = sim_find_net(s, "Earth");
    /* The big chips: models are here, around their cores. */
    s->parts[b->p_cpu].eval = cpu_eval;
    s->parts[b->p_vic].eval = vic_eval;
    s->parts[b->p_vic].commit = vic_commit;
    s->parts[b->p_cia[0]].eval = cia_eval;
    s->parts[b->p_cia[1]].eval = cia_eval;
    s->parts[b->p_sid].eval = sid_eval;
    /* ROM images. */
    ((rom_state_t *)s->parts[sim_find_part(s, "U3")].state)->data = dump_c64_basic_bin;
    ((rom_state_t *)s->parts[sim_find_part(s, "U4")].state)->data = dump_c64_kernalv3_bin;
    ((rom_state_t *)s->parts[sim_find_part(s, "U5")].state)->data = dump_c64_char_bin;
    /* Which DRAM holds which data bit: the one whose D pin is on net Dn. */
    for (int i = 0; i < s->nparts; i++) {
        if (s->parts[i].reset != PT_4164.reset) continue;
        int net = s->pins[part_pin(&s->parts[i], 2)].net;
        const char *name = net >= 0 ? s->nets[net].name : "";
        if (name[0] == 'D' && name[1] >= '0' && name[1] <= '7' && !name[2]) b->dram_part[name[1] - '0'] = i;
    }
    /* Keyboard lines on CN1, and the RESTORE line. */
    for (int i = 0; i < 8; i++) {
        b->pin_cn1_pa[i] = cn1_pin_on_net_of(b, b->p_cia[0], 2 + i);
        b->pin_cn1_pb[i] = cn1_pin_on_net_of(b, b->p_cia[0], 10 + i);
    }
    b->pin_restore = part_pin(&s->parts[b->p_cn1], 3);
    timer556_state_t *t = s->parts[sim_find_part(s, "U20")].state;
    t->restore_net = s->pins[b->pin_restore].net;
    init_keymap(b);
    /* The bus lines get consecutive bits in the bus bitmap, so the chips can
     * read them as words (speed only, see sim.h). */
    char name[32];
    int bit = 0;
    static const struct { const char *prefix; int n; } buses[] = {
        { "A", 16 }, { "D", 8 }, { "VA", 8 }, { "MA", 8 } };
    for (size_t k = 0; k < sizeof(buses) / sizeof(buses[0]); k++)
        for (int i = 0; i < buses[k].n; i++) {
            snprintf(name, sizeof(name), "%s%d", buses[k].prefix, i);
            sim_set_bus_bit(s, sim_find_net(s, name), bit++);
        }
    /* The color RAM nibble (D8-D11 of the VIC), named after U16 pads. */
    for (int i = 0; i < 4; i++)
        sim_set_bus_bit(s, s->pins[part_pin(&s->parts[b->p_vic], 38 - i)].net, bit++);
    b->check = getenv("C64HW_CHECK") != NULL;
    b->scope_pin = -1;
    b->st_hi = calloc(s->nnets, sizeof(uint32_t));
    b->st_lo = calloc(s->nnets, sizeof(uint32_t));
    b->st_float = calloc(s->nnets, sizeof(uint32_t));
    b->st_fight = calloc(s->nnets, sizeof(uint32_t));
    board_power_on(b);
    return b;
}

/* Cold start: like switching the C64 on. Faults stay where they are. */
void board_power_on(board_t *b) {
    sim_t *s = &b->sim;
    s->cycle = 0;
    b->cpu_pins = m6502_init(&b->cpu, &(m6502_desc_t){
        .m6510_in_cb = cpu_port_in,
        .m6510_out_cb = cpu_port_out,
        .m6510_user_data = b,
        .m6510_io_pullup = 0,
        .m6510_io_floating = 0xC0,  /* bits 6-7 have no pins: they keep their value */
    });
    b->cpu_in_reset = true;
    m6569_init(&b->vic, &(m6569_desc_t){
        .fetch_cb = vic_fetch,
        .screen = { .x = 64, .y = 24, .width = SCREEN_W, .height = SCREEN_H },
        .user_data = b,
        .crt_set_pixel = crt_set_pixel,
        .crt_set_pixel_fb = b->screen,
    });
    m6526_init(&b->cia[0]);
    m6526_init(&b->cia[1]);
    m6581_init(&b->sid, &(m6581_desc_t){ .tick_hz = C64_FREQ, .sound_hz = 44100, .magnitude = 1.0f });
    memset(b->screen, 0, sizeof(b->screen));
    memset(b->dout_en, 0, sizeof(b->dout_en));
    b->cia_irq[0] = b->cia_irq[1] = false;
    b->vic_ba_low = b->vic_irq = false;
    b->ba_streak = 0;
    /* Assume clocks are alive at power on, the first cycle will tell. */
    b->seen_phi0[0] = b->seen_dot[0] = b->seen_color[0] = 0;
    b->seen_phi0[1] = b->seen_dot[1] = b->seen_color[1] = 1;
    for (int i = 0; i < 3; i++) { b->seen_phi2[i][0] = 0; b->seen_phi2[i][1] = 1; }
    /* Nets start discharged, parts in their power-on state, and the noise
     * starts over (the same faults give the same run). */
    sim_discharge(s);
    sim_seed(s, 1);
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].reset) s->parts[i].reset(s, &s->parts[i]);
    board_rebuild(b);
}

/* ------------------------------------------------------------------------ */
/* Faults                                                                    */
/* ------------------------------------------------------------------------ */

void board_set_pin_fault(board_t *b, int pin, int fault) {
    sim_set_pin_fault(&b->sim, pin, fault);
    board_rebuild(b);
}

void board_set_removed(board_t *b, int part, bool removed) {
    b->sim.parts[part].removed = removed;
    board_rebuild(b);
}

void board_set_rfault(board_t *b, int part, int rfault) {
    b->sim.parts[part].rfault = rfault;
    board_rebuild(b);
}

/* Text form of a fault, used by the command line:
 *   U17:removed  U17.18:gnd  U17.18:vcc  U17.18:open  R51:open  R51:short */
bool board_apply_fault_spec(board_t *b, const char *spec) {
    char ref[16] = {0}, what[16] = {0};
    int pin = 0;
    if (sscanf(spec, "%15[^.:].%d:%15s", ref, &pin, what) != 3 &&
        sscanf(spec, "%15[^:]:%15s", ref, what) != 2) return false;
    int part = sim_find_part(&b->sim, ref);
    if (part < 0) return false;
    if (pin) {
        int pi = part_pin(&b->sim.parts[part], pin);
        if (pi < 0) return false;
        int f = !strcmp(what, "gnd") ? PF_GND : !strcmp(what, "vcc") ? PF_VCC :
                !strcmp(what, "open") ? PF_OPEN : PF_OK;
        board_set_pin_fault(b, pi, f);
    } else if (!strcmp(what, "removed") || !strcmp(what, "present")) {
        board_set_removed(b, part, !strcmp(what, "removed"));
    } else {
        board_set_rfault(b, part, !strcmp(what, "open") ? RF_OPEN : !strcmp(what, "short") ? RF_SHORT : RF_OK);
    }
    return true;
}

void board_clear_faults(board_t *b) {
    sim_t *s = &b->sim;
    for (int i = 0; i < s->npins; i++) sim_set_pin_fault(s, i, PF_OK);
    for (int i = 0; i < s->nparts; i++) { s->parts[i].removed = false; s->parts[i].rfault = RF_OK; }
    board_rebuild(b);
}

/* ------------------------------------------------------------------------ */
/* Keyboard, PRG loading, video                                              */
/* ------------------------------------------------------------------------ */

/* If the set of closed key switches changed, rebuild the network. */
static void sync_keys(board_t *b) {
    bool changed = false;
    for (int line = 0; line < 9; line++) {
        uint16_t m = b->kbd.scanout_column_masks[line];
        if (m != b->key_matrix[line]) { b->key_matrix[line] = m; changed = true; }
    }
    if (changed) board_rebuild(b);
}

void board_key_down(board_t *b, int key) { kbd_key_down(&b->kbd, key); sync_keys(b); }
void board_key_up(board_t *b, int key) { kbd_key_up(&b->kbd, key); sync_keys(b); }

/* Once per frame: release keys that were pressed long enough. 'us' is
 * emulated time, so a key stays down long enough for the KERNAL to scan it
 * even when the simulation is slower than real time. */
void board_frame_update(board_t *b, uint32_t us) {
    kbd_update(&b->kbd, us);
    sync_keys(b);
}

/* Write a byte straight into the eight DRAM chips. */
static void ram_poke(board_t *b, uint16_t addr, uint8_t v) {
    int cell = ((addr & 0xFF) << 8) | (addr >> 8);     /* row = A0-A7, column = A8-A15 */
    for (int bit = 0; bit < 8; bit++) {
        dram_state_t *st = b->sim.parts[b->dram_part[bit]].state;
        st->cells[cell] = (v >> bit) & 1;
    }
}

bool board_load_prg(board_t *b, const char *path, bool run, char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "can't open %s", path); return false; }
    static uint8_t buf[65536 + 2];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 3) { snprintf(err, errlen, "%s is too short", path); return false; }
    uint16_t start = buf[0] | (buf[1] << 8);
    uint16_t end = (uint16_t)(start + n - 2);
    for (size_t i = 2; i < n; i++) ram_poke(b, (uint16_t)(start + i - 2), buf[i]);
    /* BASIC pointers: end of program / start of variables. */
    static const uint16_t ptrs[] = { 0x2D, 0x2F, 0x31, 0x33, 0xAE };
    for (size_t i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); i++) {
        ram_poke(b, ptrs[i], end & 0xFF);
        ram_poke(b, (uint16_t)(ptrs[i] + 1), end >> 8);
    }
    if (run) {      /* type RUN<return> through the keyboard buffer */
        const char *cmd = "RUN\r";
        for (int i = 0; i < 4; i++) ram_poke(b, (uint16_t)(0x277 + i), (uint8_t)cmd[i]);
        ram_poke(b, 0xC6, 4);
    }
    snprintf(err, errlen, "loaded %s at $%04X-$%04X", path, start, end);
    return true;
}

/* What the monitor shows. Without the VIC running there is no sync: snow.
 * Without the color clock (or with the COLOR pin broken) the picture is
 * black and white: only luminance reaches the monitor. */
void board_monitor(board_t *b, uint32_t *out) {
    sim_t *s = &b->sim;
    part_t *vic = &s->parts[b->p_vic];
    bool sync = !vic->dead && s->cycle - b->vic_last_tick < 20000 &&
                s->pins[part_pin(vic, 15)].fault == PF_OK;
    bool color = b->seen_color[0] != b->seen_color[1] && s->pins[part_pin(vic, 14)].fault == PF_OK;
    static uint32_t seed = 12345;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t c = b->screen[i];
        if (!sync) {
            seed = seed * 1103515245u + 12345u;
            uint32_t g = (seed >> 24) & 0xFF;
            g = g * 3 / 4;
            c = 0xFF000000u | (g << 16) | (g << 8) | g;
        } else if (!color) {
            uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
            uint32_t y = (r * 299 + g * 587 + bl * 114) / 1000;
            c = 0xFF000000u | (y << 16) | (y << 8) | y;
        }
        out[i] = c;
    }
}

/* ------------------------------------------------------------------------ */
/* Logic probe                                                               */
/* ------------------------------------------------------------------------ */

int board_net_state(board_t *b, int net) {
    if (net < 0) return NET_NONE;
    uint32_t hi = b->st_hi[net], lo = b->st_lo[net], fl = b->st_float[net], fi = b->st_fight[net];
    uint32_t tot = hi + lo + fl + fi;
    if (tot == 0) {     /* no samples yet: use the current state */
        const net_t *n = &b->sim.nets[b->sim.nets[net].root];
        if (n->contention) return NET_CONTENTION;
        if (n->strength == S_Z) return NET_FLOATING;
        return n->level ? NET_HIGH : NET_LOW;
    }
    if (fi * 20 > tot) return NET_CONTENTION;
    if (fl == tot) return NET_FLOATING;
    if (hi && lo) return NET_PULSING;
    if (hi) return fl ? NET_PULSING : NET_HIGH;
    if (lo) return fl ? NET_PULSING : NET_LOW;
    return NET_FLOATING;
}

void board_stats_reset(board_t *b) {
    size_t n = (size_t)b->sim.nnets * sizeof(uint32_t);
    memset(b->st_hi, 0, n); memset(b->st_lo, 0, n);
    memset(b->st_float, 0, n); memset(b->st_fight, 0, n);
}
