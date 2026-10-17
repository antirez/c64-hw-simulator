/* parts.h - part types: pin descriptions and simple pin-level models for
 * the glue logic and memories of the C64 board. The big chips (6510, VIC-II,
 * CIAs, SID) are wrapped in board.c around the existing emulator cores. */
#ifndef PARTS_H
#define PARTS_H

#include "sim.h"
#include <stddef.h>

typedef struct {
    const char *name;       /* label drawn on the chip, e.g. "74LS08" */
    /* Pins in order 1..N as "NAME:K", K = kind:
     *   T  TTL input, re-evaluate on change (floating reads 1)
     *   M  MOS input, re-evaluate on change (floating reads the charge)
     *   m  MOS input sampled only at clock edges / by the chip core
     *   O  output (or analog pin not simulated)
     *   B  bidirectional MOS pin (data bus), sampled by the core
     *   P  power pin (if open, the chip is dead)
 *   S  analog switch terminal (4066): passes any drive through */
    const char *pins;
    part_fn eval, commit, reset;
    size_t state_size;
    int tech;               /* TECH_* (default NMOS) */
} part_type_t;

extern const part_type_t PT_6510, PT_6569, PT_6526, PT_6581;
extern const part_type_t PT_82S100, PT_2364, PT_2332, PT_2114, PT_4164;
extern const part_type_t PT_74LS08, PT_7406, PT_74LS139, PT_74LS257, PT_74LS258;
extern const part_type_t PT_74LS373, PT_4066, PT_556, PT_74LS629;
extern const part_type_t PT_74LS74, PT_74LS193, PT_MC4044;

/* 82S100 PLA: its logic as a table of product terms (see parts.c). The
 * input word has bit PI_x set when that input pin is high; the outputs are
 * pin levels, bit PO_x. Pin names as in the schematic (/LORAM, /CAS...). */
enum { PI_A13, PI_A14, PI_A15, PI_VA14, PI_CHAREN, PI_HIRAM, PI_LORAM, PI_CAS,
       PI_VA12, PI_VA13, PI_GAME, PI_EXROM, PI_RW, PI_AEC, PI_BA, PI_A12, PLA_NIN };
enum { PO_ROMH, PO_ROML, PO_IO, PO_GRW, PO_CHAROM, PO_KERNAL, PO_BASIC, PO_CASRAM, PLA_NOUT };
typedef struct {
    uint16_t mask, value;   /* true when (inputs & mask) == value */
    uint8_t outputs;        /* bit PO_x set: the term feeds that output */
} pla_term_t;
extern const pla_term_t pla_terms[];
extern const int pla_nterms;
extern const int pla_in_pin[PLA_NIN], pla_out_pin[PLA_NOUT];
unsigned pla_read_inputs(sim_t *s, part_t *p);
unsigned pla_logic(unsigned inputs);

/* Attach a type to a part: pin names/flags, functions, state. */
void part_set_type(sim_t *s, int part, const part_type_t *t);
/* Read/drive a group of pins as a binary number (pins[0] is bit 0). */
unsigned part_read_bus(sim_t *s, part_t *p, const int *pins, int n);
void part_drive_bus(sim_t *s, part_t *p, const int *pins, int n, unsigned v, bool enable);
/* Same, but does nothing if value and enable are the same as the last time
 * this cache was used. A pure speed-up: sim_drive() would ignore unchanged
 * drives anyway, one pin at a time. A zeroed cache means "unknown". */
static inline void part_drive_bus_cached(sim_t *s, part_t *p, const int *pins, int n,
                                         unsigned v, bool enable, int32_t *cache) {
    int32_t key = enable ? (int32_t)v + 1 : -1;
    if (*cache == key) return;
    *cache = key;
    part_drive_bus(s, p, pins, n, v, enable);
}

/* ROM state: the image the ROM contains. */
typedef struct { const uint8_t *data; int32_t out_cache; } rom_state_t;
/* DRAM state: one byte per bit cell (simple, not compact). */
typedef struct {
    uint8_t cells[65536];
    uint8_t row, col;
    uint8_t ras_prev, cas_prev;
    uint8_t out_en, out_bit;
} dram_state_t;
/* Color RAM. */
typedef struct { uint8_t cells[1024]; int32_t out_cache; } sram_state_t;
/* 556: power-on reset and RESTORE debouncer. */
typedef struct {
    uint64_t reset_until;   /* OUT2 high (CPU in reset) until this cycle */
    uint64_t nmi_until;     /* OUT1 high (NMI) until this cycle */
    int restore_net;        /* net of the RESTORE key (seen through C38) */
    uint8_t restore_prev;
    uint64_t last_cycle;    /* time only moves once per cycle */
} timer556_state_t;
/* 74LS629: dot clock and color clock oscillator. */
typedef struct { bool crystal_ok; } vco_state_t;

/* Resistors and links: what each element does electrically. */
enum { EL_PULLUP, EL_PULLDOWN, EL_LINK, EL_ANALOG };
typedef struct { int pin_a, pin_b, type; } element_t;
typedef struct { element_t el[16]; int nel; } resistor_state_t;
void resistor_eval(sim_t *s, part_t *p);

#endif
