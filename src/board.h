/* board.h - a board: the netlist of a machine and its parts, and what
 * every machine has in common (faults, logic probe, oscilloscope, labels,
 * the RESET button). What makes a machine (the cores of its big chips, its
 * bus cycle, its keyboard, what it can load, its video) is described by a
 * machine_t: see c64.c. */
#ifndef BOARD_H
#define BOARD_H
#include "sim.h"
#include "parts.h"

#define SCOPE_SAMPLES   65536           /* scope ring buffer (power of two) */

/* What a pin's net looked like over the last frame (logic probe colors). */
enum { NET_LOW, NET_HIGH, NET_PULSING, NET_FLOATING, NET_CONTENTION, NET_NONE };
/* Scope sample bits. */
#define SC_LEVEL    1
#define SC_FLOATING 2
#define SC_FIGHT    4

/* The role of a chip, to color its name. */
enum { ROLE_RAM, ROLE_VRAM, ROLE_ROM, ROLE_PLA, ROLE_CHIP, ROLE_GLUE, ROLE_CLOCK, ROLE_OTHER };

typedef struct board board_t;

/* A chip of the board: its model, a short name drawn on the chip, a few
 * short lines shown under the name when zooming in ('\n' separated), its
 * role and a one line description. */
typedef struct {
    const char *ref;
    const part_type_t *type;
    const char *label;
    int role;
    const char *info;
    const char *detail;
} part_def_t;

/* A connector: its name and what each pin is for, "PAD=FUNCTION ..."
 * ('_' is shown as a space). */
typedef struct { const char *ref, *name, *pins; } conn_def_t;

/* A kind of file a machine takes (see machine_t.file_kind). */
typedef struct {
    const char *type;           /* "program", "cartridge", "snapshot" */
    const char *action;         /* the button that opens it: "Load and RUN" */
    bool norun;                 /* it can also be loaded without running it */
    int role;                   /* ROLE_* to color it, -1 for plain text */
} file_kind_t;

typedef struct machine {
    const char *id;             /* "c64" (--machine) */
    const char *name;           /* "Commodore 64" */
    const char *board_name;     /* "C64 ASSY 250407" */
    size_t size;                /* of the machine's state, which starts with a board_t */
    const board_data_t *data;
    sim_desc_t sim;
    double cpu_hz;              /* sim.cycle counts these */
    int cycles_per_frame;       /* a video frame */
    int steps_per_cycle;        /* board_sample() calls per cycle (scope time base) */
    int screen_w, screen_h;     /* the picture of board_monitor() */
    uint64_t ready_cycles;      /* from power on to the prompt (BASIC ready) */
    const part_def_t *parts; int nparts;
    const conn_def_t *conns; int nconns;
    const char *link_resistors; /* resistors that are just wires: "RP1 RP2" */
    const char *crystals;       /* other parts that can be broken: "Y1" */
    const char *keys_help;      /* how the host keyboard maps to the machine's */
    const char *reset_help;     /* where the RESET button is */
    /* Set up the cores, models and wiring (board_create() did the rest). */
    void (*create)(board_t *b);
    void (*destroy)(board_t *b);
    /* Switched on: the cores start over (board_power_on() does the rest). */
    void (*power_on)(board_t *b);
    void (*run)(board_t *b, int cycles);
    /* Joins of nets for board_rebuild(): pressed keys, dongles. */
    void (*joins)(board_t *b);
    /* Keyboard: machine key codes, and the host key (a sokol keycode) or
     * the host character that types each. */
    void (*key_down)(board_t *b, int key);
    void (*key_up)(board_t *b, int key);
    void (*frame_update)(board_t *b, uint32_t us);
    int (*host_key)(int keycode);
    int (*host_char)(int c);
    void (*monitor)(board_t *b, uint32_t *out);
    /* Files: the title of the browser, what a file is for this machine
     * (NULL if it isn't one it takes), with a line about it, and whether
     * it can't be used (bad). */
    const char *files_title;
    const file_kind_t *(*file_kind)(const char *path, char *info, int len, bool *bad);
    /* Cartridges: the connector they plug into, the name of the one
     * plugged (NULL if none), pulling it out (the machine restarts). */
    const char *cart_slot;
    const char *(*cart_name)(board_t *b);
    void (*remove_cart)(board_t *b);
    /* Load a file (program, cartridge...): false and a message if not. */
    bool (*load)(board_t *b, const char *path, bool run, char *msg, int msglen);
    const char *(*cpu_status)(board_t *b);              /* "running", "in reset"... */
    void (*cpu_regs)(board_t *b, char *buf, int len);    /* registers, for reports */
    void (*print_text)(board_t *b);                     /* --text: read the screen */
} machine_t;

extern const machine_t machine_c64, machine_zx48;

struct board {
    sim_t sim;
    const machine_t *m;
    /* Video, as the machine's video chip drew it (screen_w x screen_h). */
    uint32_t *screen;
    /* Oscilloscope: one sample per bus step for the selected pin. */
    int scope_pin;
    uint8_t scope[SCOPE_SAMPLES];
    uint32_t scope_pos;
    /* Self-check oracle for tests (C64HW_CHECK, see the machines). */
    bool check;
    uint64_t check_errors;
    /* Logic probe statistics per net, reset by the UI every frame. */
    uint32_t *st_hi, *st_lo, *st_float, *st_fight;
    int st_countdown;
    /* Also sampled when pin_stats is set (the UI does): what each pin
     * itself drives, which can differ from its net (a lifted leg, two
     * outputs fighting, an open collector), and how often each product
     * term of the PLA is true (p_pla < 0: no PLA). Reading them changes
     * nothing simulated. */
    bool pin_stats;
    uint32_t *pst_hi, *pst_lo, *pst_off, *pst_weak;
    int p_pla;
    uint32_t pla_hits[64], pla_samples;
    /* The RESET button: a switch from /RESET to GND on a connector, like a
     * reset cartridge (pins set by the machine, -1 if none). */
    bool reset_button;
    int pin_reset, pin_reset_gnd;
    /* Connectors: what each pin is for ("ATN"), NULL for other parts. */
    const char **pin_func;
    char **conn_strings;
};

board_t *board_create(const machine_t *m);
void board_destroy(board_t *b);
void board_power_on(board_t *b);
/* Rebuild the electrical network after changing faults, links or what
 * a part drives. */
void board_rebuild(board_t *b);
void board_run(board_t *b, int cycles);
/* For the machines: after every bus step, the scope and the statistics. */
void board_sample(board_t *b);

/* Faults. Every change rebuilds the electrical network. */
void board_set_pin_fault(board_t *b, int pin, int fault);
void board_set_removed(board_t *b, int part, bool removed);
void board_set_rfault(board_t *b, int part, int rfault);
void board_clear_faults(board_t *b);
bool board_apply_fault_spec(board_t *b, const char *spec);
/* Press or release the RESET button (also as the spec "RESET:pressed" /
 * "RESET:released"). It is not a fault: Repair all doesn't release it. */
void board_set_reset_button(board_t *b, bool pressed);
bool board_part_simulated(board_t *b, int part);
/* Short name drawn on a chip ("RAM1", "PLA"...), the lines shown under it
 * when zooming in (model, address range), a one line description, and its
 * role (to color the label). */
const char *board_part_label(board_t *b, int part);
const char *board_part_info(board_t *b, int part);
const char *board_part_detail(board_t *b, int part);
int board_part_role(board_t *b, int part);
/* The pin of a part by its pad name ("18", "B"; "D" or "4" for the
 * cassette port's "D,4"), or -1. */
int board_pad_pin(board_t *b, int part, const char *pad);
/* Connectors: name ("SERIAL") and the function of a pin ("ATN"), or NULL. */
const char *board_connector_name(board_t *b, int part);
const char *board_pin_func(board_t *b, int pin);
/* Is a net part of the simulated circuit (not just power or analog)? */
bool board_net_simulated(board_t *b, int net);

/* Keyboard (machine key codes), loading files, video: see machine_t. */
void board_key_down(board_t *b, int key);
void board_key_up(board_t *b, int key);
void board_frame_update(board_t *b, uint32_t us);
bool board_load(board_t *b, const char *path, bool run, char *msg, int msglen);
const char *board_cart_name(board_t *b);
void board_remove_cart(board_t *b);
void board_monitor(board_t *b, uint32_t *out);

/* Logic probe: what a net did since the last board_stats_reset(). */
int board_net_state(board_t *b, int net);
void board_stats_reset(board_t *b);
/* What a pin itself drove (NET_HIGH/LOW/PULSING, or NET_FLOATING if off). */
int board_pin_drive_state(board_t *b, int pin);

#endif
