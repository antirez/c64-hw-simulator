/* zx48.h - the Sinclair ZX Spectrum 48K, board Issue 3B: netlist + part
 * models + the Z80 core and the ULA model, and the clock that moves them. */
#ifndef ZX48_H
#define ZX48_H
#include "board.h"
#include "chips_common.h"
#include "z80.h"
#include "kbd.h"
#include "ula.h"

#define ZX_FREQ             3500000         /* CPU clock, Hz */
#define ZX_CYCLES_PER_FRAME (224 * 312)     /* T states per frame */
#define ZX_STEPS_PER_CYCLE  8               /* half periods of the 14 MHz oscillator */

typedef struct zx48 {
    board_t board;              /* first: a zx48_t is a board_t */
    z80_t cpu;
    uint64_t cpu_pins;
    int p_cpu, p_ula, p_rom;
    int pin_clk;                /* the Z80's CLK input */
    int clk;                    /* its level at the last step */
    bool in_reset, cpu_running;
    uint64_t last_edge;         /* cycle of the last clock edge */
    int32_t cache_addr, cache_data;
    uint64_t changes_seen;      /* sim.changes at the last commit */
    int lram_part[8], uram_part[8]; /* the DRAM of each data bit */
    /* Keyboard: the half rows (KB2 pads, one per address line A8-A15
     * through D1-D8) and the lines into the ULA (KB1 pads, KB0-KB4). */
    kbd_t kbd;
    uint16_t key_matrix[5];
    int pin_kb_row[8], pin_kb_line[5];
    /* Power: the 9 V input, and the DC-DC converter's +12V and -5V. */
    int pin_9v, pin_12v, pin_m5v;
    /* A ROM cartridge in the edge connector (zx48.c), NULL if none. */
    void *cart;
    /* Power on reset: C27 holds /RESET low for a while. */
    int pin_reset_cap;
    uint64_t reset_until;
} zx48_t;

static inline zx48_t *ZX(board_t *b) { return (zx48_t *)b; }

extern const machine_t machine_zx48;

#endif
