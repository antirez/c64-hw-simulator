/* harness.c - the test harness of the Diagnostic Rev. 586220 cartridge.
 *
 * The 586220 diagnostic tests the ports by looping them back to the C64:
 * dongles on the user port, the cassette port, the serial port, both
 * control ports and the keyboard connector. This is Sven Petersen's open
 * hardware build of it (Rev. 3, github.com/svenpetersen1965/c64-diagnostic-
 * rev.-586220-harness), which does what Commodore's does and also fixes
 * its false "OK" of the control port test. Electrically:
 *
 *  - wires: the user port loopbacks (/FLAG2-/PC2, PB0-3 to PB4-7, CNT1-CNT2,
 *    SP1-SP2, ATN-PA2), cassette READ-SENSE, serial SRQ-DATA and ATN-CLK,
 *    keyboard lines PA5-PA7 to PB5-PB7 (keyboard dongle);
 *  - the cassette MOTOR (about 6 V) drives WRITE through a 150/316 ohm
 *    divider, which overpowers the C64's own WRITE output: the diagnostic
 *    switches the harness by turning the motor on and off;
 *  - 4066 switches: with WRITE high the two control ports are connected
 *    line by line (up, down, left, right, fire), with WRITE low the
 *    keyboard lines PA0-PA4 are connected to PB0-PB4 (these are shared
 *    with the joysticks: connected for good, they would make the control
 *    port test pass on a broken port);
 *  - 110K from each control port's +5V to its POT X and POT Y lines, which
 *    the SID reads (see board_pot_value()).
 *
 * Like the keys and the RESET button, the connections are joins of nets,
 * made when the network is rebuilt; a switch changing rebuilds it. */
#include "c64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Connector pins as "REF.PAD". */
static const char *const wires[][2] = {
    { "CN2.B", "CN2.8" },   /* /FLAG2 - /PC2 */
    { "CN2.C", "CN2.H" },   /* PB0 - PB4 */
    { "CN2.D", "CN2.J" },   /* PB1 - PB5 */
    { "CN2.E", "CN2.K" },   /* PB2 - PB6 */
    { "CN2.F", "CN2.L" },   /* PB3 - PB7 */
    { "CN2.4", "CN2.6" },   /* CNT1 - CNT2 */
    { "CN2.5", "CN2.7" },   /* SP1 - SP2 */
    { "CN2.9", "CN2.M" },   /* SERIAL ATN - PA2 */
    { "CN3.D", "CN3.F" },   /* cassette READ - SENSE */
    { "CN4.1", "CN4.5" },   /* serial SRQ IN - DATA */
    { "CN4.3", "CN4.4" },   /* serial ATN - CLK */
    { "CN1.7", "CN1.15" },  /* keyboard ROW5 (PB5) - COL5 (PA5) */
    { "CN1.6", "CN1.14" },  /* ROW6 - COL6 */
    { "CN1.9", "CN1.20" },  /* ROW7 - COL7 */
};
/* Closed while WRITE is high: control port 1 to control port 2. */
static const char *const joy_switches[][2] = {
    { "CN9.1", "CN8.1" }, { "CN9.2", "CN8.2" }, { "CN9.3", "CN8.3" }, { "CN9.4", "CN8.4" },
    { "CN9.6", "CN8.6" },
};
/* Closed while WRITE is low: keyboard COLn (PAn) to ROWn (PBn), n = 0-4. */
static const char *const kbd_switches[][2] = {
    { "CN1.13", "CN1.12" }, { "CN1.19", "CN1.11" }, { "CN1.18", "CN1.10" }, { "CN1.17", "CN1.5" },
    { "CN1.16", "CN1.8" },
};
/* 110K from the port's +5V: POT AX, POT AY (port 1), POT BX, POT BY (port 2). */
static const char *const paddles[] = { "CN9.9", "CN9.5", "CN8.9", "CN8.5" };
#define PADDLE_OHM 110e3

/* The pin "REF.PAD", or -1. */
static int find_pin(board_t *b, const char *spec) {
    char ref[8];
    const char *dot = strchr(spec, '.');
    snprintf(ref, sizeof(ref), "%.*s", (int)(dot - spec), spec);
    return board_pad_pin(b, sim_find_part(&b->sim, ref), dot + 1);
}

/* A dongle contact touches the pin, unless the pin is broken (open). */
static void join_pins(board_t *b, const char *a, const char *c) {
    sim_t *s = &b->sim;
    int pa = find_pin(b, a), pc = find_pin(b, c);
    if (pa < 0 || pc < 0 || s->pins[pa].fault == PF_OPEN || s->pins[pc].fault == PF_OPEN) return;
    sim_join(s, s->pins[pa].net, s->pins[pc].net);
}

void harness_joins(board_t *b) {
    c64_t *c64 = C64(b);
    if (!c64->harness) return;
    for (size_t i = 0; i < sizeof(wires) / sizeof(wires[0]); i++) join_pins(b, wires[i][0], wires[i][1]);
    if (c64->harness_write)
        for (size_t i = 0; i < sizeof(joy_switches) / sizeof(joy_switches[0]); i++)
            join_pins(b, joy_switches[i][0], joy_switches[i][1]);
    else
        for (size_t i = 0; i < sizeof(kbd_switches) / sizeof(kbd_switches[0]); i++)
            join_pins(b, kbd_switches[i][0], kbd_switches[i][1]);
}

/* Every cycle: the motor drives WRITE through the divider, and WRITE
 * flips the switches (rebuilding the network when it changes). */
void harness_tick(board_t *b) {
    c64_t *c64 = C64(b);
    if (!c64->harness || c64->pin_hmotor < 0 || c64->pin_hwrite < 0) return;
    sim_t *s = &b->sim;
    const pin_t *m = &s->pins[c64->pin_hmotor];
    bool motor = m->root >= 0 && m->fault != PF_OPEN && s->nets[m->root].strength != S_Z && s->nets[m->root].level;
    sim_drive(s, c64->pin_hwrite, DRIVE(S_SUPPLY, motor));
    const pin_t *w = &s->pins[c64->pin_hwrite];
    bool write = w->root >= 0 && s->nets[w->root].level;
    if (write != c64->harness_write) {
        c64->harness_write = write;
        board_rebuild(b);
    }
}

void board_set_harness(board_t *b, bool plugged) {
    if (!board_is_c64(b)) return;
    c64_t *c64 = C64(b);
    if (c64->harness == plugged) return;
    c64->pin_hmotor = find_pin(b, "CN3.C");
    c64->pin_hwrite = find_pin(b, "CN3.E");
    c64->harness = plugged;
    c64->harness_write = false;
    if (c64->pin_hwrite >= 0) b->sim.pins[c64->pin_hwrite].drive = DRV_Z;
    /* Dongles go in with the power off: switch the C64 on again. */
    board_rebuild(b);
    board_power_on(b);
}

bool board_harness(board_t *b) { return board_is_c64(b) && C64(b)->harness; }

/* What the SID reads on a POT pin: the time the board's capacitor on that
 * line (C48, C93: 1800 pF) takes to charge through the paddle resistors
 * that reach it, through the U28 switches selected by CIA1 PA6/PA7. The
 * 6581 data sheet: RC = 4.7E-4 s reads 255 (470K, 1000 pF). Nothing
 * connected: the capacitor never charges, 255. */
int board_pot_value(board_t *b, int sid_pin) {
    sim_t *s = &b->sim;
    int root = s->pins[sid_pin].root;
    if (root < 0 || s->pins[sid_pin].fault == PF_OPEN) return 255;
    /* The capacitor on the line, from its value in the board data. */
    double cap = 0;
    const net_t *n = &s->nets[root];
    for (int k = 0; k < n->npins; k++) {
        const part_t *p = &s->parts[s->pins[n->pins[k]].part];
        if (p->ref[0] != 'C' || p->kind != PART_OTHER) continue;
        double v = atof(p->name);
        cap += strstr(p->name, "pF") ? v * 1e-12 : strstr(p->name, "uF") ? v * 1e-6 : v * 1e-6;
    }
    /* The paddles reaching the line through a closed U28 switch. */
    double g = 0;
    int u28 = C64(b)->p_u28;
    if (C64(b)->harness && u28 >= 0 && !s->parts[u28].dead) {
        static const int sw[4][3] = { {1, 2, 13}, {4, 3, 5}, {8, 9, 6}, {11, 10, 12} };
        const part_t *p = &s->parts[u28];
        for (int i = 0; i < 4; i++) {
            int a = part_pin(p, sw[i][0]), c = part_pin(p, sw[i][1]), ctl = part_pin(p, sw[i][2]);
            if (a < 0 || c < 0 || ctl < 0) continue;
            const pin_t *pc = &s->pins[ctl];
            if (pc->root < 0 || !s->nets[pc->root].level || s->nets[pc->root].strength == S_Z) continue;
            int other = s->pins[a].root == root ? c : s->pins[c].root == root ? a : -1;
            if (other < 0 || s->pins[a].fault == PF_OPEN || s->pins[c].fault == PF_OPEN) continue;
            for (size_t k = 0; k < sizeof(paddles) / sizeof(paddles[0]); k++) {
                int pp = find_pin(b, paddles[k]);
                if (pp >= 0 && s->pins[pp].root == s->pins[other].root && s->pins[pp].fault != PF_OPEN)
                    g += 1.0 / PADDLE_OHM;
            }
        }
    }
    if (g <= 0 || cap <= 0) return 255;
    double v = 255.0 * (cap / g) / 4.7e-4;
    return v > 255 ? 255 : (int)v;
}
