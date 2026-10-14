/* sim.c - the electrical net engine. See sim.h for the concepts. */
#include "sim.h"
#include "board_data.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static void compute_hold_times(sim_t *s);

/* Pending joins for the next sim_rebuild(). */
static int *join_a, *join_b, njoin, join_cap;

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

static int rail_of(const char *name) {
    if (!strcmp(name, "Earth")) return RAIL_GND;
    if (!strcmp(name, "+5V") || !strcmp(name, "+Vc") || !strcmp(name, "CAN+5V") ||
        !strcmp(name, "+12V") || !strcmp(name, "5V_joyport") || !strcmp(name, "5V_FB3"))
        return RAIL_VCC;
    return RAIL_NONE;
}

static int part_kind_of(const char *ref) {
    if (ref[0] == 'U') return PART_IC;
    if (ref[0] == 'R') return PART_RESISTOR;   /* R and RP */
    if (!strncmp(ref, "FB", 2) || ref[0] == 'L' || ref[0] == 'F' ||
        (ref[0] == 'E' && ref[1] >= '0' && ref[1] <= '9')) return PART_LINK;
    if (!strncmp(ref, "CN", 2)) return PART_CONNECTOR;
    return PART_OTHER;
}

void sim_init(sim_t *s) {
    memset(s, 0, sizeof(*s));
    s->npins = bd_npads;
    s->pins = calloc(s->npins, sizeof(pin_t));
    s->pin_names = calloc(s->npins, sizeof(char *));
    s->nnets = bd_nnets;
    s->nets = calloc(s->nnets, sizeof(net_t));
    s->nread = calloc(s->nnets, 1);
    s->open_since = calloc(s->npins, sizeof(uint64_t));
    s->noisy = calloc(SIM_MAX_PARTS, sizeof(int));
    sim_seed(s, 1);
    compute_hold_times(s);
    for (int i = 0; i < s->nnets; i++) {
        s->nets[i].name = bd_nets[i];
        s->nets[i].root = i;
        s->nets[i].busbit = -1;
        s->nets[i].rail = rail_of(bd_nets[i]);
        s->nets[i].level = s->nets[i].rail == RAIL_VCC;
    }
    /* One part per footprint, one pin per pad. */
    for (int c = 0; c < bd_ncomps && s->nparts < SIM_MAX_PARTS; c++) {
        const bd_comp_t *bc = &bd_comps[c];
        part_t *p = &s->parts[s->nparts];
        p->ref = bc->ref;
        p->name = bc->value;
        p->kind = part_kind_of(bc->ref);
        p->first_pin = bc->first_pad;
        p->npins = bc->npads;
        for (int i = 0; i < 48; i++) p->pinmap[i] = -1;
        for (int i = 0; i < bc->npads; i++) {
            int pi = bc->first_pad + i;
            pin_t *pin = &s->pins[pi];
            pin->part = (int16_t)s->nparts;
            pin->net = (int16_t)bd_pads[pi].net;
            pin->root = pin->net;
            pin->number = (int16_t)atoi(bd_pads[pi].name);
            if (pin->number > 0 && pin->number < 48) p->pinmap[pin->number] = pi;
        }
        s->nparts++;
    }
    sim_rebuild(s);
}

void sim_set_bus_bit(sim_t *s, int net, int bit) {
    if (net >= 0 && bit >= 0 && bit < 64) s->nets[net].busbit = bit;
}

/* Recompute the dense copies of the net state (nread[] and the bus bitmap)
 * from the roots. */
static void mirror_all(sim_t *s) {
    s->bus_level = s->bus_float = s->bus_erratic = 0;
    for (int i = 0; i < s->nnets; i++) {
        const net_t *n = &s->nets[i];
        if (n->root != i) continue;
        int floating = n->strength == S_Z;
        s->nread[i] = (uint8_t)(n->level | (floating << 1));
        if (n->level) s->bus_level |= n->busbits;
        if (floating) s->bus_float |= n->busbits;
    }
}

void sim_discharge(sim_t *s) {
    for (int i = 0; i < s->nnets; i++) {
        s->nets[i].level = s->nets[i].rail == RAIL_VCC;
        s->nets[i].float_since = UINT64_MAX;    /* restarted by the next rebuild */
    }
    /* Lifted MOS legs lose their charge too. */
    for (int i = 0; i < s->npins; i++) {
        s->pins[i].held = 0;
        s->open_since[i] = 0;
    }
    mirror_all(s);
}

int sim_find_part(sim_t *s, const char *ref) {
    for (int i = 0; i < s->nparts; i++) if (!strcmp(s->parts[i].ref, ref)) return i;
    return -1;
}

int sim_find_net(sim_t *s, const char *name) {
    for (int i = 0; i < s->nnets; i++) if (!strcmp(s->nets[i].name, name)) return i;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Merging nets (union-find)                                                 */
/* ------------------------------------------------------------------------ */

void sim_join_begin(sim_t *s) { (void)s; njoin = 0; }

void sim_join(sim_t *s, int a, int b) {
    (void)s;
    if (a < 0 || b < 0) return;
    if (njoin == join_cap) {
        join_cap = join_cap ? join_cap * 2 : 256;
        join_a = realloc(join_a, join_cap * sizeof(int));
        join_b = realloc(join_b, join_cap * sizeof(int));
    }
    join_a[njoin] = a; join_b[njoin] = b; njoin++;
}

static int find_root(sim_t *s, int n) {
    while (s->nets[n].root != n) {
        s->nets[n].root = s->nets[s->nets[n].root].root;   /* path halving */
        n = s->nets[n].root;
    }
    return n;
}

static void resolve(sim_t *s, int root, int cause_part);

/* ------------------------------------------------------------------------ */
/* Floating inputs                                                           */
/* ------------------------------------------------------------------------ */

/* A net nobody drives keeps its charge for a while, then drifts:
 *  - with a TTL input on it, the input pushes current out and brings the
 *    net to about 1.4V within microseconds;
 *  - with only MOS inputs, leakage brings it to 0V in a fraction of a second
 *    (the 6510 port bits 6/7 fade in ~350ms, VICE measured).
 * A lifted MOS input leg is a tiny node of its own: it drifts to about
 * 1.4V in a few milliseconds, and it picks up the levels of its two
 * neighbour pins in the package (crosstalk).
 * A MOS input compares this voltage, plus noise, with its own threshold
 * (about 1.4V, a bit different for every pin). Far from the threshold the
 * result is stable; near it, it is random, as on a real board. TTL inputs
 * always read a floating net or a lifted leg as 1. All the numbers below
 * are educated guesses: the shape of the behavior matters more. */
#define V_HIGH        5.0
#define V_TH          1.4       /* MOS input threshold */
#define V_TH_SPREAD   0.25      /* +/- per pin */
#define V_NOISE       0.08      /* sigma of the noise on a floating input */
#define V_BAND        (V_TH_SPREAD + 4 * V_NOISE)
#define V_TTL_REST    1.4       /* floating net with TTL inputs */
#define TAU_TTL       2.0       /* cycles */
#define TAU_MOS       300000.0  /* cycles: leakage, ~0.3 s */
#define V_OPEN_REST   1.4       /* lifted MOS leg */
#define TAU_OPEN      5000.0    /* cycles: ~5 ms */
#define K_CROSSTALK   0.12      /* coupling from each neighbour pin */

void sim_seed(sim_t *s, uint64_t seed) { s->rng = seed * 0x9E3779B97F4A7C15ULL + 1; }

static double noise(sim_t *s) {
    double sum = 0;      /* sum of 4 uniforms: close enough to a gaussian */
    for (int i = 0; i < 4; i++) {
        s->rng ^= s->rng << 13; s->rng ^= s->rng >> 7; s->rng ^= s->rng << 17;
        sum += (double)(s->rng >> 11) / (double)(1ULL << 53);
    }
    return (sum - 2.0) * 1.732 * V_NOISE;
}

/* The threshold of a pin: fixed, but different from pin to pin. */
static double threshold(int pin) {
    uint32_t h = (uint32_t)pin * 2654435761u;
    h ^= h >> 15;
    return V_TH + ((double)(h & 1023) / 1023.0 * 2 - 1) * V_TH_SPREAD;
}

/* Voltage of a node that started at v0 and drifts to 'rest'. */
static double drift(double v0, double rest, double tau, uint64_t age) {
    return rest + (v0 - rest) * exp(-(double)age / tau);
}

/* How long a floating net still reads clearly as its last level, before
 * the drift brings it near any input threshold: reads before that time
 * don't need any computation. It only depends on the last level and on
 * the kind of net, so the four cases are computed once, by sim_init(). */
static void compute_hold_times(sim_t *s) {
    for (int t = 0; t < 2; t++)
        for (int l = 0; l < 2; l++) {
            double v0 = l ? V_HIGH : 0, rest = t ? V_TTL_REST : 0, tau = t ? TAU_TTL : TAU_MOS;
            double lo = V_TH - V_BAND, hi = V_TH + V_BAND, d;
            if (v0 >= lo && v0 <= hi) d = 0;
            else if ((v0 < lo && rest < lo) || (v0 > hi && rest > hi)) d = INFINITY;
            else d = tau * log((v0 - rest) / ((v0 < lo ? lo : hi) - rest));
            s->hold_time[t][l] = isinf(d) ? UINT64_MAX : (uint64_t)d;
        }
}

static inline void set_hold(sim_t *s, net_t *n) {
    uint64_t d = s->hold_time[n->ttl_loaded][n->level];
    n->hold_until = d == UINT64_MAX ? UINT64_MAX : n->float_since + d;
}

int sim_read_floating(sim_t *s, int pin) {
    const net_t *n = &s->nets[s->pins[pin].root];
    double v = drift(n->level ? V_HIGH : 0, n->ttl_loaded ? V_TTL_REST : 0,
                     n->ttl_loaded ? TAU_TTL : TAU_MOS, s->cycle - n->float_since);
    return v + noise(s) > threshold(pin);
}

int sim_read_open(sim_t *s, int pin) {
    const pin_t *p = &s->pins[pin];
    const part_t *part = &s->parts[p->part];
    double v = drift(p->held ? V_HIGH : 0, V_OPEN_REST, TAU_OPEN, s->cycle - s->open_since[pin]);
    /* Crosstalk from the neighbour pins on the same side of the package. */
    int half = part->npins / 2, num = p->number;
    for (int d = -1; d <= 1; d += 2) {
        int nb = num + d;
        if (num <= 0 || nb < 1 || nb > part->npins || (num <= half) != (nb <= half)) continue;
        int pi = part->pinmap[nb];
        if (pi < 0 || (s->pins[pi].flags & PIN_POWER) || s->pins[pi].root < 0) continue;
        v += K_CROSSTALK * ((s->nread[s->pins[pi].root] & 1) ? V_HIGH / 2 : -V_HIGH / 2);
    }
    return v + noise(s) > threshold(pin);
}

void sim_wake_erratic(sim_t *s) {
    for (int i = 0; i < s->nnets; i++) {
        net_t *n = &s->nets[i];
        if (n->root != i || n->strength != S_Z || s->cycle < n->hold_until) continue;
        s->nread[i] |= 4;           /* see sim_read() */
        s->bus_erratic |= n->busbits;
        for (int k = 0; k < n->nreaders; k++) sim_mark_dirty(s, n->readers[k]);
    }
    for (int i = 0; i < s->nnoisy; i++)
        if (s->parts[s->noisy[i]].eval) sim_mark_dirty(s, s->noisy[i]);
}

/* Rebuild the merged nets: roots, pin lists and reader lists. This is the
 * only "slow" operation of the engine, called when the user changes a
 * fault or a key is pressed. */
void sim_rebuild(sim_t *s) {
    for (int i = 0; i < s->nnets; i++) {
        net_t *n = &s->nets[i];
        n->root = i;
        free(n->pins); n->pins = NULL; n->npins = 0;
        free(n->readers); n->readers = NULL; n->nreaders = 0;
        free(n->switches); n->switches = NULL; n->nswitches = 0;
    }
    for (int j = 0; j < njoin; j++) {
        int ra = find_root(s, join_a[j]), rb = find_root(s, join_b[j]);
        if (ra == rb) continue;
        /* Keep a rail as root, so the merged net stays a rail. */
        if (s->nets[rb].rail && !s->nets[ra].rail) { int t = ra; ra = rb; rb = t; }
        s->nets[rb].root = ra;
    }
    for (int i = 0; i < s->nnets; i++) s->nets[i].root = find_root(s, i);
    for (int i = 0; i < s->nnets; i++) s->nets[i].busbits = 0;
    for (int i = 0; i < s->nnets; i++)
        if (s->nets[i].busbit >= 0) s->nets[s->nets[i].root].busbits |= 1ULL << s->nets[i].busbit;

    /* Pin lists. */
    for (int i = 0; i < s->npins; i++) {
        pin_t *p = &s->pins[i];
        p->root = (int16_t)(p->net >= 0 ? s->nets[p->net].root : -1);
        if (p->root < 0) continue;
        net_t *n = &s->nets[p->root];
        n->pins = realloc(n->pins, (n->npins + 1) * sizeof(int));
        n->pins[n->npins++] = i;
        /* Reader: a part that wants to be re-evaluated when this net changes.
         * An open pin doesn't see the net, so it isn't a reader. */
        if ((p->flags & PIN_SWITCH) && p->fault != PF_OPEN) {
            n->switches = realloc(n->switches, (n->nswitches + 1) * sizeof(int));
            n->switches[n->nswitches++] = p->part;
        }
        if ((p->flags & PIN_SENSE) && p->fault != PF_OPEN && s->parts[p->part].eval) {
            bool dup = false;
            for (int k = 0; k < n->nreaders; k++) if (n->readers[k] == p->part) dup = true;
            if (!dup) {
                n->readers = realloc(n->readers, (n->nreaders + 1) * sizeof(int));
                n->readers[n->nreaders++] = p->part;
            }
        }
    }
    /* Commit list (parts with memory). */
    free(s->commit_list);
    s->commit_list = malloc(s->nparts * sizeof(int));
    s->ncommit = 0;
    for (int i = 0; i < s->nparts; i++) if (s->parts[i].commit) s->commit_list[s->ncommit++] = i;

    /* Count the drives on every net, then resolve everything and
     * re-evaluate every part. */
    for (int i = 0; i < s->nnets; i++) memset(s->nets[i].count, 0, sizeof(s->nets[i].count));
    for (int i = 0; i < s->nnets; i++)
        if (s->nets[i].rail) s->nets[s->nets[i].root].count[DRIVE(S_SUPPLY, s->nets[i].rail == RAIL_VCC)]++;
    for (int i = 0; i < s->npins; i++) {
        pin_t *p = &s->pins[i];
        const part_t *part = &s->parts[p->part];
        /* Locked: what reaches the net doesn't depend on what the part drives
         * (a fault, or a chip without power, see pin_drive()). */
        p->locked = p->root < 0 || p->fault != PF_OK || part->power == PWR_OFF ||
                    (part->power != PWR_OK && part->tech != TECH_CMOS);
        p->eff = p->root >= 0 ? pin_drive(s, p) : DRV_Z;
        if (p->eff >> 1) s->nets[p->root].count[p->eff]++;
    }
    /* Nets with a TTL input on them (a present, connected one). */
    for (int i = 0; i < s->nnets; i++) s->nets[i].ttl_loaded = 0;
    for (int i = 0; i < s->npins; i++) {
        const pin_t *p = &s->pins[i];
        if (p->root >= 0 && (p->flags & PIN_TTL) && p->fault != PF_OPEN && s->parts[p->part].power != PWR_OFF)
            s->nets[p->root].ttl_loaded = 1;
    }
    for (int i = 0; i < s->nnets; i++) if (s->nets[i].root == i) resolve(s, i, -1);
    /* Floating nets: (re)compute how long their charge clearly holds. */
    for (int i = 0; i < s->nnets; i++) {
        net_t *n = &s->nets[i];
        if (n->root != i || n->strength != S_Z) continue;
        if (n->float_since > s->cycle) n->float_since = s->cycle;
        set_hold(s, n);
    }
    /* Parts with a lifted MOS input leg: their input is noisy. */
    s->nnoisy = 0;
    for (int i = 0; i < s->nparts; i++) {
        const part_t *part = &s->parts[i];
        if (part->dead || (!part->eval && !part->commit)) continue;
        for (int k = 0; k < part->npins; k++) {
            const pin_t *p = &s->pins[part->first_pin + k];
            if (p->fault == PF_OPEN && (p->flags & PIN_IN) && !(p->flags & PIN_TTL)) {
                s->noisy[s->nnoisy++] = i;
                break;
            }
        }
    }
    mirror_all(s);
    s->epoch++;     /* pin groups must be looked at again */
    for (int i = 0; i < s->nparts; i++) if (s->parts[i].eval) sim_mark_dirty(s, i);
}

/* ------------------------------------------------------------------------ */
/* Resolution                                                                */
/* ------------------------------------------------------------------------ */

/* The effective drive of a pin, taking its fault and the power state of
 * its chip into account. A chip with an open power pin:
 *  - NMOS or TTL without VCC: off, all its pins float. NMOS protection
 *    devices only go to the substrate (ground) and TTL inputs don't conduct
 *    into an unpowered chip, so it neither works nor loads the board.
 *  - NMOS or TTL without GND: the inside of the chip floats up to VCC, so the
 *    outputs go (weakly) high. Open collector outputs can't go high: off.
 *  - CMOS (the 4066) has protection diodes to both rails and needs almost no
 *    current: it runs on what its pins give it through those diodes
 *    ("phantom power", see sim_settle), so its drives are normal. */
drive_t pin_drive(const sim_t *s, const pin_t *p) {
    switch (p->fault) {
    case PF_GND:  return DRIVE(S_SUPPLY, 0);
    case PF_VCC:  return DRIVE(S_SUPPLY, 1);
    case PF_OPEN: return DRV_Z;     /* the leg doesn't touch the copper */
    default: break;
    }
    const part_t *part = &s->parts[p->part];
    if (part->power == PWR_OK || part->tech == TECH_CMOS) return part->power == PWR_OFF ? DRV_Z : p->drive;
    if (part->power == PWR_OFF || (p->flags & PIN_POWER)) return DRV_Z;
    if (part->power == PWR_NO_VCC) return DRV_Z;
    /* PWR_NO_GND */
    if (part->tech == TECH_TTL_OC) return DRV_Z;
    return (p->flags & PIN_OUT) ? DRIVE(S_PULL, 1) : DRV_Z;
}

/* Combine the drives counted on a net: the strongest wins, equal ones
 * fighting produce a contention where low wins. */
static drive_t combine(const uint16_t *count, int *contention) {
    for (int st = S_SUPPLY; st >= S_PULL; st--) {
        int lo = count[DRIVE(st, 0)], hi = count[DRIVE(st, 1)];
        if (lo || hi) { *contention = lo && hi; return DRIVE(st, lo ? 0 : 1); }
    }
    *contention = 0;
    return DRV_Z;
}

/* Resolve a root net. If its level or "drivenness" changed, every part that
 * reads it must be re-evaluated. 'cause_part' is the part whose drive just
 * changed: analog switches don't need to look at their own drive. */
static void resolve(sim_t *s, int root, int cause_part) {
    net_t *n = &s->nets[root];
    int contention;
    drive_t d = combine(n->count, &contention);
    int st = d >> 1;
    /* A floating net keeps its level (the charge on the copper). */
    int level = st == S_Z ? n->level : (d & 1);
    n->contention = contention;
    /* An analog switch passes the other drivers through: it must look again
     * whenever any of them changes, even if the resulting level doesn't. */
    for (int i = 0; i < n->nswitches; i++)
        if (n->switches[i] != cause_part) sim_mark_dirty(s, n->switches[i]);
    if (level == n->level && (st == S_Z) == (n->strength == S_Z)) { n->strength = st; return; }
    if (st == S_Z && n->strength != S_Z) {      /* nobody drives it anymore */
        n->float_since = s->cycle;
        n->level = level;
        set_hold(s, n);
    }
    n->level = level;
    n->strength = st;
    /* Keep the dense copies used by sim_read() and sim_read_group(). */
    int floating = st == S_Z;
    s->nread[root] = (uint8_t)(level | (floating << 1));
    if (n->busbits) {
        s->bus_level = level ? s->bus_level | n->busbits : s->bus_level & ~n->busbits;
        s->bus_float = floating ? s->bus_float | n->busbits : s->bus_float & ~n->busbits;
        s->bus_erratic &= ~n->busbits;
    }
    /* Every reader looks again, including the part that caused the change:
     * a chip can read its own output back (U14 uses two of its sections as
     * inverters feeding the other two). */
    for (int i = 0; i < n->nreaders; i++) sim_mark_dirty(s, n->readers[i]);
}

/* Slow path of sim_drive(): the drive of an unlocked pin changed. */
void sim_drive_changed(sim_t *s, pin_t *p) {
    drive_t eff = p->drive;
    if (eff == p->eff) return;
    net_t *n = &s->nets[p->root];
    if (p->eff >> 1) n->count[p->eff]--;
    if (eff >> 1) n->count[eff]++;
    p->eff = eff;
    resolve(s, p->root, p->part);
}

void sim_set_pin_fault(sim_t *s, int pin, int fault) {
    pin_t *p = &s->pins[pin];
    if (fault == PF_OPEN && p->fault != PF_OPEN) {
        p->held = (uint8_t)sim_read(s, pin);
        s->open_since[pin] = s->cycle;
    }
    p->fault = (uint8_t)fault;
}

drive_t sim_drive_except(sim_t *s, int pin_on_net, int except_pin) {
    const pin_t *p = &s->pins[pin_on_net];
    if (p->root < 0 || p->fault == PF_OPEN) return DRV_Z;
    /* Like combine(), with the excluded pin's own drive taken out. */
    const uint16_t *count = s->nets[p->root].count;
    const pin_t *x = &s->pins[except_pin];
    drive_t skip = x->root == p->root ? x->eff : DRV_Z;
    for (int st = S_SUPPLY; st >= S_PULL; st--) {
        int lo = count[DRIVE(st, 0)] - (skip == DRIVE(st, 0));
        int hi = count[DRIVE(st, 1)] - (skip == DRIVE(st, 1));
        if (lo || hi) return DRIVE(st, lo ? 0 : 1);
    }
    return DRV_Z;
}

/* ------------------------------------------------------------------------ */
/* Group reads                                                               */
/* ------------------------------------------------------------------------ */

/* A group can be read from the bus bitmap if its pins sit, in order, on
 * consecutive bits of the bitmap and none of them is open (an open pin
 * reads its own remembered level, not the net). */
static void group_setup(sim_t *s, part_t *p, group_t *g, const int *numbers, int n) {
    g->epoch = s->epoch;
    g->first = -1;
    g->mask = g->ttl = 0;
    int first = -1;
    for (int i = 0; i < n; i++) {
        int pi = p->pinmap[numbers[i]];
        if (pi < 0) return;
        const pin_t *pin = &s->pins[pi];
        if (pin->fault == PF_OPEN || pin->root < 0) return;
        int bit = s->nets[pin->net].busbit;
        if (bit < 0 || (i > 0 && bit != first + i)) return;
        if (i == 0) first = bit;
        if (pin->flags & PIN_TTL) g->ttl |= 1ULL << i;
    }
    g->first = first;
    g->mask = n == 64 ? ~0ULL : (1ULL << n) - 1;
}

unsigned sim_read_group(sim_t *s, part_t *p, const int *numbers, int n) {
    group_t *g = NULL;
    for (int i = 0; i < PART_GROUPS; i++) {
        group_t *c = &p->groups[i];
        if (c->key == numbers && c->n == n) { g = c; break; }
        if (!c->key) { g = c; c->key = numbers; c->n = n; c->epoch = s->epoch - 1; break; }
    }
    if (g && g->epoch != s->epoch) group_setup(s, p, g, numbers, n);
    if (g && g->first >= 0) {
        /* Level of every line, plus 1 for TTL inputs on floating lines. An
         * erratic line read by a MOS input goes the slow way below. */
        uint64_t level = s->bus_level >> g->first, floating = s->bus_float >> g->first;
        if (!((s->bus_erratic >> g->first) & g->mask & ~g->ttl))
            return (unsigned)((level & g->mask) | (floating & g->ttl));
    }
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)pin_in(s, p, numbers[i]) << i;
    return v;
}

/* ------------------------------------------------------------------------ */
/* Evaluation                                                                */
/* ------------------------------------------------------------------------ */

/* A CMOS chip with an open power pin works if one of its pins gives it
 * power through the protection diodes: a high pin without VCC, a low pin
 * without GND. If none does, its outputs are off. */
static bool phantom_powered(sim_t *s, part_t *p) {
    for (int k = 0; k < p->npins; k++) {
        int pi = p->first_pin + k;
        if (s->pins[pi].flags & PIN_POWER) continue;
        if (sim_read(s, pi) == (p->power == PWR_NO_VCC)) return true;
    }
    for (int k = 0; k < p->npins; k++)
        if (!(s->pins[p->first_pin + k].flags & PIN_POWER)) sim_drive(s, p->first_pin + k, DRV_Z);
    return false;
}

void sim_settle(sim_t *s) {
    /* Evaluate dirty parts until the board is stable. A board with a fault
     * can oscillate (an output feeding its own input through a short), so
     * give up after a large number of evaluations. */
    int budget = 20000;
    while (s->qhead != s->qtail && budget--) {
        int part = s->queue[s->qhead++ & (SIM_QUEUE_SIZE - 1)];
        s->dirty[part] = 0;
        part_t *p = &s->parts[part];
        if (p->dead) continue;
        if (p->power != PWR_OK && !phantom_powered(s, p)) continue;
        p->eval(s, p);
    }
    while (s->qhead != s->qtail)        /* oscillating: drop pending work */
        s->dirty[s->queue[s->qhead++ & (SIM_QUEUE_SIZE - 1)]] = 0;
}

void sim_commit(sim_t *s) {
    for (int i = 0; i < s->ncommit; i++) {
        part_t *p = &s->parts[s->commit_list[i]];
        if (!p->dead) p->commit(s, p);
    }
}

void sim_update_part_power(sim_t *s, int part) {
    part_t *p = &s->parts[part];
    bool no_vcc = false, no_gnd = false;
    for (int i = 0; i < p->npins; i++) {
        int pi = p->first_pin + i;
        if (!(s->pins[pi].flags & PIN_POWER) || s->pins[pi].fault != PF_OPEN) continue;
        const char *name = s->pin_names[pi];
        if (name && (strstr(name, "GND") || !strcmp(name, "VSS"))) no_gnd = true;
        else no_vcc = true;
    }
    if (p->removed || (no_vcc && no_gnd)) p->power = PWR_OFF;
    else if (no_vcc) p->power = PWR_NO_VCC;
    else if (no_gnd) p->power = PWR_NO_GND;
    else p->power = PWR_OK;
    p->dead = p->power == PWR_OFF || (p->power != PWR_OK && p->tech != TECH_CMOS);
}
