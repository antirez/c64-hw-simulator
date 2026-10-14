/* sim.h - the electrical net engine.
 *
 * The board is a set of PARTS (chips, resistors, connectors...). Every part
 * has PINS, and every pin sits on a NET (a piece of copper). At any moment a
 * pin may DRIVE its net: low or high, with a strength. The engine RESOLVES
 * each net from all the drives on it:
 *
 *   SUPPLY  shorts to GND/+5V and the power rails themselves
 *   STRONG  normal chip outputs
 *   PULL    pull-up resistors, weak NMOS outputs (CIA port lines)
 *   Z       nobody drives: the net is FLOATING and keeps its last level
 *           (the copper and the chip inputs behave like a small capacitor)
 *
 * The strongest drive wins. Two drives of the same strength fighting with
 * different levels is a CONTENTION: low wins (an NMOS/TTL output pulls down
 * harder than it pulls up) and the net is flagged so the UI can show it.
 *
 * What a chip READS from a floating net depends on its input technology:
 * bipolar/TTL inputs (74LSxx, the 82S100 PLA) read 1, MOS inputs read the
 * charge left on the net.
 *
 * Parts are evaluated only when one of their input nets changes, in a loop
 * that runs until the whole board is stable (settle). Parts that have memory
 * (latches, RAM) sample their inputs in a separate commit pass, after the
 * board settled, like a real clock edge.
 *
 * Nets can be joined together by "links" (ferrite beads, series resistors),
 * by pressed keyboard keys, and so forth. Joined nets are merged with a
 * union-find: every net has a ROOT, and all the electrical state lives in the
 * root. Links change rarely (faults, key presses) so the merge is just rebuilt
 * from scratch when that happens.
 *
 * Speed notes. The data parts read and write all the time is kept small
 * and dense (pin_t is 14 bytes, the level of every net is mirrored in a byte
 * array). The lines of the main buses also have a bit in a 64 bit "bus
 * bitmap", so a chip can read a whole bus with a shift and a mask instead of
 * pin by pin (see sim_read_group()). None of this changes what is simulated:
 * a bus line with a fault simply falls back to reading pin by pin. */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <stdbool.h>

/* Drive strengths. */
enum { S_Z = 0, S_PULL = 1, S_STRONG = 2, S_SUPPLY = 3 };

/* A drive packs strength and level in one byte: (strength << 1) | level. */
typedef uint8_t drive_t;
#define DRIVE(strength, level) ((drive_t)(((strength) << 1) | ((level) & 1)))
#define DRV_Z       DRIVE(S_Z, 0)
#define DRV_LO      DRIVE(S_STRONG, 0)
#define DRV_HI      DRIVE(S_STRONG, 1)
#define DRV_PULLUP  DRIVE(S_PULL, 1)
#define DRV_BIT(v)  ((v) ? DRV_HI : DRV_LO)

/* Pin faults the user can inject by clicking a pin. */
enum { PF_OK = 0, PF_OPEN, PF_GND, PF_VCC };

/* Pin flags (from the part type description). */
#define PIN_TTL    (1 << 0)   /* bipolar input: floating reads as 1 */
#define PIN_SENSE  (1 << 1)   /* re-evaluate the part when this net changes */
#define PIN_POWER  (1 << 2)   /* VCC/GND pin: if open, the chip is dead */
#define PIN_SWITCH (1 << 3)   /* analog switch pin: re-evaluate when ANY drive on the
                                 net changes, even if the level stays the same */
#define PIN_OUT    (1 << 4)   /* the chip can drive this pin (output or bidirectional) */
#define PIN_IN     (1 << 5)   /* the chip reads this pin (input or bidirectional) */

/* Chip technology: how inputs behave and what happens without power. */
enum { TECH_NMOS = 0, TECH_TTL, TECH_TTL_OC, TECH_CMOS };

/* Power state of a part, from its power pins (see sim_update_part_power). */
enum { PWR_OK = 0, PWR_OFF, PWR_NO_VCC, PWR_NO_GND };

/* Part kinds, mostly used by the UI. */
enum { PART_IC, PART_RESISTOR, PART_LINK, PART_CONNECTOR, PART_OTHER };

/* Resistor / link faults. */
enum { RF_OK = 0, RF_OPEN, RF_SHORT };

typedef struct sim sim_t;
typedef struct part part_t;
typedef void (*part_fn)(sim_t *s, part_t *p);

typedef struct {
    int16_t part;       /* owning part index */
    int16_t number;     /* pin number (1..N), 0 if the pad name isn't a number */
    int16_t net;        /* net index, -1 if unconnected */
    int16_t root;       /* root of the merged net (cache of nets[net].root) */
    drive_t drive;      /* what the part currently drives on this pin */
    drive_t eff;        /* the drive actually counted on the net (faults applied) */
    uint8_t fault;      /* PF_* */
    uint8_t flags;      /* PIN_* */
    uint8_t held;       /* level a MOS input had when its leg was lifted */
    uint8_t locked;     /* the drive can't reach the net: unconnected, faulted,
                           or the chip is dead (computed by sim_rebuild) */
} pin_t;

/* Rails are nets with a constant SUPPLY level. */
enum { RAIL_NONE = 0, RAIL_GND, RAIL_VCC };

typedef struct {
    /* Electrical state (meaningful on roots only). */
    uint16_t count[8];  /* number of drives of each kind (index = drive_t value),
                           so resolving a change costs the same on any net */
    uint8_t level;      /* 0/1: driven level, or the charge when floating */
    uint8_t strength;   /* S_* of the winning drive, S_Z if floating */
    uint8_t contention; /* two equal drives fighting */
    uint8_t rail;       /* RAIL_* */
    int nreaders, nswitches;
    int *readers;       /* parts to re-evaluate when the level changes */
    int *switches;      /* parts to re-evaluate on any drive change */
    uint64_t busbits;   /* bus bitmap bits of the nets merged into this root */
    /* Floating nets (see "Floating inputs" in sim.c). While floating,
     * 'level' is the level the net had when it was last driven. */
    uint64_t float_since;   /* cycle the net stopped being driven */
    uint64_t hold_until;    /* until then MOS inputs still clearly read 'level' */
    uint8_t ttl_loaded;     /* a TTL input sits on the net (pulls it to ~1.4V) */
    /* Rarely used. */
    int root;           /* union-find parent: equals own index for a root */
    int busbit;         /* this net's bit in the bus bitmap, or -1 */
    int *pins, npins;   /* every pin on this (merged) net */
    const char *name;
} net_t;

/* A group of pins read together (an address or data bus), see
 * sim_read_group(). Cached per part, recomputed after every rebuild. */
typedef struct {
    const int *key;     /* the pin number array identifying the group */
    int n;              /* and its length */
    uint32_t epoch;     /* rebuild epoch the cache was computed in */
    int first;          /* bus bitmap bit of the group's first pin, or -1 */
    uint64_t mask, ttl; /* group bits / TTL inputs, shifted to bit 0 */
} group_t;

#define PART_GROUPS 4

struct part {
    const char *ref;    /* "U17" */
    const char *name;   /* "82S100 PLA", shown on the chip */
    int kind;           /* PART_* */
    int first_pin, npins;
    int pinmap[48];     /* pin number -> pin index (ICs and connectors) */
    bool removed;       /* pulled out of its socket */
    uint8_t tech;       /* TECH_* */
    uint8_t power;      /* PWR_*, computed from 'removed' and power pin faults */
    bool dead;          /* computed: the chip doesn't work (no eval, no core) */
    int rfault;         /* RF_* for resistors and links */
    part_fn eval;       /* compute output drives from inputs (may be NULL) */
    part_fn commit;     /* latch / store on clock edges (may be NULL) */
    part_fn reset;      /* power-on state (may be NULL) */
    void *state;        /* part specific state */
    void *user;         /* back pointer to the board */
    group_t groups[PART_GROUPS];
};

#define SIM_MAX_PARTS 256
#define SIM_QUEUE_SIZE 512      /* power of two, > SIM_MAX_PARTS */

struct sim {
    pin_t *pins; int npins;
    const char **pin_names;         /* datasheet pin names (NULL if unknown) */
    net_t *nets; int nnets;
    uint8_t *nread;                 /* per net: bit 0 level, bit 1 floating,
                                       bit 2 erratic (floating near threshold) */
    uint64_t bus_level, bus_float;  /* the bus bitmap */
    uint64_t bus_erratic;           /* bus lines with the erratic bit */
    uint32_t epoch;                 /* bumped by every rebuild */
    part_t parts[SIM_MAX_PARTS]; int nparts;
    int *commit_list; int ncommit;  /* parts that have a commit function */
    /* Dirty parts waiting for eval, in a FIFO ring. A part is in the queue
     * at most once, so the ring never holds more than SIM_MAX_PARTS. */
    uint8_t dirty[SIM_MAX_PARTS];
    int queue[SIM_QUEUE_SIZE];
    unsigned qhead, qtail;
    uint64_t cycle;                 /* board clock, in CPU cycles (set by the board) */
    int clock_level;                /* PHI0 level of the current half cycle */
    uint64_t rng;                   /* noise generator state (deterministic) */
    uint64_t hold_time[2][2];       /* [ttl_loaded][level], see set_hold() */
    uint64_t *open_since;           /* per pin: cycle its leg was lifted */
    int *noisy, nnoisy;             /* parts with a lifted MOS input leg */
};

/* Set up pins/nets/parts from the generated board tables. */
void sim_init(sim_t *s);
/* Rebuild merged nets (call after changing links, keys, faults). */
void sim_rebuild(sim_t *s);
/* Add a "join" between two nets for the next rebuild (links, keys). */
void sim_join_begin(sim_t *s);
void sim_join(sim_t *s, int net_a, int net_b);
/* Power on: every net loses its charge (rails keep their level). */
void sim_discharge(sim_t *s);
/* Restart the noise generator (call at power on, for reproducible runs). */
void sim_seed(sim_t *s, uint64_t seed);
/* Mark the floating nets whose charge drifted near the input threshold
 * as erratic, and wake the parts that read erratic inputs (those nets,
 * lifted MOS legs): their value can change without any event. The board
 * calls this every few bus steps. */
void sim_wake_erratic(sim_t *s);
/* Give a net a bit in the bus bitmap (0..63), so chips can read the bus it
 * belongs to as a word. Lines of a bus must get consecutive bits. */
void sim_set_bus_bit(sim_t *s, int net, int bit);

/* Used by parts: drive a pin, read the level a pin sees. These two are
 * called all the time, so their fast paths are inline. */
void sim_drive_changed(sim_t *s, pin_t *p);
static inline void sim_drive(sim_t *s, int pin, drive_t d) {
    if (pin < 0) return;
    pin_t *p = &s->pins[pin];
    if (p->drive == d) return;
    p->drive = d;
    if (!p->locked) sim_drive_changed(s, p);
}

/* Slow paths of sim_read(), in sim.c: a MOS input on a floating net that
 * has been floating for a while, and a MOS input whose leg is lifted. */
int sim_read_floating(sim_t *s, int pin);
int sim_read_open(sim_t *s, int pin);

static inline int sim_read(sim_t *s, int pin) {
    if (pin < 0) return 1;
    const pin_t *p = &s->pins[pin];
    int ttl = p->flags & PIN_TTL;
    /* Lifted leg: a TTL input reads high, a MOS input floats on its own. */
    if (p->fault == PF_OPEN || p->root < 0) return ttl ? 1 : sim_read_open(s, pin);
    /* Driven net: its level. Floating net: a TTL input reads 1, a MOS input
     * reads the charge left on the net, as long as it is clearly there. */
    uint8_t v = s->nread[p->root];
    if (__builtin_expect((v & 4) && !ttl, 0)) return sim_read_floating(s, pin);
    return (v & 1) | ((v >> 1) & ttl);
}

/* Read a group of pins of a part (pin numbers in 'numbers', bit 0 first)
 * as a binary number. Same result as reading them one by one. */
unsigned sim_read_group(sim_t *s, part_t *p, const int *numbers, int n);

/* Change a pin fault (the caller must sim_rebuild() afterwards). An opened
 * MOS input remembers the level it saw at that moment. */
void sim_set_pin_fault(sim_t *s, int pin, int fault);
/* Strongest drive on a pin's net ignoring the drive of one pin (4066). */
drive_t sim_drive_except(sim_t *s, int pin_on_net, int except_pin);
static inline int part_pin(const part_t *p, int number) { return p->pinmap[number]; }
static inline void pin_out(sim_t *s, part_t *p, int number, drive_t d) { sim_drive(s, p->pinmap[number], d); }
static inline int pin_in(sim_t *s, part_t *p, int number) { return sim_read(s, p->pinmap[number]); }

/* Queue a part for evaluation (no-op if already queued). Only parts with
 * an eval function may be marked (reader lists only contain those). */
static inline void sim_mark_dirty(sim_t *s, int part) {
    if (s->dirty[part]) return;
    s->dirty[part] = 1;
    s->queue[s->qtail++ & (SIM_QUEUE_SIZE - 1)] = part;
}
/* The drive a pin actually puts on its net, faults considered. */
drive_t pin_drive(const sim_t *s, const pin_t *p);
/* Evaluate dirty parts until nothing changes anymore. */
void sim_settle(sim_t *s);
/* Let every part with memory sample its inputs (a clock edge). */
void sim_commit(sim_t *s);
/* Recompute the power state of a part from 'removed' and its power pins
 * (call before sim_rebuild). */
void sim_update_part_power(sim_t *s, int part);
int  sim_find_part(sim_t *s, const char *ref);
int  sim_find_net(sim_t *s, const char *name);
static inline const char *sim_pin_name(const sim_t *s, int pin) { return s->pin_names[pin]; }

#endif
