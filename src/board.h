/* board.h - the C64 ASSY 250407 board: netlist + part models + the four
 * big chip cores (6510, VIC-II, two CIAs, SID), and the bus cycle that
 * moves them. */
#ifndef BOARD_H
#define BOARD_H

#include "sim.h"
#include "parts.h"
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "kbd.h"

#define C64_FREQ        985248          /* PAL CPU clock, Hz */
#define CYCLES_PER_FRAME (63 * 312)     /* PAL: 63 cycles per line, 312 lines */
#define SCREEN_W        392
#define SCREEN_H        272
#define SCOPE_SAMPLES   65536           /* scope ring buffer (power of two) */
#define STEPS_PER_CYCLE 6               /* two halves, three bus steps each */

/* Bus steps of each half cycle (see board.c for the full story). */
enum { STEP_ROW, STEP_RAS, STEP_CAS };

/* What a pin's net looked like over the last frame (logic probe colors). */
enum { NET_LOW, NET_HIGH, NET_PULSING, NET_FLOATING, NET_CONTENTION, NET_NONE };

/* Scope sample bits. */
#define SC_LEVEL    1
#define SC_FLOATING 2
#define SC_FIGHT    4

/* Special C64 keys (same codes as c64-llm). */
#define C64_KEY_SPACE    0x20
#define C64_KEY_CSRLEFT  0x08
#define C64_KEY_CSRRIGHT 0x09
#define C64_KEY_CSRDOWN  0x0A
#define C64_KEY_CSRUP    0x0B
#define C64_KEY_DEL      0x01
#define C64_KEY_INST     0x10
#define C64_KEY_HOME     0x0C
#define C64_KEY_CLR      0x02
#define C64_KEY_RETURN   0x0D
#define C64_KEY_CTRL     0x0E
#define C64_KEY_CBM      0x0F
#define C64_KEY_RESTORE  0xFF
#define C64_KEY_STOP     0x03
#define C64_KEY_F1       0xF1
#define C64_KEY_F3       0xF3
#define C64_KEY_F5       0xF5
#define C64_KEY_F7       0xF7

typedef struct board {
    sim_t sim;

    /* Chip cores and their parts on the board. */
    m6502_t cpu;    uint64_t cpu_pins;  int p_cpu;
    m6569_t vic;    int p_vic;
    m6526_t cia[2]; int p_cia[2];
    m6581_t sid;    int p_sid;
    int p_u31, p_y1, p_cn1;
    int dram_part[8];           /* the DRAM holding each data bit */
    int pin_cn1_pa[8], pin_cn1_pb[8], pin_restore, net_gnd;

    /* State of the current bus half cycle. */
    int phase;                  /* 1 = phi1 (VIC), 2 = phi2 (CPU or VIC) */
    int step;                   /* STEP_* */
    bool vic_owns;              /* the VIC drives the address bus */
    uint16_t vic_addr;          /* 14 bit address of the VIC access */
    int vic_fetches;            /* VIC accesses done in this cycle */
    bool vic_steal;             /* VIC owns phi2 too (AEC low) */
    int ba_streak;              /* cycles BA has been low */
    bool vic_ba_low, vic_irq;   /* VIC outputs */
    uint8_t vic_refresh;        /* DRAM refresh counter */
    uint8_t vic_reg;            /* register address latched when /RAS falls */

    /* Read data a chip core puts on the data bus at the end of phi2:
     * index 0 = VIC, 1 = CIA1, 2 = CIA2, 3 = SID. */
    bool dout_en[4]; uint8_t dout_val[4];
    bool cia_irq[2];
    bool cpu_ticked;            /* the CPU core advanced in this cycle */
    /* Caches for part_drive_bus_cached() (speed only, see parts.h). */
    int32_t cache_cpu_addr, cache_cpu_data, cache_cpu_port, cache_vic_mux, cache_vic_addr,
            cache_vic_data, cache_cia_pa[2], cache_cia_pb[2], cache_cia_data[2], cache_sid_data;

    /* Clock detection: level of each clock pin in phi1 and phi2. */
    uint8_t seen_phi0[2], seen_dot[2], seen_color[2], seen_phi2[3][2];
    bool cpu_running, vic_running, color_ok;
    bool cpu_in_reset;
    uint64_t vic_last_tick;

    /* Keyboard: host keys and the switch matrix they close. */
    kbd_t kbd;
    uint16_t key_matrix[9];

    /* Video, as the VIC drew it. */
    uint32_t screen[SCREEN_W * SCREEN_H];

    /* Oscilloscope: one sample per bus step for the selected pin. */
    int scope_pin;
    uint8_t scope[SCOPE_SAMPLES];
    uint32_t scope_pos;

    /* Self-check oracle for tests (see board.c). */
    bool check;
    uint64_t check_errors;

    /* Logic probe statistics per net, reset by the UI every frame. */
    uint32_t *st_hi, *st_lo, *st_float, *st_fight;
    int st_countdown;
} board_t;

board_t *board_create(void);
void board_power_on(board_t *b);
void board_run(board_t *b, int cycles);

/* Faults. Every change rebuilds the electrical network. */
void board_set_pin_fault(board_t *b, int pin, int fault);
void board_set_removed(board_t *b, int part, bool removed);
void board_set_rfault(board_t *b, int part, int rfault);
void board_clear_faults(board_t *b);
bool board_apply_fault_spec(board_t *b, const char *spec);
bool board_part_simulated(board_t *b, int part);
/* Short name drawn on a chip ("RAM1", "PLA"...), the lines shown under it
 * when zooming in (model, address range), a one line description, and its
 * role (to color the label). */
enum { ROLE_RAM, ROLE_VRAM, ROLE_ROM, ROLE_PLA, ROLE_CHIP, ROLE_GLUE, ROLE_CLOCK, ROLE_OTHER };
const char *board_part_label(board_t *b, int part);
const char *board_part_info(board_t *b, int part);
const char *board_part_detail(board_t *b, int part);
int board_part_role(board_t *b, int part);

/* Keyboard (C64 key codes, as c64-llm). */
void board_key_down(board_t *b, int key);
void board_key_up(board_t *b, int key);
void board_frame_update(board_t *b, uint32_t frame_us);

/* Load a .prg straight into the DRAM chips (a "backdoor", like a freezer
 * cartridge: it works even on a broken machine). Optionally type RUN. */
bool board_load_prg(board_t *b, const char *path, bool run, char *err, int errlen);

/* The picture as the monitor shows it (black & white without color clock,
 * snow without a video signal). */
void board_monitor(board_t *b, uint32_t *out);

/* Logic probe state of a net over the last statistics window. */
int  board_net_state(board_t *b, int net);
void board_stats_reset(board_t *b);

#endif
