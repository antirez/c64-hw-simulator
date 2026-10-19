/* c64.h - the Commodore 64 board, ASSY 250407: netlist + part models +
 * the four big chip cores (6510, VIC-II, two CIAs, SID), and the bus
 * cycle that moves them. Shared by c64.c, cart.c and harness.c. */
#ifndef C64_H
#define C64_H
#include "board.h"
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "kbd.h"

#define C64_FREQ        985248          /* PAL CPU clock, Hz */
#define C64_CYCLES_PER_FRAME (63 * 312) /* PAL: 63 cycles per line, 312 lines */
#define C64_SCREEN_W    392
#define C64_SCREEN_H    272
#define C64_STEPS_PER_CYCLE 6           /* two halves, three bus steps each */

/* Bus steps of each half cycle (see c64.c for the full story). */
enum { STEP_ROW, STEP_RAS, STEP_CAS };

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

typedef struct c64 {
    board_t board;              /* first: a c64_t is a board_t */
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
    uint8_t cia_outs[2];        /* /PC, SP, CNT as last driven */
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
    /* The mains, as the board sees it through R5 and R100, and the
     * cassette motor driver (see c64.c). */
    int pin_mains, p_r5, p_r100, pin_motor, pin_motor_ctl;
    uint64_t mains_next, mains_edges;     /* next flip of the mains, flips so far */
    /* The 586220 diagnostic harness (harness.c): plugged, and the level of
     * WRITE that sets its switches. */
    bool harness, harness_write;
    int pin_hmotor, pin_hwrite, p_u28;
    /* The cartridge in the expansion port (cart.c), NULL if none. */
    void *cart;
    int p_cn6;
} c64_t;

static inline c64_t *C64(board_t *b) { return (c64_t *)b; }
static inline bool board_is_c64(const board_t *b) { return b->m == &machine_c64; }

/* Load a .prg straight into the DRAM chips (a "backdoor", like a freezer
 * cartridge: it works even on a broken machine). Optionally type RUN. */
bool c64_load_prg(board_t *b, const char *path, bool run, char *err, int errlen);
/* Plug a .CRT cartridge into the expansion port, or pull it out. Both
 * switch the C64 off and on again, as you would. 'msg' tells what was
 * loaded or why not. The name is NULL without a cartridge (or a C64). */
bool board_insert_cart(board_t *b, const char *path, char *msg, int msglen);
void board_remove_cart(board_t *b);
const char *board_cart_name(board_t *b);
/* Plug or unplug the 586220 diagnostic harness (harness.c); the C64 is
 * switched off and on. board_harness() is false on other machines. */
void board_set_harness(board_t *b, bool plugged);
bool board_harness(board_t *b);
/* For c64.c: the harness connections, its per-cycle work, and the value
 * the SID reads on a POT pin. */
void harness_joins(board_t *b);
void harness_tick(board_t *b);
int board_pot_value(board_t *b, int sid_pin);

#endif
