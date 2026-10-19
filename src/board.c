/* board.c - what every board has, whatever the machine: setting it up
 * from its netlist, the passive parts, faults, the network rebuild, the
 * oscilloscope and the logic probe statistics, labels and connectors.
 * The machines (c64.c) add the cores of their chips and the bus cycle
 * that moves them, through their machine_t. */
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Is 'word' one of the space separated words of 'list'? */
static bool in_list(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *s = list; s && *s; ) {
        size_t len = strcspn(s, " ");
        if (len == n && !strncmp(s, word, n)) return true;
        s += len + (s[len] == ' ');
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Scope and statistics                                                      */
/* ------------------------------------------------------------------------ */

void board_sample(board_t *b) {
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
    if (!b->pin_stats) return;
    for (int i = 0; i < s->npins; i++) {
        drive_t d = s->pins[i].drive;
        if ((d >> 1) == S_Z) b->pst_off[i]++;
        else if (d & 1) b->pst_hi[i]++;
        else b->pst_lo[i]++;
        if ((d >> 1) == S_PULL) b->pst_weak[i]++;
    }
    /* The PLA inputs are TTL: reading them has no side effects. */
    if (b->p_pla < 0) return;
    part_t *pla = &s->parts[b->p_pla];
    if (!pla->dead) {
        unsigned in = pla_read_inputs(s, pla);
        for (int t = 0; t < pla_nterms; t++)
            if ((in & pla_terms[t].mask) == pla_terms[t].value) b->pla_hits[t]++;
        b->pla_samples++;
    }
}

/* ------------------------------------------------------------------------ */
/* Labels                                                                    */
/* ------------------------------------------------------------------------ */

static const part_def_t *part_def(board_t *b, int part) {
    const char *ref = b->sim.parts[part].ref;
    for (int i = 0; i < b->m->nparts; i++)
        if (!strcmp(b->m->parts[i].ref, ref)) return &b->m->parts[i];
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

/* ------------------------------------------------------------------------ */
/* Passive parts                                                             */
/* ------------------------------------------------------------------------ */

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
     * NTSC on the C64): it connects nothing. */
    if (strstr(p->name, "DASHED")) npairs = 0;
    /* Series resistors that are part of the logic (the others are analog). */
    bool series = in_list(b->m->link_resistors, p->ref);
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
    if (in_list(b->m->crystals, p->ref)) return true;
    if (p->kind == PART_RESISTOR || p->kind == PART_LINK) {
        resistor_state_t *st = p->state;
        for (int i = 0; st && i < st->nel; i++) if (st->el[i].type != EL_ANALOG) return true;
    }
    return false;
}

/* Rebuild the electrical network after a change: links, keys, faults. */
void board_rebuild(board_t *b) {
    sim_t *s = &b->sim;
    sim_join_begin(s);
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if ((p->kind != PART_RESISTOR && p->kind != PART_LINK) || !p->state) continue;
        if (p->rfault == RF_OPEN) continue;
        resistor_state_t *st = p->state;
        for (int k = 0; k < st->nel; k++) {
            element_t *e = &st->el[k];
            /* A shorted series resistor is a wire. */
            if (e->type != EL_LINK && !(e->type == EL_SERIES && p->rfault == RF_SHORT)) continue;
            if (s->pins[e->pin_a].fault == PF_OPEN || s->pins[e->pin_b].fault == PF_OPEN) continue;
            sim_join(s, s->pins[e->pin_a].net, s->pins[e->pin_b].net);
        }
    }
    /* Pressed keys, dongles... */
    if (b->m->joins) b->m->joins(b);
    /* The RESET button closes /RESET to GND. */
    if (b->reset_button && b->pin_reset >= 0 && b->pin_reset_gnd >= 0 &&
        s->pins[b->pin_reset].fault != PF_OPEN && s->pins[b->pin_reset_gnd].fault != PF_OPEN)
        sim_join(s, s->pins[b->pin_reset].net, s->pins[b->pin_reset_gnd].net);
    for (int i = 0; i < s->nparts; i++) sim_update_part_power(s, i);
    sim_rebuild(s);
}

/* ------------------------------------------------------------------------ */
/* Connectors                                                                */
/* ------------------------------------------------------------------------ */

/* Does a pad name match? The C64 cassette port pads are named after both
 * sides of the edge connector ("D,4"): either name matches. */
static bool pad_matches(const char *name, const char *pad) {
    size_t n = strlen(pad);
    for (const char *s = name; *s; ) {
        size_t len = strcspn(s, ",");
        if (len == n && !strncmp(s, pad, n)) return true;
        s += len + (s[len] == ',');
    }
    return false;
}

int board_pad_pin(board_t *b, int part, const char *pad) {
    if (part < 0) return -1;
    const part_t *p = &b->sim.parts[part];
    for (int k = 0; k < p->npins; k++)
        if (pad_matches(b->sim.bd->pads[p->first_pin + k].name, pad)) return p->first_pin + k;
    return -1;
}

static void setup_connectors(board_t *b) {
    sim_t *s = &b->sim;
    b->pin_func = calloc(s->npins, sizeof(char *));
    b->conn_strings = calloc((size_t)b->m->nconns + 1, sizeof(char *));
    for (int c = 0; c < b->m->nconns; c++) {
        int part = sim_find_part(s, b->m->conns[c].ref);
        if (part < 0) continue;
        /* The copy stays allocated: its pieces are the labels. */
        char *copy = strdup(b->m->conns[c].pins), *save = NULL;
        b->conn_strings[c] = copy;
        for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            char *eq = strchr(tok, '=');
            *eq = 0;
            for (char *u = eq + 1; *u; u++) if (*u == '_') *u = ' ';
            /* A pad name can repeat (both sides of an edge connector). */
            const part_t *p = &s->parts[part];
            for (int k = 0; k < p->npins; k++)
                if (pad_matches(s->bd->pads[p->first_pin + k].name, tok)) b->pin_func[p->first_pin + k] = eq + 1;
        }
    }
}

const char *board_connector_name(board_t *b, int part) {
    for (int c = 0; c < b->m->nconns; c++)
        if (!strcmp(b->m->conns[c].ref, b->sim.parts[part].ref)) return b->m->conns[c].name;
    return NULL;
}

const char *board_pin_func(board_t *b, int pin) { return pin >= 0 ? b->pin_func[pin] : NULL; }

bool board_net_simulated(board_t *b, int net) {
    if (net < 0) return false;
    const sim_t *s = &b->sim;
    const net_t *n = &s->nets[s->nets[net].root];
    if (n->rail) return true;
    for (int k = 0; k < n->npins; k++) {
        int part = s->pins[n->pins[k]].part;
        if (s->parts[part].kind == PART_IC && board_part_simulated(b, part)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Life                                                                      */
/* ------------------------------------------------------------------------ */

board_t *board_create(const machine_t *m) {
    board_t *b = calloc(1, m->size);
    b->m = m;
    sim_t *s = &b->sim;
    sim_init(s, m->data, &m->sim);
    for (int i = 0; i < s->nparts; i++) s->parts[i].user = b;
    for (int i = 0; i < m->nparts; i++) {
        int idx = sim_find_part(s, m->parts[i].ref);
        if (idx >= 0) part_set_type(s, idx, m->parts[i].type);
    }
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].kind == PART_RESISTOR || s->parts[i].kind == PART_LINK) setup_passive(b, i);
    b->screen = calloc((size_t)m->screen_w * m->screen_h, sizeof(uint32_t));
    b->check = getenv("C64HW_CHECK") != NULL;
    b->scope_pin = -1;
    b->p_pla = b->pin_reset = b->pin_reset_gnd = -1;
    b->st_hi = calloc(s->nnets, sizeof(uint32_t));
    b->st_lo = calloc(s->nnets, sizeof(uint32_t));
    b->st_float = calloc(s->nnets, sizeof(uint32_t));
    b->st_fight = calloc(s->nnets, sizeof(uint32_t));
    b->pst_hi = calloc(s->npins, sizeof(uint32_t));
    b->pst_lo = calloc(s->npins, sizeof(uint32_t));
    b->pst_off = calloc(s->npins, sizeof(uint32_t));
    b->pst_weak = calloc(s->npins, sizeof(uint32_t));
    setup_connectors(b);
    m->create(b);
    board_power_on(b);
    return b;
}

void board_destroy(board_t *b) {
    if (b->m->destroy) b->m->destroy(b);
    sim_free(&b->sim);
    for (int c = 0; c < b->m->nconns; c++) free(b->conn_strings[c]);
    free(b->conn_strings);
    free(b->pin_func);
    free(b->screen);
    free(b->st_hi); free(b->st_lo); free(b->st_float); free(b->st_fight);
    free(b->pst_hi); free(b->pst_lo); free(b->pst_off); free(b->pst_weak);
    free(b);
}

/* Cold start: like switching the machine on. Faults stay where they are. */
void board_power_on(board_t *b) {
    sim_t *s = &b->sim;
    s->cycle = 0;
    b->m->power_on(b);
    /* Nets start discharged, parts in their power-on state, and the noise
     * starts over (the same faults give the same run). */
    sim_discharge(s);
    sim_seed(s, 1);
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].reset) s->parts[i].reset(s, &s->parts[i]);
    board_rebuild(b);
}

void board_run(board_t *b, int cycles) { b->m->run(b, cycles); }

void board_key_down(board_t *b, int key) { b->m->key_down(b, key); }
void board_key_up(board_t *b, int key) { b->m->key_up(b, key); }
void board_frame_update(board_t *b, uint32_t us) { b->m->frame_update(b, us); }
void board_monitor(board_t *b, uint32_t *out) { b->m->monitor(b, out); }

bool board_load(board_t *b, const char *path, bool run, char *msg, int msglen) {
    return b->m->load(b, path, run, msg, msglen);
}

const char *board_cart_name(board_t *b) { return b->m->cart_name ? b->m->cart_name(b) : NULL; }
void board_remove_cart(board_t *b) { if (b->m->remove_cart) b->m->remove_cart(b); }

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

/* The RESET button: not a fault, a switch (see board_rebuild). */
void board_set_reset_button(board_t *b, bool pressed) {
    if (b->reset_button == pressed) return;
    b->reset_button = pressed;
    board_rebuild(b);
}

/* Text form of a fault, used by the command line:
 *   U17:removed  U17.18:gnd  U17.18:vcc  U17.18:open  R51:open  R51:short
 * and the RESET button: RESET:pressed  RESET:released */
bool board_apply_fault_spec(board_t *b, const char *spec) {
    if (!strcmp(spec, "RESET:pressed") || !strcmp(spec, "RESET:released")) {
        board_set_reset_button(b, !strcmp(spec, "RESET:pressed"));
        return true;
    }
    char ref[16] = {0}, pad[8] = {0}, what[16] = {0};
    if (sscanf(spec, "%15[^.:].%7[^:]:%15s", ref, pad, what) != 3 &&
        sscanf(spec, "%15[^:]:%15s", ref, what) != 2) return false;
    int part = sim_find_part(&b->sim, ref);
    if (part < 0) return false;
    if (pad[0]) {       /* a pin, by its pad name: "18", or "B" on CN6 */
        int pi = board_pad_pin(b, part, pad);
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
    n = (size_t)b->sim.npins * sizeof(uint32_t);
    memset(b->pst_hi, 0, n); memset(b->pst_lo, 0, n);
    memset(b->pst_off, 0, n); memset(b->pst_weak, 0, n);
    memset(b->pla_hits, 0, sizeof(b->pla_hits));
    b->pla_samples = 0;
}

int board_pin_drive_state(board_t *b, int pin) {
    uint32_t hi = b->pst_hi[pin], lo = b->pst_lo[pin], off = b->pst_off[pin];
    if (hi + lo + off == 0) {   /* no samples yet: the current drive */
        drive_t d = b->sim.pins[pin].drive;
        return (d >> 1) == S_Z ? NET_FLOATING : (d & 1) ? NET_HIGH : NET_LOW;
    }
    if ((hi && lo) || ((hi || lo) && off)) return NET_PULSING;
    if (hi) return NET_HIGH;
    if (lo) return NET_LOW;
    return NET_FLOATING;
}
