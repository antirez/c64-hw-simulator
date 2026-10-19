/* c64.c - the Commodore 64, board C64 ASSY 250407.
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
#include "c64.h"
#include "c64-roms.h"
#include "sokol/sokol_app.h"   /* key codes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
    c64_t *c64 = C64(b);
    if (i == DOUT_VIC) return c64->p_vic;
    if (i == DOUT_SID) return c64->p_sid;
    return c64->p_cia[i - DOUT_CIA1];
}

/* ------------------------------------------------------------------------ */
/* 6510 CPU (U7)                                                             */
/* ------------------------------------------------------------------------ */

/* The CPU core runs one cycle at the start of each bus cycle: it consumes
 * the data it sampled at the end of the previous phi2 and produces the next
 * address. Its pins are driven by cpu_eval() during phi2. */
static void cpu_tick(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    part_t *p = P(b, c64->p_cpu);
    c64->cpu_ticked = false;
    /* No PHI0 clock from the VIC: the CPU is frozen. */
    c64->cpu_running = !p->dead && c64->seen_phi0[0] == 0 && c64->seen_phi0[1] == 1;
    if (!c64->cpu_running) return;
    /* /RES is a level: the CPU is held while it is low, and runs its reset
     * sequence when it goes high. */
    if (!pin_in(s, p, 40)) { c64->cpu_in_reset = true; return; }
    if (c64->cpu_in_reset) {
        c64->cpu_in_reset = false;
        c64->cpu_pins |= M6502_RES;
        c64->cpu.io_ddr = 0;          /* the 6510 port comes out of reset as input */
        c64->cpu.io_out = 0;
    }
    uint64_t pins = c64->cpu_pins & ~(M6502_IRQ | M6502_NMI | M6502_RDY);
    if (!pin_in(s, p, 3)) pins |= M6502_IRQ;     /* /IRQ low */
    if (!pin_in(s, p, 4)) pins |= M6502_NMI;     /* /NMI low (edge detected by the core) */
    if (!pin_in(s, p, 2)) pins |= M6502_RDY;     /* RDY low: halt on the next read */
    c64->cpu_pins = m6502_tick(&c64->cpu, pins);
    c64->cpu_ticked = true;
    sim_mark_dirty(s, c64->p_cpu);
}

static void cpu_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    c64_t *c64 = C64(b);
    /* PHI2 output follows the PHI0 input: it clocks the CIAs and the SID. */
    pin_out(s, p, 39, DRV_BIT(pin_in(s, p, 1)));
    /* I/O port P0..P5 (pins 29..24): driven where the DDR says output. */
    int32_t port_key = (c64->cpu.io_ddr << 8) | c64->cpu.io_out;
    if (c64->cache_cpu_port != port_key + 1) {
        c64->cache_cpu_port = port_key + 1;
        for (int i = 0; i < 6; i++) {
            bool out = (c64->cpu.io_ddr >> i) & 1;
            pin_out(s, p, 29 - i, out ? DRV_BIT((c64->cpu.io_out >> i) & 1) : DRV_Z);
        }
    }
    /* Address, R/W and data only while AEC is high: when the VIC owns the
     * bus the 6510 puts its outputs in high impedance. */
    bool bus = pin_in(s, p, 5);
    uint64_t pins = c64->cpu_pins;
    uint16_t addr = M6502_GET_ADDR(pins);
    bool write = !(pins & M6502_RW);
    part_drive_bus_cached(s, p, cpu_addr_pins, 16, addr, bus, &c64->cache_cpu_addr);
    pin_out(s, p, 38, bus ? DRV_BIT(!write) : DRV_Z);
    /* Writes to $00/$01 go to the internal port: the 6510 doesn't drive the
     * data bus, so the RAM below gets whatever charge is on the bus. */
    part_drive_bus_cached(s, p, cpu_data_pins, 8, M6502_GET_DATA(pins), bus && write && addr > 1,
                          &c64->cache_cpu_data);
}

/* Self-check oracle, for tests only (C64HW_CHECK=1): on a healthy board
 * the CPU must read the KERNAL exactly, and the VIC must see in bank 0 the
 * character ROM at $1000-$1FFF and the DRAM contents elsewhere. This code
 * knows the memory map; the simulation itself doesn't use it. */
static uint8_t dram_peek(board_t *b, uint16_t addr) {
    c64_t *c64 = C64(b);
    int cell = ((addr & 0xFF) << 8) | (addr >> 8);
    uint8_t v = 0;
    for (int bit = 0; bit < 8; bit++)
        v |= (uint8_t)(((dram_state_t *)b->sim.parts[c64->dram_part[bit]].state)->cells[cell] << bit);
    return v;
}

static void check_report(board_t *b, const char *what, uint16_t addr, uint8_t got, uint8_t want) {
    if (b->check_errors++ < 10)
        printf("CHECK: %s $%04X read $%02X, expected $%02X (cycle %llu)\n", what, addr, got, want,
               (unsigned long long)b->sim.cycle);
}

static void check_cpu_read(board_t *b, uint64_t pins) {
    c64_t *c64 = C64(b);
    uint16_t a = M6502_GET_ADDR(pins);
    if (c64->cart) return;        /* a cartridge changes the memory map */
    if (!(pins & M6502_RW) || a < 0xE000 || (c64->cpu.io_out & c64->cpu.io_ddr & 2) == 0) return;
    uint8_t want = dump_c64_kernalv3_bin[a - 0xE000];
    if (M6502_GET_DATA(pins) != want) check_report(b, "CPU", a, M6502_GET_DATA(pins), want);
}

static void check_vic_fetch(board_t *b, uint16_t a, uint8_t got) {
    c64_t *c64 = C64(b);
    if (c64->cart) return;
    uint8_t want = (a & 0x3000) == 0x1000 ? dump_c64_char_bin[a & 0xFFF] : dram_peek(b, a);
    if (got != want) check_report(b, "VIC", a, got, want);
}

/* End of phi2: the CPU latches the data bus. */
static void cpu_sample(board_t *b) {
    c64_t *c64 = C64(b);
    if (!c64->cpu_ticked) return;
    sim_t *s = &b->sim;
    uint64_t pins = c64->cpu_pins;
    if (pins & M6502_RW) {
        uint8_t data = (uint8_t)part_read_bus(s, P(b, c64->p_cpu), cpu_data_pins, 8);
        M6502_SET_DATA(pins, data);
    }
    if (M6510_CHECK_IO(pins)) pins = m6510_iorq(&c64->cpu, pins);
    if (b->check && !c64->vic_steal) check_cpu_read(b, pins);
    c64->cpu_pins = pins;
    sim_mark_dirty(s, c64->p_cpu);
}

/* The 6510 port reads the real pin levels (pull-ups on LORAM/HIRAM/CHAREN
 * and cassette sense make them read 1 when not driven). */
static uint8_t cpu_port_in(void *user) {
    board_t *b = user;
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    uint8_t v = 0;
    for (int i = 0; i < 6; i++) v |= (uint8_t)(pin_in(s, P(b, c64->p_cpu), 29 - i) << i);
    return v;
}
static void cpu_port_out(uint8_t data, void *user) { (void)data; (void)user; }

/* ------------------------------------------------------------------------ */
/* Bus half cycles                                                           */
/* ------------------------------------------------------------------------ */

static void bus_step(board_t *b, int step) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    c64->step = step;
    s->clock_level = c64->phase == 2;
    /* The VIC drives /RAS, /CAS and the address; the clock oscillator
     * changes level with the phase. */
    sim_mark_dirty(s, c64->p_vic);
    if (step == STEP_ROW) sim_mark_dirty(s, c64->p_u31);
    sim_settle(s);
    sim_commit(s);      /* latches and memories see the edge */
    sim_settle(s);      /* and their outputs propagate */
    board_sample(b);
}

static void bus_half(board_t *b, int phase, bool vic_owns, uint16_t vic_addr) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    c64->phase = phase;
    c64->vic_owns = vic_owns;
    c64->vic_addr = vic_addr;
    /* Register read data of the previous phi2 leaves the bus. */
    for (int i = 0; i < 4; i++) {
        if (!c64->dout_en[i]) continue;
        c64->dout_en[i] = false;
        sim_mark_dirty(s, dout_part(b, i));
    }
    bus_step(b, STEP_ROW);
    bus_step(b, STEP_RAS);
    bus_step(b, STEP_CAS);
    /* Clock detection: remember the clock pin levels in this half. */
    int h = phase - 1;
    c64->seen_phi0[h] = (uint8_t)pin_in(s, P(b, c64->p_cpu), 1);
    c64->seen_dot[h] = (uint8_t)pin_in(s, P(b, c64->p_vic), 22);
    c64->seen_color[h] = (uint8_t)pin_in(s, P(b, c64->p_vic), 21);
    c64->seen_phi2[0][h] = (uint8_t)pin_in(s, P(b, c64->p_cia[0]), 25);
    c64->seen_phi2[1][h] = (uint8_t)pin_in(s, P(b, c64->p_cia[1]), 25);
    c64->seen_phi2[2][h] = (uint8_t)pin_in(s, P(b, c64->p_sid), 6);
}

static void set_dout(board_t *b, int idx, int part, uint8_t val) {
    c64_t *c64 = C64(b);
    c64->dout_en[idx] = true;
    c64->dout_val[idx] = val;
    sim_mark_dirty(&b->sim, part);
}

/* ------------------------------------------------------------------------ */
/* 6569 VIC-II (U19)                                                         */
/* ------------------------------------------------------------------------ */

/* Memory fetch callback of the VIC core: every fetch is a real bus half. */
static uint16_t vic_fetch(uint16_t addr, void *user) {
    board_t *b = user;
    c64_t *c64 = C64(b);
    int phase = (c64->vic_fetches > 0 && c64->vic_steal) ? 2 : 1;
    c64->vic_fetches++;
    bus_half(b, phase, true, addr & 0x3FFF);
    /* D0-D7 from the main bus, D8-D11 from the color RAM. */
    static const int vic_color_pins[4] = { 38, 37, 36, 35 };     /* D8-D11 */
    uint16_t data = (uint16_t)(part_read_bus(&b->sim, P(b, c64->p_vic), vic_data_pins, 8) |
                               part_read_bus(&b->sim, P(b, c64->p_vic), vic_color_pins, 4) << 8);
    if (b->check) check_vic_fetch(b, addr & 0x3FFF, data & 0xFF);
    return data;
}

static void vic_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    c64_t *c64 = C64(b);
    bool run = c64->vic_running;
    uint16_t a = c64->vic_addr;
    bool own = run && c64->vic_owns;
    pin_out(s, p, 17, run ? DRV_BIT(c64->phase == 2) : DRV_Z);          /* PHI0 */
    pin_out(s, p, 18, run ? DRV_BIT(c64->step == STEP_ROW) : DRV_Z);    /* /RAS */
    pin_out(s, p, 19, run ? DRV_BIT(c64->step != STEP_CAS) : DRV_Z);    /* /CAS */
    pin_out(s, p, 16, run ? DRV_BIT(!c64->vic_owns) : DRV_Z);           /* AEC */
    pin_out(s, p, 12, run ? DRV_BIT(!c64->vic_ba_low) : DRV_Z);         /* BA */
    pin_out(s, p, 8, run && c64->vic_irq ? DRV_LO : DRV_Z);             /* /IRQ, open drain */
    /* Multiplexed address: row (A0-A5) then column (A8-A13). */
    unsigned mux = c64->step == STEP_CAS ? (a >> 8) & 63 : a & 63;
    part_drive_bus_cached(s, p, vic_mux_pins, 6, mux, own, &c64->cache_vic_mux);
    /* Static address pins A6-A7 (to U14) and A8-A11 (on the address bus). */
    static const int vic_a67_pins[2] = { 30, 31 }, vic_a811_pins[4] = { 32, 33, 34, 23 };
    if (c64->cache_vic_addr != (own ? (int32_t)((a >> 6) & 63) + 1 : -1)) {
        c64->cache_vic_addr = own ? (int32_t)((a >> 6) & 63) + 1 : -1;
        part_drive_bus(s, p, vic_a67_pins, 2, (a >> 6) & 3, own);
        part_drive_bus(s, p, vic_a811_pins, 4, (a >> 8) & 15, own);
    }
    /* Register read: data on D0-D7. */
    part_drive_bus_cached(s, p, vic_data_pins, 8, c64->dout_val[DOUT_VIC], c64->dout_en[DOUT_VIC],
                          &c64->cache_vic_data);
}

/* The register address comes from the multiplexed pins, latched by the VIC
 * when /RAS falls in a CPU half. */
static void vic_commit(sim_t *s, part_t *p) {
    board_t *b = p->user;
    c64_t *c64 = C64(b);
    if (c64->step == STEP_RAS && !c64->vic_owns)
        c64->vic_reg = (uint8_t)part_read_bus(s, p, vic_mux_pins, 6);
}

static void vic_tick(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    part_t *p = P(b, c64->p_vic);
    c64->vic_fetches = 0;
    /* The VIC needs its dot clock (from U31) to do anything. */
    c64->vic_running = !p->dead && c64->seen_dot[0] != c64->seen_dot[1];
    /* Estimate for the fetches of this cycle: AEC goes low (the VIC takes
     * phi2 too) three cycles after BA went low. */
    c64->vic_steal = c64->vic_running && c64->vic_ba_low && c64->ba_streak >= 3;
    if (c64->vic_running) {
        uint64_t pins = m6569_tick_fetch(&c64->vic, 0);   /* calls vic_fetch() */
        c64->vic_ba_low = (pins & M6569_BA) != 0;
        c64->vic_irq = (pins & M6569_IRQ) != 0;
        c64->vic_last_tick = s->cycle;
    } else {
        c64->vic_ba_low = c64->vic_irq = false;
    }
    c64->ba_streak = c64->vic_ba_low ? c64->ba_streak + 1 : 0;
    c64->vic_steal = c64->vic_ba_low && c64->ba_streak >= 4;
    sim_mark_dirty(s, c64->p_vic);
    /* Every phi1 has a VIC access: when the VIC core had nothing to fetch
     * this is a DRAM refresh (address $3Fxx, an 8 bit counter). */
    if (c64->vic_fetches == 0) bus_half(b, 1, c64->vic_running, (uint16_t)(0x3F00 | c64->vic_refresh--));
}

static void vic_finish(board_t *b) {
    c64_t *c64 = C64(b);
    if (!c64->vic_running) return;
    sim_t *s = &b->sim;
    part_t *p = P(b, c64->p_vic);
    uint64_t pins = 0;
    if (!pin_in(s, p, 10)) {                         /* /CS: $D000-$D3FF */
        pins |= M6569_CS | (c64->vic_reg & 63);
        if (pin_in(s, p, 11)) pins |= M6569_RW;
        else M6569_SET_DATA(pins, part_read_bus(s, p, vic_data_pins, 8));
    }
    pins = m6569_tick_finish(&c64->vic, pins);
    if ((pins & (M6569_CS | M6569_RW)) == (M6569_CS | M6569_RW))
        set_dout(b, DOUT_VIC, c64->p_vic, M6569_GET_DATA(pins));
}

static void crt_set_pixel(void *fb, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= C64_SCREEN_W || y >= C64_SCREEN_H) return;
    ((uint32_t *)fb)[y * C64_SCREEN_W + x] = color;
}

/* ------------------------------------------------------------------------ */
/* 6526 CIA (U1, U2) and 6581 SID (U18)                                      */
/* ------------------------------------------------------------------------ */

static bool clock_toggles(const uint8_t seen[2]) { return seen[0] == 0 && seen[1] == 1; }

static void cia_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    c64_t *c64 = C64(b);
    int i = (int)(p - s->parts) == c64->p_cia[0] ? 0 : 1;
    m6526_t *c = &c64->cia[i];
    /* Port lines: an output 0 pulls down strongly, everything else is a
     * weak pull-up (so a key or a joystick can pull an output 1 low). */
    uint8_t lows[2] = { (uint8_t)(c->pa.ddr & ~c->pa.reg), (uint8_t)(c->pb.ddr & ~c->pb.reg) };
    int32_t *cache[2] = { &c64->cache_cia_pa[i], &c64->cache_cia_pb[i] };
    for (int port = 0; port < 2; port++) {
        if (*cache[port] == lows[port] + 1) continue;
        *cache[port] = lows[port] + 1;
        for (int bit = 0; bit < 8; bit++)
            pin_out(s, p, 2 + port * 8 + bit, (lows[port] >> bit) & 1 ? DRV_LO : DRV_PULLUP);
    }
    pin_out(s, p, 21, c64->cia_irq[i] ? DRV_LO : DRV_Z);               /* /IRQ */
    /* /PC: low for the cycle after an access to port B. SP and CNT: driven
     * by the serial port in output mode only (pull-ups on the board). */
    pin_out(s, p, 18, c->pc_low ? DRV_LO : DRV_HI);
    bool sp_out = (c->ta.cr & (1 << 6)) != 0;
    pin_out(s, p, 39, sp_out ? DRV_BIT(c->sp.sp_out) : DRV_Z);
    pin_out(s, p, 40, sp_out ? DRV_BIT(c->sp.cnt_out) : DRV_Z);
    part_drive_bus_cached(s, p, cia_data_pins, 8, c64->dout_val[DOUT_CIA1 + i], c64->dout_en[DOUT_CIA1 + i],
                          &c64->cache_cia_data[i]);
}

static void cia_tick(board_t *b, int i) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    part_t *p = P(b, c64->p_cia[i]);
    if (p->dead || !clock_toggles(c64->seen_phi2[i])) return;
    m6526_t *c = &c64->cia[i];
    if (!pin_in(s, p, 34)) {                     /* /RES held low */
        m6526_reset(c);
        c64->cia_irq[i] = false;
        sim_mark_dirty(s, c64->p_cia[i]);
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
    /* /FLAG acts on a falling edge: the core flags a 0 to 1 change of
     * M6526_FLAG, so the bit means "the pin is low". */
    if (!pin_in(s, p, 24)) pins |= M6526_FLAG;
    if (pin_in(s, p, 19)) pins |= M6526_TOD;     /* 50 Hz from the mains */
    if (pin_in(s, p, 39)) pins |= M6526_SP;
    if (pin_in(s, p, 40)) pins |= M6526_CNT;
    pins = m6526_tick(c, pins);
    /* The pins cia_eval() drives from the core's state. */
    uint8_t outs = (uint8_t)(c->pc_low | c->sp.sp_out << 1 | c->sp.cnt_out << 2 | ((c->ta.cr >> 6) & 1) << 3);
    if (outs != c64->cia_outs[i]) { c64->cia_outs[i] = outs; sim_mark_dirty(s, c64->p_cia[i]); }
    c64->cia_irq[i] = (pins & M6526_IRQ) != 0;
    if ((pins & (M6526_CS | M6526_RW)) == (M6526_CS | M6526_RW))
        set_dout(b, DOUT_CIA1 + i, c64->p_cia[i], M6526_GET_DATA(pins));
    sim_mark_dirty(s, c64->p_cia[i]);
}

static void sid_eval(sim_t *s, part_t *p) {
    board_t *b = p->user;
    c64_t *c64 = C64(b);
    part_drive_bus_cached(s, p, sid_data_pins, 8, c64->dout_val[DOUT_SID], c64->dout_en[DOUT_SID],
                          &c64->cache_sid_data);
}

static void sid_tick(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    part_t *p = P(b, c64->p_sid);
    if (p->dead || !clock_toggles(c64->seen_phi2[2])) return;
    if (!pin_in(s, p, 5)) { m6581_reset(&c64->sid); return; }
    uint64_t pins = 0;
    if (!pin_in(s, p, 8)) {                     /* /CS: a register access */
        pins = M6581_CS | part_read_bus(s, p, sid_addr_pins, 5);
        if (pin_in(s, p, 7)) pins |= M6581_RW;
        M6502_SET_DATA(pins, part_read_bus(s, p, sid_data_pins, 8));
    }
    /* POT X/Y: what the A/D converters measured on pins 24 and 23. */
    if ((pins & (M6581_CS | M6581_RW)) == (M6581_CS | M6581_RW)) {
        uint8_t reg = pins & M6581_ADDR_MASK;
        if (reg == M6581_POT_X) c64->sid.pot[0] = (uint8_t)board_pot_value(b, part_pin(p, 24));
        if (reg == M6581_POT_Y) c64->sid.pot[1] = (uint8_t)board_pot_value(b, part_pin(p, 23));
    }
    pins = m6581_tick(&c64->sid, pins);
    if ((pins & (M6581_CS | M6581_RW)) == (M6581_CS | M6581_RW))
        set_dout(b, DOUT_SID, c64->p_sid, M6502_GET_DATA(pins));
}

/* ------------------------------------------------------------------------ */
/* One cycle                                                                 */
/* ------------------------------------------------------------------------ */

/* The mains. The power supply brings 9 V AC to the board; R5, the zener
 * CR1 and R100 clip it, and with R37 U27 squares it up (a Schmitt
 * trigger): the 50 Hz of the CIAs' time of day clocks. That analog stage
 * is not simulated, so this is what it puts on U27's input, through R100:
 * a square wave at the mains frequency, 50 Hz (a PAL machine, in Europe).
 * Like the power supply, it is an input to the board; opening R5 or R100
 * cuts it, and U27 and the rest are simulated as usual. */
#define MAINS_HZ 50

static void mains_tick(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    if (c64->pin_mains < 0) return;
    /* The level flips every half period: (n * C64_FREQ) / (2 * MAINS_HZ). */
    if (s->cycle >= c64->mains_next) {
        c64->mains_edges = (s->cycle * (2 * MAINS_HZ)) / C64_FREQ + 1;
        c64->mains_next = (c64->mains_edges * C64_FREQ + 2 * MAINS_HZ - 1) / (2 * MAINS_HZ);
    }
    bool on = s->parts[c64->p_r5].rfault != RF_OPEN && s->parts[c64->p_r100].rfault != RF_OPEN;
    int level = (int)((c64->mains_edges - 1) & 1);
    sim_drive(s, c64->pin_mains, on ? DRIVE(S_STRONG, level) : DRV_Z);
}

/* The cassette motor driver: 6510 P5 through R4 drives Q2, which keeps
 * the Darlington Q1/Q3 off. With P5 low (or not driven: R3 pulls Q2's
 * base down) Q3 puts about 6 V on the MOTOR pin. Analog, so modeled here
 * by what it does: MOTOR is driven high while P5 isn't high. */
static void motor_tick(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    if (c64->pin_motor < 0 || c64->pin_motor_ctl < 0) return;
    const pin_t *c = &s->pins[c64->pin_motor_ctl];
    bool p5_high = c->root >= 0 && s->nets[c->root].strength != S_Z && s->nets[c->root].level;
    sim_drive(s, c64->pin_motor, p5_high ? DRV_Z : DRV_HI);
}

static void board_cycle(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    s->cycle++;
    mains_tick(b);
    motor_tick(b);
    harness_tick(b);
    cpu_tick(b);
    vic_tick(b);                                    /* phi1 (and maybe a stolen phi2) */
    if (!c64->vic_steal) bus_half(b, 2, false, 0);    /* phi2: the CPU's access */
    else if (c64->vic_fetches < 2) bus_half(b, 2, true, 0x3FFF);
    /* End of phi2: the chips with a core see the bus, then the CPU. */
    vic_finish(b);
    cia_tick(b, 0);
    cia_tick(b, 1);
    sid_tick(b);
    sim_settle(s);
    cpu_sample(b);
}

static void c64_run(board_t *b, int cycles) {
    for (int i = 0; i < cycles; i++) board_cycle(b);
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

/* Which nets are power rails, and what each part is, from the names of the
 * KiCad replica. */
static int c64_rail_of(const char *name) {
    if (!strcmp(name, "Earth")) return RAIL_GND;
    if (!strcmp(name, "+5V") || !strcmp(name, "+Vc") || !strcmp(name, "CAN+5V") ||
        !strcmp(name, "+12V") || !strcmp(name, "5V_joyport") || !strcmp(name, "5V_FB3"))
        return RAIL_VCC;
    return RAIL_NONE;
}

static int c64_kind_of(const char *ref) {
    if (ref[0] == 'U') return PART_IC;
    if (ref[0] == 'R') return PART_RESISTOR;   /* R and RP */
    if (!strncmp(ref, "FB", 2) || ref[0] == 'L' || ref[0] == 'F' ||
        (ref[0] == 'E' && ref[1] >= '0' && ref[1] <= '9')) return PART_LINK;
    if (!strncmp(ref, "CN", 2)) return PART_CONNECTOR;
    return PART_OTHER;
}

/* Every chip of the board. The eight DRAMs don't split the memory by
 * address: each one stores one bit of every byte, so RAMn holds data bit
 * n-1 of all of $0000-$FFFF. */
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

/* Joins of nets for board_rebuild(). Keyboard: a pressed key connects a PA
 * line to a PB line through the keyboard connector CN1. RESTORE connects
 * its line to ground. The dongles of the harness. And the crystal. */
static void c64_joins(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    for (int line = 0; line < 8; line++)
        for (int col = 0; col < 8; col++) {
            if (!((c64->key_matrix[line] >> col) & 1)) continue;
            int pa = c64->pin_cn1_pa[line], pb = c64->pin_cn1_pb[col];
            if (pa < 0 || pb < 0 || s->pins[pa].fault == PF_OPEN || s->pins[pb].fault == PF_OPEN) continue;
            sim_join(s, s->pins[pa].net, s->pins[pb].net);
        }
    if ((c64->key_matrix[8] & 1) && c64->pin_restore >= 0 && s->pins[c64->pin_restore].fault != PF_OPEN)
        sim_join(s, s->pins[c64->pin_restore].net, c64->net_gnd);
    harness_joins(b);
    vco_state_t *vco = s->parts[c64->p_u31].state;
    vco->crystal_ok = c64->p_y1 < 0 || s->parts[c64->p_y1].rfault != RF_OPEN;
}

static void init_keymap(board_t *b) {
    c64_t *c64 = C64(b);
    /* Same matrix as c64-llm/c64.h: line = CIA1 PA bit, column = PB bit. */
    kbd_init(&c64->kbd, 2);
    const char *keymap =
        "        3WA4ZSE 5RD6CFTX7YG8BHUV9IJ0MKON+PL-.:@,~*;  = /1  2  Q "
        "        #wa$zse %rd&cftx'yg(bhuv)ij0mkon pl >[ <$ ]    ?!  \"  q ";
    kbd_register_modifier(&c64->kbd, 0, 7, 1);    /* shift */
    kbd_register_modifier(&c64->kbd, 1, 2, 7);    /* ctrl */
    for (int shift = 0; shift < 2; shift++)
        for (int column = 0; column < 8; column++)
            for (int line = 0; line < 8; line++) {
                int c = keymap[shift * 64 + line * 8 + column];
                if (c != 0x20) kbd_register_key(&c64->kbd, c, column, line, shift ? 1 : 0);
            }
    kbd_register_key(&c64->kbd, C64_KEY_SPACE, 4, 7, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CSRLEFT, 2, 0, 1);
    kbd_register_key(&c64->kbd, C64_KEY_CSRRIGHT, 2, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CSRDOWN, 7, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CSRUP, 7, 0, 1);
    kbd_register_key(&c64->kbd, C64_KEY_DEL, 0, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_INST, 0, 0, 1);
    kbd_register_key(&c64->kbd, C64_KEY_HOME, 3, 6, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CLR, 3, 6, 1);
    kbd_register_key(&c64->kbd, C64_KEY_RETURN, 1, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CTRL, 2, 7, 0);
    kbd_register_key(&c64->kbd, C64_KEY_CBM, 5, 7, 0);
    kbd_register_key(&c64->kbd, C64_KEY_RESTORE, 0, 8, 0);
    kbd_register_key(&c64->kbd, C64_KEY_STOP, 7, 7, 0);
    kbd_register_key(&c64->kbd, C64_KEY_F1, 4, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_F3, 5, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_F5, 6, 0, 0);
    kbd_register_key(&c64->kbd, C64_KEY_F7, 3, 0, 0);
}

/* The keyboard connector pin that shares a net with the given CIA pin. */
static int cn1_pin_on_net_of(board_t *b, int part, int number) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    int net = s->pins[part_pin(&s->parts[part], number)].net;
    part_t *cn1 = &s->parts[c64->p_cn1];
    for (int i = 0; i < cn1->npins; i++)
        if (s->pins[cn1->first_pin + i].net == net) return cn1->first_pin + i;
    return -1;
}

/* The connectors, and what each pin is for (from the C64 service manual,
 * checked against the netlist: keyboard COL lines go to CIA1 port A, ROW
 * lines to port B; control port 1 is the one with the light pen). Pins are
 * named by their pad ("1", "A"...); '_' is shown as a space. */
static const conn_def_t conn_defs[] = {
    { "CN1", "KEYBOARD", "1=GND 3=RESTORE 4=+5V 5=ROW3 6=ROW6 7=ROW5 8=ROW4 9=ROW7 10=ROW2 "
      "11=ROW1 12=ROW0 13=COL0 14=COL6 15=COL5 16=COL4 17=COL3 18=COL2 19=COL1 20=COL7" },
    { "CN2", "USER PORT", "1=GND 2=+5V 3=/RESET 4=CNT1 5=SP1 6=CNT2 7=SP2 8=/PC2 9=SERIAL_ATN "
      "10=9V_AC 11=9V_AC 12=GND A=GND B=/FLAG2 C=PB0 D=PB1 E=PB2 F=PB3 H=PB4 J=PB5 K=PB6 L=PB7 "
      "M=PA2 N=GND" },
    { "CN3", "CASSETTE", "A=GND B=+5V C=MOTOR D=READ E=WRITE F=SENSE" },
    { "CN4", "SERIAL", "1=SRQ_IN 2=GND 3=ATN 4=CLK 5=DATA 6=/RESET" },
    { "CN5", "AUDIO/VIDEO", "1=LUMINANCE 2=GND 3=AUDIO_OUT 4=VIDEO_OUT 5=AUDIO_IN 6=CHROMA "
      "7=NOT_CONNECTED 8=NOT_CONNECTED" },
    { "CN6", "EXPANSION PORT", "1=GND 2=+5V 3=+5V 4=/IRQ 5=R/W 6=DOT_CLOCK 7=/I/O1 8=/GAME "
      "9=/EXROM 10=/I/O2 11=/ROML 12=BA 13=/DMA 14=D7 15=D6 16=D5 17=D4 18=D3 19=D2 20=D1 21=D0 "
      "22=GND A=GND B=/ROMH C=/RESET D=/NMI E=PHI2 F=A15 H=A14 J=A13 K=A12 L=A11 M=A10 N=A9 "
      "P=A8 R=A7 S=A6 T=A5 U=A4 V=A3 W=A2 X=A1 Y=A0 Z=GND" },
    { "CN7", "POWER", "1=GND 2=GND 3=GND 4=+5V 5=+5V 6=9V_AC 7=9V_AC" },
    { "CN8", "CONTROL PORT 2", "1=UP 2=DOWN 3=LEFT 4=RIGHT 5=POT_Y 6=FIRE 7=+5V 8=GND 9=POT_X" },
    { "CN9", "CONTROL PORT 1", "1=UP 2=DOWN 3=LEFT 4=RIGHT 5=POT_Y 6=FIRE/LIGHT_PEN 7=+5V 8=GND "
      "9=POT_X" },
    { "CN10", "POWER LED", "1=LED+ 2=LED- 3=LED+" },
};

static void c64_create(board_t *b) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    c64->p_cpu = sim_find_part(s, "U7");
    c64->p_vic = sim_find_part(s, "U19");
    c64->p_cia[0] = sim_find_part(s, "U1");
    c64->p_cia[1] = sim_find_part(s, "U2");
    c64->p_sid = sim_find_part(s, "U18");
    c64->p_u31 = sim_find_part(s, "U31");
    c64->p_y1 = sim_find_part(s, "Y1");
    c64->p_cn1 = sim_find_part(s, "CN1");
    c64->net_gnd = sim_find_net(s, "Earth");
    /* The big chips: models are here, around their cores. */
    s->parts[c64->p_cpu].eval = cpu_eval;
    s->parts[c64->p_vic].eval = vic_eval;
    s->parts[c64->p_vic].commit = vic_commit;
    s->parts[c64->p_cia[0]].eval = cia_eval;
    s->parts[c64->p_cia[1]].eval = cia_eval;
    s->parts[c64->p_sid].eval = sid_eval;
    /* ROM images. */
    ((rom_state_t *)s->parts[sim_find_part(s, "U3")].state)->data = dump_c64_basic_bin;
    ((rom_state_t *)s->parts[sim_find_part(s, "U4")].state)->data = dump_c64_kernalv3_bin;
    ((rom_state_t *)s->parts[sim_find_part(s, "U5")].state)->data = dump_c64_char_bin;
    /* Which DRAM holds which data bit: the one whose D pin is on net Dn. */
    for (int i = 0; i < s->nparts; i++) {
        if (s->parts[i].reset != PT_4164.reset) continue;
        int net = s->pins[part_pin(&s->parts[i], 2)].net;
        const char *name = net >= 0 ? s->nets[net].name : "";
        if (name[0] == 'D' && name[1] >= '0' && name[1] <= '7' && !name[2]) c64->dram_part[name[1] - '0'] = i;
    }
    /* Keyboard lines on CN1, and the RESTORE line. */
    for (int i = 0; i < 8; i++) {
        c64->pin_cn1_pa[i] = cn1_pin_on_net_of(b, c64->p_cia[0], 2 + i);
        c64->pin_cn1_pb[i] = cn1_pin_on_net_of(b, c64->p_cia[0], 10 + i);
    }
    c64->pin_restore = part_pin(&s->parts[c64->p_cn1], 3);
    timer556_state_t *t = s->parts[sim_find_part(s, "U20")].state;
    t->restore_net = s->pins[c64->pin_restore].net;
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
        sim_set_bus_bit(s, s->pins[part_pin(&s->parts[c64->p_vic], 38 - i)].net, bit++);
    c64->p_r5 = sim_find_part(s, "R5");
    c64->p_r100 = sim_find_part(s, "R100");
    c64->pin_mains = c64->p_r5 >= 0 && c64->p_r100 >= 0 ? part_pin(&s->parts[c64->p_r100], 2) : -1;
    b->p_pla = sim_find_part(s, "U17");
    c64->p_cn6 = sim_find_part(s, "CN6");
    c64->p_u28 = sim_find_part(s, "U28");
    int q3 = sim_find_part(s, "Q3");
    c64->pin_motor = q3 >= 0 ? part_pin(&s->parts[q3], 3) : -1;              /* emitter */
    c64->pin_motor_ctl = part_pin(&s->parts[c64->p_cpu], 24);                    /* P5 */
    c64->pin_hmotor = c64->pin_hwrite = -1;
    /* The RESET button sits on the user port, like a reset cartridge. */
    int user_port = sim_find_part(s, "CN2");
    b->pin_reset = board_pad_pin(b, user_port, "3");
    b->pin_reset_gnd = board_pad_pin(b, user_port, "1");
}

/* Cold start: like switching the C64 on (board_power_on() does the rest). */
static void c64_power_on(board_t *b) {
    c64_t *c64 = C64(b);
    c64->mains_next = 0;
    c64->cpu_pins = m6502_init(&c64->cpu, &(m6502_desc_t){
        .m6510_in_cb = cpu_port_in,
        .m6510_out_cb = cpu_port_out,
        .m6510_user_data = b,
        .m6510_io_pullup = 0,
        .m6510_io_floating = 0xC0,  /* bits 6-7 have no pins: they keep their value */
    });
    c64->cpu_in_reset = true;
    m6569_init(&c64->vic, &(m6569_desc_t){
        .fetch_cb = vic_fetch,
        .screen = { .x = 64, .y = 24, .width = C64_SCREEN_W, .height = C64_SCREEN_H },
        .user_data = b,
        .crt_set_pixel = crt_set_pixel,
        .crt_set_pixel_fb = b->screen,
    });
    m6526_init(&c64->cia[0]);
    m6526_init(&c64->cia[1]);
    m6581_init(&c64->sid, &(m6581_desc_t){ .tick_hz = C64_FREQ, .sound_hz = 44100, .magnitude = 1.0f });
    memset(b->screen, 0, C64_SCREEN_W * C64_SCREEN_H * sizeof(uint32_t));
    memset(c64->dout_en, 0, sizeof(c64->dout_en));
    c64->cia_irq[0] = c64->cia_irq[1] = false;
    c64->vic_ba_low = c64->vic_irq = false;
    c64->ba_streak = 0;
    /* Assume clocks are alive at power on, the first cycle will tell. */
    c64->seen_phi0[0] = c64->seen_dot[0] = c64->seen_color[0] = 0;
    c64->seen_phi0[1] = c64->seen_dot[1] = c64->seen_color[1] = 1;
    for (int i = 0; i < 3; i++) { c64->seen_phi2[i][0] = 0; c64->seen_phi2[i][1] = 1; }
}

/* ------------------------------------------------------------------------ */
/* Keyboard, PRG loading, video                                              */
/* ------------------------------------------------------------------------ */

/* If the set of closed key switches changed, rebuild the network. */
static void sync_keys(board_t *b) {
    c64_t *c64 = C64(b);
    bool changed = false;
    for (int line = 0; line < 9; line++) {
        uint16_t m = c64->kbd.scanout_column_masks[line];
        if (m != c64->key_matrix[line]) { c64->key_matrix[line] = m; changed = true; }
    }
    if (changed) board_rebuild(b);
}

static void c64_key_down(board_t *b, int key) { kbd_key_down(&C64(b)->kbd, key); sync_keys(b); }
static void c64_key_up(board_t *b, int key) { kbd_key_up(&C64(b)->kbd, key); sync_keys(b); }

/* Once per frame: release keys that were pressed long enough. 'us' is
 * emulated time, so a key stays down long enough for the KERNAL to scan it
 * even when the simulation is slower than real time. */
static void c64_frame_update(board_t *b, uint32_t us) {
    c64_t *c64 = C64(b);
    kbd_update(&c64->kbd, us);
    sync_keys(b);
}

/* Write a byte straight into the eight DRAM chips. */
static void ram_poke(board_t *b, uint16_t addr, uint8_t v) {
    c64_t *c64 = C64(b);
    int cell = ((addr & 0xFF) << 8) | (addr >> 8);     /* row = A0-A7, column = A8-A15 */
    for (int bit = 0; bit < 8; bit++) {
        dram_state_t *st = b->sim.parts[c64->dram_part[bit]].state;
        st->cells[cell] = (v >> bit) & 1;
    }
}

bool c64_load_prg(board_t *b, const char *path, bool run, char *err, int errlen) {
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
static void c64_monitor(board_t *b, uint32_t *out) {
    c64_t *c64 = C64(b);
    sim_t *s = &b->sim;
    part_t *vic = &s->parts[c64->p_vic];
    bool sync = !vic->dead && s->cycle - c64->vic_last_tick < 20000 &&
                s->pins[part_pin(vic, 15)].fault == PF_OK;
    bool color = c64->seen_color[0] != c64->seen_color[1] && s->pins[part_pin(vic, 14)].fault == PF_OK;
    static uint32_t seed = 12345;
    for (int i = 0; i < C64_SCREEN_W * C64_SCREEN_H; i++) {
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

/* A .prg goes into RAM; a .crt cartridge into the expansion port. */
static bool c64_load(board_t *b, const char *path, bool run, char *msg, int msglen) {
    size_t n = strlen(path);
    if (n > 4 && !strcasecmp(path + n - 4, ".crt")) return board_insert_cart(b, path, msg, msglen);
    return c64_load_prg(b, path, run, msg, msglen);
}

/* ------------------------------------------------------------------------ */
/* Reports                                                                   */
/* ------------------------------------------------------------------------ */

static const char *c64_cpu_status(board_t *b) {
    c64_t *c64 = C64(b);
    return !c64->cpu_running ? "stopped (no clock)" : c64->cpu_in_reset ? "in reset" : "running";
}

static void c64_cpu_regs(board_t *b, char *buf, int len) {
    c64_t *c64 = C64(b);
    snprintf(buf, len, "PC=%04X A=%02X X=%02X Y=%02X S=%02X %s", m6502_pc(&c64->cpu), m6502_a(&c64->cpu),
             m6502_x(&c64->cpu), m6502_y(&c64->cpu), m6502_s(&c64->cpu),
             !c64->cpu_running ? "(no clock)" : c64->cpu_in_reset ? "(in reset)" : "");
}

/* Read the text on the monitor picture, like a person would: match every
 * 8x8 cell of the text area against the character ROM glyphs. Cells that
 * match no glyph are shown as '?'. This looks at the PICTURE, not at the
 * screen memory, so it shows what a broken machine really displays. */
static int match_cell(const uint32_t *px, int x0, int y0, uint32_t bg) {
    uint8_t cell[8];
    for (int y = 0; y < 8; y++) {
        cell[y] = 0;
        for (int x = 0; x < 8; x++)
            if (px[(y0 + y) * C64_SCREEN_W + x0 + x] != bg) cell[y] |= (uint8_t)(0x80 >> x);
    }
    for (int c = 0; c < 128; c++) {
        bool same = true, inv = true;
        for (int y = 0; y < 8; y++) {
            if (dump_c64_char_bin[c * 8 + y] != cell[y]) same = false;
            if ((uint8_t)~dump_c64_char_bin[c * 8 + y] != cell[y]) inv = false;
        }
        if (same || inv) return c;
    }
    return -1;
}

static void c64_print_text(board_t *b) {
    static uint32_t px[C64_SCREEN_W * C64_SCREEN_H];
    c64_monitor(b, px);
    /* The text window starts where the border ends (32,36 on a healthy
     * machine); if there is no border, assume the normal position. */
    uint32_t border = px[2 * C64_SCREEN_W + 2];
    int ox = 32, oy = 36;
    for (int y = 20; y < 60; y++) if (px[y * C64_SCREEN_W + C64_SCREEN_W / 2] != border) { oy = y; break; }
    for (int x = 16; x < 60; x++) if (px[(oy + 100) * C64_SCREEN_W + x] != border) { ox = x; break; }
    uint32_t bg = px[(oy + 100) * C64_SCREEN_W + ox + 4];
    for (int row = 0; row < 25; row++) {
        char line[41];
        for (int col = 0; col < 40; col++) {
            int code = match_cell(px, ox + col * 8, oy + row * 8, bg);
            char ch = '?';
            if (code == 0) ch = '@';
            else if (code >= 1 && code <= 26) ch = (char)('A' + code - 1);
            else if (code >= 27 && code < 64) ch = "[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?"[code - 27];
            else if (code >= 64) ch = '#';
            line[col] = ch;
        }
        line[40] = 0;
        int end = 40;
        while (end > 0 && line[end - 1] == ' ') line[--end] = 0;
        printf("|%s\n", line);
    }
}

/* ------------------------------------------------------------------------ */
/* The host keyboard                                                         */
/* ------------------------------------------------------------------------ */

static int c64_host_key(int keycode) {
    switch (keycode) {
    case SAPP_KEYCODE_ENTER: return C64_KEY_RETURN;
    case SAPP_KEYCODE_BACKSPACE: return C64_KEY_DEL;
    case SAPP_KEYCODE_LEFT: return C64_KEY_CSRLEFT;
    case SAPP_KEYCODE_RIGHT: return C64_KEY_CSRRIGHT;
    case SAPP_KEYCODE_UP: return C64_KEY_CSRUP;
    case SAPP_KEYCODE_DOWN: return C64_KEY_CSRDOWN;
    case SAPP_KEYCODE_HOME: return C64_KEY_HOME;
    case SAPP_KEYCODE_ESCAPE: return C64_KEY_STOP;
    case SAPP_KEYCODE_PAGE_UP: return C64_KEY_RESTORE;
    case SAPP_KEYCODE_TAB: return C64_KEY_CTRL;
    case SAPP_KEYCODE_LEFT_ALT: return C64_KEY_CBM;
    case SAPP_KEYCODE_F1: return C64_KEY_F1;
    case SAPP_KEYCODE_F3: return C64_KEY_F3;
    case SAPP_KEYCODE_F5: return C64_KEY_F5;
    case SAPP_KEYCODE_F7: return C64_KEY_F7;
    default: return 0;
    }
}

/* The C64 types uppercase letters without shift. */
static int c64_host_char(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

const machine_t machine_c64 = {
    .id = "c64",
    .name = "Commodore 64",
    .board_name = "C64 ASSY 250407",
    .size = sizeof(c64_t),
    .data = &bd_c64,
    .sim = { .rail_of = c64_rail_of, .kind_of = c64_kind_of, .cycles_per_us = 1.0 },
    .cpu_hz = C64_FREQ,
    .cycles_per_frame = C64_CYCLES_PER_FRAME,
    .steps_per_cycle = C64_STEPS_PER_CYCLE,
    .screen_w = C64_SCREEN_W, .screen_h = C64_SCREEN_H,
    .ready_cycles = 3 * C64_FREQ,
    .parts = part_defs, .nparts = sizeof(part_defs) / sizeof(part_defs[0]),
    .conns = conn_defs, .nconns = sizeof(conn_defs) / sizeof(conn_defs[0]),
    .link_resistors = "RP1 RP2 R42",
    .crystals = "Y1",
    .keys_help = "Keys: ESC=RUN/STOP  PgUp=RESTORE  TAB=CTRL  ALT=C=",
    .create = c64_create,
    .power_on = c64_power_on,
    .run = c64_run,
    .joins = c64_joins,
    .key_down = c64_key_down,
    .key_up = c64_key_up,
    .frame_update = c64_frame_update,
    .host_key = c64_host_key,
    .host_char = c64_host_char,
    .monitor = c64_monitor,
    .load = c64_load,
    .cpu_status = c64_cpu_status,
    .cpu_regs = c64_cpu_regs,
    .print_text = c64_print_text,
};
