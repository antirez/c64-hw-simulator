/* parts.c - pin-level models of the C64 glue logic and memories.
 *
 * Every model follows the same pattern:
 *   eval()   reads input pins (pread) and drives output pins (pdrive).
 *            It must have no side effects other than driving pins.
 *   commit() is called after the board settled, at every bus step: this is
 *            where latches and memories store what they see (clock edges).
 *
 * Pin numbers always follow the chip datasheet, so the code can be checked
 * against a pinout diagram. */
#include "parts.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

void part_set_type(sim_t *s, int part, const part_type_t *t) {
    part_t *p = &s->parts[part];
    p->name = t->name;
    p->eval = t->eval;
    p->commit = t->commit;
    p->reset = t->reset;
    p->tech = (uint8_t)t->tech;
    if (t->state_size) p->state = calloc(1, t->state_size);
    /* Parse "NAME:K NAME:K ..." into pin names and flags. */
    char *copy = strdup(t->pins), *save = NULL;
    int number = 1;
    for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save), number++) {
        char *colon = strchr(tok, ':');
        uint8_t flags = 0;
        if (colon) {
            *colon = 0;
            switch (colon[1]) {
            case 'T': flags = PIN_TTL | PIN_SENSE | PIN_IN; break;
            case 'M': flags = PIN_SENSE | PIN_IN; break;
            case 'm': flags = PIN_IN; break;
            case 'O': flags = PIN_OUT; break;
            case 'B': flags = PIN_IN | PIN_OUT; break;
            case 'P': flags = PIN_POWER; break;
            case 'S': flags = PIN_SENSE | PIN_SWITCH | PIN_IN | PIN_OUT; break;
            default: flags = 0; break;
            }
        }
        int pi = p->pinmap[number < 48 ? number : 0];
        if (number < 48 && pi >= 0) {
            s->pin_names[pi] = strdup(tok);
            s->pins[pi].flags = flags;
        }
    }
    free(copy);
}

unsigned part_read_bus(sim_t *s, part_t *p, const int *pins, int n) {
    return sim_read_group(s, p, pins, n);
}

void part_drive_bus(sim_t *s, part_t *p, const int *pins, int n, unsigned v, bool enable) {
    for (int i = 0; i < n; i++)
        pin_out(s, p, pins[i], enable ? DRV_BIT((v >> i) & 1) : DRV_Z);
}

/* ------------------------------------------------------------------------ */
/* The big chips: only pin descriptions here, the models are in board.c     */
/* ------------------------------------------------------------------------ */

const part_type_t PT_6510 = { "6510 CPU",
    "PHI0:M RDY:m /IRQ:m /NMI:m AEC:M VCC:P A0:O A1:O A2:O A3:O A4:O A5:O A6:O A7:O "
    "A8:O A9:O A10:O A11:O A12:O A13:O GND:P A14:O A15:O P5:B P4:B P3:B P2:B P1:B P0:B "
    "D7:B D6:B D5:B D4:B D3:B D2:B D1:B D0:B R/W:O PHI2:O /RES:m", 0, 0, 0, 0, TECH_NMOS };

const part_type_t PT_6569 = { "6569 VIC-II",
    "D6:B D5:B D4:B D3:B D2:B D1:B D0:B /IRQ:O LP:m /CS:m R/W:m BA:O VDD:P COLOR:O "
    "S/LUM:O AEC:O PHI0:O /RAS:O /CAS:O GND:P PHICOL:m PHIIN:m A11:O A0/A8:B A1/A9:B "
    "A2/A10:B A3/A11:B A4/A12:B A5/A13:B A6:O A7:O A8:O A9:O A10:O D11:B D10:B D9:B "
    "D8:B D7:B VCC:P", 0, 0, 0, 0, TECH_NMOS };

const part_type_t PT_6526 = { "6526 CIA",
    "GND:P PA0:B PA1:B PA2:B PA3:B PA4:B PA5:B PA6:B PA7:B PB0:B PB1:B PB2:B PB3:B "
    "PB4:B PB5:B PB6:B PB7:B /PC:O TOD:m VCC:P /IRQ:O R/W:m /CS:m /FLAG:m PHI2:m "
    "D7:B D6:B D5:B D4:B D3:B D2:B D1:B D0:B /RES:m RS3:m RS2:m RS1:m RS0:m SP:O CNT:O",
    0, 0, 0, 0, TECH_NMOS };

const part_type_t PT_6581 = { "6581 SID",
    "CAP1A:O CAP1B:O CAP2A:O CAP2B:O /RES:m PHI2:m R/W:m /CS:m A0:m A1:m A2:m A3:m A4:m "
    "GND:P D0:B D1:B D2:B D3:B D4:B D5:B D6:B D7:B POTY:m POTX:m VCC:P EXTIN:m "
    "AUDIO:O VDD:P", 0, 0, 0, 0, TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 82S100 PLA (U17)                                                          */
/* ------------------------------------------------------------------------ */

/* The logic equations of the C64 PLA, from the 82S100 read-out by Mark Smith
 * (ABEL file in "C64 PLA logic equations", Marko Mäkelä et al.), written as
 * the chip is made: an AND array of product terms (every term requires some
 * inputs high and some low) and an OR array (every output is true when any
 * of its terms is). The inspector in the UI shows this same table live.
 *
 * Inputs are pin levels, named as in the schematic: /LORAM=1 means that pin
 * is high (BASIC visible). The PLA is bipolar, so its inputs are TTL: a
 * floating input reads 1. */
const int pla_in_pin[PLA_NIN] = { 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 26, 27 };
const int pla_out_pin[PLA_NOUT] = { 10, 11, 12, 13, 15, 16, 17, 18 };

#define HI(x) ((1u << PI_##x) | (1u << (PI_##x + 16)))     /* input x high */
#define LO(x) (1u << PI_##x)                                 /* input x low */
#define TERM(spec, outs) { (uint16_t)((spec) & 0xFFFF), (uint16_t)((spec) >> 16), (uint8_t)(outs) }
#define PO(x) (1u << PO_##x)
#define T_CPU LO(AEC)                                       /* the CPU has the bus */
#define T_VIC HI(AEC)                                       /* the VIC has the bus */
#define T_IO  (HI(A15) | HI(A14) | LO(A13) | HI(A12))       /* $D000-$DFFF */

/* Every term but the color RAM write also feeds /CASRAM: RAM is selected
 * only when nothing else is. */
const pla_term_t pla_terms[] = {
    /* /BASIC: BASIC ROM at $A000-$BFFF. */
    TERM(HI(LORAM) | HI(HIRAM) | HI(A15) | LO(A14) | HI(A13) | T_CPU | HI(RW) | HI(GAME), PO(BASIC) | PO(CASRAM)),
    /* /KERNAL: KERNAL ROM at $E000-$FFFF. */
    TERM(HI(HIRAM) | HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(RW) | HI(GAME), PO(KERNAL) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(KERNAL) | PO(CASRAM)),
    /* /CHAROM: character ROM at $D000-$DFFF for the CPU, $1000/$9000 for the VIC. */
    TERM(HI(HIRAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | HI(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(LORAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | HI(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(HIRAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(VA14) | T_VIC | HI(GAME) | LO(VA13) | HI(VA12), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(VA14) | T_VIC | LO(EXROM) | LO(GAME) | LO(VA13) | HI(VA12), PO(CHAROM) | PO(CASRAM)),
    /* /IO: the I/O chips at $D000-$DFFF. CPU reads only while BA is high, so
     * a read stalled by the VIC has no side effects. */
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(T_IO | T_CPU | HI(BA) | HI(RW) | HI(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(T_IO | T_CPU | LO(RW) | HI(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    /* GR/W: write to the color RAM. */
    TERM(LO(CAS) | T_IO | T_CPU | LO(RW), PO(GRW)),
    /* /ROML: cartridge ROM at $8000-$9FFF. */
    TERM(HI(LORAM) | HI(HIRAM) | HI(A15) | LO(A14) | LO(A13) | T_CPU | HI(RW) | LO(EXROM), PO(ROML) | PO(CASRAM)),
    TERM(HI(A15) | LO(A14) | LO(A13) | T_CPU | HI(EXROM) | LO(GAME), PO(ROML) | PO(CASRAM)),
    /* /ROMH: cartridge ROM at $A000 or $E000 (Ultimax), and for the VIC. */
    TERM(HI(HIRAM) | HI(A15) | LO(A14) | HI(A13) | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(ROMH) | PO(CASRAM)),
    TERM(HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(EXROM) | LO(GAME), PO(ROMH) | PO(CASRAM)),
    TERM(T_VIC | HI(EXROM) | LO(GAME) | HI(VA13) | HI(VA12), PO(ROMH) | PO(CASRAM)),
    /* Ultimax mode leaves holes where nothing, not even RAM, answers. */
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | LO(A14) | HI(A12), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | LO(A14) | HI(A13), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | HI(A14), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | HI(A15) | LO(A14) | HI(A13), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | HI(A15) | HI(A14) | LO(A13) | LO(A12), PO(CASRAM)),
    /* RAM is selected only while the VIC's /CAS is low: the PLA gates the
     * DRAM column strobe. */
    TERM(HI(CAS), PO(CASRAM)),
};
const int pla_nterms = sizeof(pla_terms) / sizeof(pla_terms[0]);

unsigned pla_read_inputs(sim_t *s, part_t *p) {
    unsigned in = 0;
    for (int k = 0; k < PLA_NIN; k++) in |= (unsigned)pin_in(s, p, pla_in_pin[k]) << k;
    return in;
}

unsigned pla_logic(unsigned in) {
    unsigned any = 0;
    for (int t = 0; t < pla_nterms; t++)
        if ((in & pla_terms[t].mask) == pla_terms[t].value) any |= pla_terms[t].outputs;
    /* The selects are active low; /CASRAM is high when RAM is NOT selected. */
    return (any ^ ~(1u << PO_CASRAM)) & 0xFF;
}

static void pla_eval(sim_t *s, part_t *p) {
    unsigned out = pla_logic(pla_read_inputs(s, p));
    bool en = !pin_in(s, p, 19);            /* /OE, tied to GND */
    for (int k = 0; k < PLA_NOUT; k++)
        pin_out(s, p, pla_out_pin[k], en ? DRV_BIT((out >> k) & 1) : DRV_Z);
}

const part_type_t PT_82S100 = { "82S100 PLA",
    "FE:O A13:T A14:T A15:T /VA14:T /CHAREN:T /HIRAM:T /LORAM:T /CAS:T /ROMH:O /ROML:O "
    "/IO:O GR/W:O GND:P /CHAROM:O /KERNAL:O /BASIC:O /CASRAM:O /OE:T VA12:T VA13:T "
    "/GAME:T /EXROM:T R/W:T /AEC:T BA:T A12:T VCC:P", pla_eval, 0, 0, 0, TECH_TTL };

/* ------------------------------------------------------------------------ */
/* ROMs: 2364 (8K, BASIC and KERNAL) and 2332 (4K, characters)               */
/* ------------------------------------------------------------------------ */

static const int rom_addr_pins[13] = { 8, 7, 6, 5, 4, 3, 2, 1, 23, 22, 19, 18, 21 };
static const int rom_data_pins[8] = { 9, 10, 11, 13, 14, 15, 16, 17 };

static void rom2364_eval(sim_t *s, part_t *p) {
    rom_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20);
    unsigned addr = selected ? part_read_bus(s, p, rom_addr_pins, 13) : 0;
    part_drive_bus_cached(s, p, rom_data_pins, 8, st->data ? st->data[addr] : 0xFF, selected, &st->out_cache);
}

static void rom2332_eval(sim_t *s, part_t *p) {
    rom_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20) && pin_in(s, p, 21);   /* CS1 low, CS2 high */
    unsigned addr = selected ? part_read_bus(s, p, rom_addr_pins, 12) : 0;
    part_drive_bus_cached(s, p, rom_data_pins, 8, st->data ? st->data[addr] : 0xFF, selected, &st->out_cache);
}

const part_type_t PT_2364 = { "2364 ROM",
    "A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O "
    "D7:O A11:M A10:M /CS:M A12:M A9:M A8:M VCC:P", rom2364_eval, 0, 0, sizeof(rom_state_t), TECH_NMOS };

const part_type_t PT_2332 = { "2332 ROM",
    "A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O "
    "D7:O A11:M A10:M /CS1:M CS2:M A9:M A8:M VCC:P", rom2332_eval, 0, 0, sizeof(rom_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 2114 static RAM 1K x 4 (color RAM, U6)                                    */
/* ------------------------------------------------------------------------ */

static const int sram_addr_pins[10] = { 5, 6, 7, 4, 3, 2, 1, 17, 16, 15 };
static const int sram_io_pins[4] = { 14, 13, 12, 11 };

static void sram_eval(sim_t *s, part_t *p) {
    sram_state_t *st = p->state;
    bool read = !pin_in(s, p, 8) && pin_in(s, p, 10);         /* /CS low, /WE high */
    unsigned addr = read ? part_read_bus(s, p, sram_addr_pins, 10) : 0;
    part_drive_bus_cached(s, p, sram_io_pins, 4, st->cells[addr], read, &st->out_cache);
}

static void sram_commit(sim_t *s, part_t *p) {
    sram_state_t *st = p->state;
    if (!pin_in(s, p, 8) && !pin_in(s, p, 10))                 /* /CS low, /WE low */
        st->cells[part_read_bus(s, p, sram_addr_pins, 10)] = (uint8_t)part_read_bus(s, p, sram_io_pins, 4);
}

static void sram_reset(sim_t *s, part_t *p) {
    (void)s;
    sram_state_t *st = p->state;
    for (int i = 0; i < 1024; i++) st->cells[i] = (uint8_t)((i * 7 + (i >> 4)) & 15);
}

const part_type_t PT_2114 = { "2114 SRAM",
    "A6:M A5:M A4:M A3:M A0:M A1:M A2:M /CS:M GND:P /WE:M IO4:B IO3:B IO2:B IO1:B "
    "A9:M A8:M A7:M VCC:P", sram_eval, sram_commit, sram_reset, sizeof(sram_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 4164 dynamic RAM 64K x 1 (U9-U12, U21-U24)                                */
/* ------------------------------------------------------------------------ */

/* The DRAM receives 8 multiplexed address lines: the row is latched when
 * /RAS falls, the column when /CAS falls. On the C64 a CPU address maps to
 * row = A0-A7, column = A8-A15. Each chip stores one bit of every byte. */
static const int dram_addr_pins[8] = { 5, 7, 6, 12, 11, 10, 13, 9 };

/* Everything a DRAM does happens on the edges of /RAS and /CAS, so the
 * work is in commit(); eval() just drives Q (pin 14) during a read. */
static void dram_eval(sim_t *s, part_t *p) {
    dram_state_t *st = p->state;
    pin_out(s, p, 14, st->out_en ? DRV_BIT(st->out_bit) : DRV_Z);
}

static void dram_commit(sim_t *s, part_t *p) {
    dram_state_t *st = p->state;
    int ras = pin_in(s, p, 4), cas = pin_in(s, p, 15);
    uint8_t out_en = st->out_en, out_bit = st->out_bit;
    if (st->ras_prev && !ras) st->row = (uint8_t)part_read_bus(s, p, dram_addr_pins, 8);
    if (st->cas_prev && !cas && !ras) {
        st->col = (uint8_t)part_read_bus(s, p, dram_addr_pins, 8);
        int cell = (st->row << 8) | st->col;
        if (!pin_in(s, p, 3)) {          /* /WE low before /CAS: early write */
            st->cells[cell] = (uint8_t)pin_in(s, p, 2);
            st->out_en = 0;
        } else {                        /* read: Q drives until /CAS rises */
            st->out_en = 1;
            st->out_bit = st->cells[cell];
        }
    }
    if (cas || ras) st->out_en = 0;
    st->ras_prev = (uint8_t)ras;
    st->cas_prev = (uint8_t)cas;
    if (st->out_en != out_en || st->out_bit != out_bit) sim_mark_dirty(s, (int)(p - s->parts));
}

/* Power-on pattern used by c64-llm: 64 bytes of $00, then 64 of $FF. With
 * row = A0-A7 and column = A8-A15 the address of a cell is (col << 8) | row. */
static void dram_reset(sim_t *s, part_t *p) {
    (void)s;
    dram_state_t *st = p->state;
    for (int row = 0; row < 256; row++)
        for (int col = 0; col < 256; col++) {
            int addr = (col << 8) | row;
            st->cells[(row << 8) | col] = (addr & 64) ? 1 : 0;
        }
    st->ras_prev = st->cas_prev = 1;
    st->out_en = 0;
}

const part_type_t PT_4164 = { "4164",
    "NC:O D:m /WE:m /RAS:m A0:m A2:m A1:m VCC:P A7:m A5:m A4:m A3:m A6:m Q:O /CAS:m GND:P",
    dram_eval, dram_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* TTL glue                                                                  */
/* ------------------------------------------------------------------------ */

/* 74LS08: quad 2-input AND. */
static void ls08_eval(sim_t *s, part_t *p) {
    static const int g[4][3] = { {1, 2, 3}, {4, 5, 6}, {9, 10, 8}, {12, 13, 11} };
    for (int i = 0; i < 4; i++)
        pin_out(s, p, g[i][2], DRV_BIT(pin_in(s, p, g[i][0]) && pin_in(s, p, g[i][1])));
}
const part_type_t PT_74LS08 = { "74LS08",
    "1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3A:T 3B:T 4Y:O 4A:T 4B:T VCC:P",
    ls08_eval, 0, 0, 0, TECH_TTL };

/* 7406: hex inverter with open collector outputs: it can only pull low. */
static void ls06_eval(sim_t *s, part_t *p) {
    static const int g[6][2] = { {1, 2}, {3, 4}, {5, 6}, {9, 8}, {11, 10}, {13, 12} };
    for (int i = 0; i < 6; i++)
        pin_out(s, p, g[i][1], pin_in(s, p, g[i][0]) ? DRV_LO : DRV_Z);
}
const part_type_t PT_7406 = { "7406",
    "1A:T 1Y:O 2A:T 2Y:O 3A:T 3Y:O GND:P 4Y:O 4A:T 5Y:O 5A:T 6Y:O 6A:T VCC:P",
    ls06_eval, 0, 0, 0, TECH_TTL_OC };

/* 74LS139: dual 2-to-4 decoder, active low outputs. */
static void ls139_eval(sim_t *s, part_t *p) {
    static const int half[2][7] = { {1, 2, 3, 4, 5, 6, 7}, {15, 14, 13, 12, 11, 10, 9} };
    for (int h = 0; h < 2; h++) {
        const int *pin = half[h];
        bool enabled = !pin_in(s, p, pin[0]);
        int sel = pin_in(s, p, pin[1]) | (pin_in(s, p, pin[2]) << 1);
        for (int y = 0; y < 4; y++) pin_out(s, p, pin[3 + y], DRV_BIT(!(enabled && sel == y)));
    }
}
const part_type_t PT_74LS139 = { "74LS139",
    "1/G:T 1A:T 1B:T 1Y0:O 1Y1:O 1Y2:O 1Y3:O GND:P 2Y3:O 2Y2:O 2Y1:O 2Y0:O 2B:T 2A:T "
    "2/G:T VCC:P", ls139_eval, 0, 0, 0, TECH_TTL };

/* 74LS257 / 74LS258: quad 2-to-1 multiplexer (258 has inverted outputs).
 * S low selects the A inputs, S high selects B. /G high: outputs off. */
static void mux_eval(sim_t *s, part_t *p, bool invert) {
    static const int sec[4][3] = { {2, 3, 4}, {5, 6, 7}, {11, 10, 9}, {14, 13, 12} };
    static const int outputs[4] = { 4, 7, 9, 12 };
    bool enabled = !pin_in(s, p, 15);
    int sel = pin_in(s, p, 1);
    unsigned v = 0;
    for (int i = 0; i < 4; i++) v |= (unsigned)(pin_in(s, p, sel ? sec[i][1] : sec[i][0]) ^ invert) << i;
    part_drive_bus(s, p, outputs, 4, v, enabled);
}
static void ls257_eval(sim_t *s, part_t *p) { mux_eval(s, p, false); }
static void ls258_eval(sim_t *s, part_t *p) { mux_eval(s, p, true); }
#define MUX_PINS "S:T 1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3B:T 3A:T 4Y:O 4B:T 4A:T /G:T VCC:P"
const part_type_t PT_74LS257 = { "74LS257", MUX_PINS, ls257_eval, 0, 0, 0, TECH_TTL };
const part_type_t PT_74LS258 = { "74LS258", MUX_PINS, ls258_eval, 0, 0, 0, TECH_TTL };

/* 74LS373: octal transparent latch. While LE is high the outputs follow the
 * inputs; when LE goes low the last value is held. /OE high: outputs off. */
typedef struct { uint8_t latch; int32_t out_cache; } ls373_state_t;
static const int ls373_d[8] = { 3, 4, 7, 8, 13, 14, 17, 18 };
static const int ls373_q[8] = { 2, 5, 6, 9, 12, 15, 16, 19 };

static void ls373_eval(sim_t *s, part_t *p) {
    ls373_state_t *st = p->state;
    bool enabled = !pin_in(s, p, 1);
    unsigned v = enabled && pin_in(s, p, 11) ? part_read_bus(s, p, ls373_d, 8) : st->latch;
    part_drive_bus_cached(s, p, ls373_q, 8, v, enabled, &st->out_cache);
}
static void ls373_commit(sim_t *s, part_t *p) {
    ls373_state_t *st = p->state;
    if (pin_in(s, p, 11)) st->latch = (uint8_t)part_read_bus(s, p, ls373_d, 8);
}
const part_type_t PT_74LS373 = { "74LS373",
    "/OE:T 1Q:O 1D:T 2D:T 2Q:O 3Q:O 3D:T 4D:T 4Q:O GND:P LE:T 5Q:O 5D:T 6D:T 6Q:O 7Q:O "
    "7D:T 8D:T 8Q:O VCC:P", ls373_eval, ls373_commit, 0, sizeof(ls373_state_t), TECH_TTL };

/* 4066: four bidirectional analog switches. When a switch is on, each side
 * gets whatever drives the other side (excluding the switch itself), so a
 * signal passes through in either direction. */
static void cd4066_eval(sim_t *s, part_t *p) {
    static const int sw[4][3] = { {1, 2, 13}, {4, 3, 5}, {8, 9, 6}, {11, 10, 12} };
    for (int i = 0; i < 4; i++) {
        int a = part_pin(p, sw[i][0]), b = part_pin(p, sw[i][1]);
        if (a < 0 || b < 0) continue;
        if (pin_in(s, p, sw[i][2])) {
            drive_t from_a = sim_drive_except(s, a, a);
            drive_t from_b = sim_drive_except(s, b, b);
            sim_drive(s, a, from_b);
            sim_drive(s, b, from_a);
        } else {
            sim_drive(s, a, DRV_Z);
            sim_drive(s, b, DRV_Z);
        }
    }
}
const part_type_t PT_4066 = { "4066",
    "1A:S 1B:S 2B:S 2A:S C2:M C3:M VSS:P 3A:S 3B:S 4B:S 4A:S C4:M C1:M VDD:P",
    cd4066_eval, 0, 0, 0, TECH_CMOS };

/* 556 dual timer (U20), modeled by what it does on the C64:
 *  - timer 2: holds the system in reset for a while after power-on (OUT2
 *    high, inverted by U8 into /RESET low);
 *  - timer 1: turns the RESTORE key press into a clean pulse (OUT1 high,
 *    inverted by U8 into /NMI low).
 * The RESTORE key reaches TRIG1 through a capacitor (C38) we don't simulate,
 * so the model watches the RESTORE net directly. */
#define RESET_CYCLES   50000    /* ~50 ms */
#define NMI_CYCLES      1000    /* ~1 ms */

static void t556_eval(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    pin_out(s, p, 5, DRV_BIT(s->cycle < st->nmi_until));
    pin_out(s, p, 9, DRV_BIT(s->cycle < st->reset_until));
}
static void t556_commit(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    if (st->last_cycle == s->cycle) return;     /* nothing new in this cycle */
    st->last_cycle = s->cycle;
    uint8_t restore = 1;
    if (st->restore_net >= 0) restore = s->nets[s->nets[st->restore_net].root].level;
    if (st->restore_prev && !restore) st->nmi_until = s->cycle + NMI_CYCLES;
    st->restore_prev = restore;
    bool out1 = s->cycle < st->nmi_until, out2 = s->cycle < st->reset_until;
    if (out1 != (s->pins[part_pin(p, 5)].drive == DRV_HI) ||
        out2 != (s->pins[part_pin(p, 9)].drive == DRV_HI))
        sim_mark_dirty(s, (int)(p - s->parts));
}
static void t556_reset(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    st->reset_until = s->cycle + RESET_CYCLES;
    st->nmi_until = 0;
    st->restore_prev = 1;
    st->last_cycle = UINT64_MAX;
}
const part_type_t PT_556 = { "556 TIMER",
    "DIS1:O THR1:O CV1:O /RES1:m OUT1:O TRIG1:m GND:P TRIG2:m OUT2:O /RES2:m CV2:O THR2:O "
    "DIS2:O VCC:P", t556_eval, t556_commit, t556_reset, sizeof(timer556_state_t), TECH_TTL };

/* 74LS629: two voltage controlled oscillators (U31). On the C64 one makes
 * the dot clock for the VIC (pin 7), the other, with the crystal Y1, the
 * color clock (pin 10). Real clocks are MHz signals: here they toggle once
 * per half bus cycle, enough for the board to know they are alive. */
static void ls629_eval(sim_t *s, part_t *p) {
    vco_state_t *st = p->state;
    pin_out(s, p, 7, DRV_BIT(s->clock_level));
    pin_out(s, p, 10, st->crystal_ok ? DRV_BIT(s->clock_level) : DRV_LO);
}
const part_type_t PT_74LS629 = { "74LS629 VCO",
    "2FC:O 1FC:O 1RNG:O 1CX:O 1CX:O /1EN:m 1Y:O OSCGND:P GND:P 2Y:O /2EN:m 2CX:O 2CX:O "
    "2RNG:O OSCVCC:O VCC:P", ls629_eval, 0, 0, sizeof(vco_state_t), TECH_TTL };

/* The rest of the PLL is not simulated: removing these chips changes nothing. */
const part_type_t PT_74LS74 = { "74LS74",
    "/1CLR:m 1D:m 1CK:m /1PR:m 1Q:O /1Q:O GND:P /2Q:O 2Q:O /2PR:m 2CK:m 2D:m /2CLR:m VCC:P",
    0, 0, 0, 0, TECH_TTL };
const part_type_t PT_74LS193 = { "74LS193",
    "B:m QB:O QA:O DOWN:m UP:m QC:O QD:O GND:P D:m C:m /LOAD:m /CO:O /BO:O CLR:m A:m VCC:P",
    0, 0, 0, 0, TECH_TTL };
const part_type_t PT_MC4044 = { "MC4044 PLL",
    "R:m 2:O V:m 4:O 5:O 6:O GND:P 8:O 9:O 10:O 11:O 12:O 13:O VCC:P", 0, 0, 0, 0, TECH_TTL };

/* ------------------------------------------------------------------------ */
/* Resistors                                                                 */
/* ------------------------------------------------------------------------ */

/* A pull-up drives its net weakly high, unless it is open; a shorted pull-up
 * ties the net to the rail. Links are handled by the board (net merging). */
void resistor_eval(sim_t *s, part_t *p) {
    resistor_state_t *st = p->state;
    for (int i = 0; i < st->nel; i++) {
        element_t *e = &st->el[i];
        if (e->type != EL_PULLUP && e->type != EL_PULLDOWN) continue;
        int level = e->type == EL_PULLUP;
        drive_t d = p->rfault == RF_OPEN ? DRV_Z :
                    p->rfault == RF_SHORT ? DRIVE(S_SUPPLY, level) : DRIVE(S_PULL, level);
        sim_drive(s, e->pin_a, d);
    }
}
