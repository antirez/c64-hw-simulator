/* inspect.c - the chip inspector and the logic diagrams.
 *
 * "Inspect" in a chip's menu opens a window with the chip drawn large, as
 * in a datasheet: the pins numbered and colored like on the board (the
 * state of their net), the net names outside, and for the logic chips the
 * gates inside, wired to their pins.
 *
 * A wire has the color of what is on it: the net, or, when the leg is
 * lifted, what the chip sees (a TTL input reads 1, a MOS input floats) or
 * drives. The output of every gate is marked with what the gate itself
 * drives, so a gate driving high into a net that stays low is plain to
 * see. For open collector outputs (7406) the mark is the transistor: blue
 * while it pulls the line low, gray while it is off and the pull-up (or
 * someone else) decides the level.
 *
 * For the PLA the inspector also lists its product terms, lit while they
 * are true. Nothing here changes the simulation: the states come from the
 * statistics the board samples (see board.h). */
#include "inspect.h"
#include <math.h>

/* ------------------------------------------------------------------------ */
/* What is inside the logic chips                                            */
/* ------------------------------------------------------------------------ */

enum { G_INV, G_AND, G_MUX, G_LATCH, G_SWITCH, G_BOX };

typedef struct {
    int kind;
    int8_t in[2], out;      /* input and output pins (0: none) */
    int8_t ctl;             /* G_SWITCH: the pin that closes it */
    int8_t pins[14];        /* G_BOX: the pins it covers, 0 terminated */
    const char *label;      /* G_BOX: what it is */
} gate_t;

typedef struct {
    const char *type;       /* part type name, as in parts.c */
    bool oc, inv;           /* open collector / inverting outputs */
    int8_t common[2];       /* pins every gate shares: S and /G, LE and /OE */
    int n;
    gate_t g[8];
} diagram_t;

#define INV(a, o)       { G_INV, {a, 0}, o, 0, {0}, NULL }
#define AND(a, b, o)    { G_AND, {a, b}, o, 0, {0}, NULL }
#define MUX(a, b, o)    { G_MUX, {a, b}, o, 0, {0}, NULL }
#define LATCH(d, q)     { G_LATCH, {d, 0}, q, 0, {0}, NULL }
#define SWITCH(a, b, c) { G_SWITCH, {a, b}, 0, c, {0}, NULL }
#define BOX(label, ...) { G_BOX, {0, 0}, 0, 0, {__VA_ARGS__}, label }
#define MUX4 { MUX(2, 3, 4), MUX(5, 6, 7), MUX(11, 10, 9), MUX(14, 13, 12) }

/* The gates of every logic chip, by pin number (datasheet pinouts). */
static const diagram_t diagrams[] = {
    { "7406", true, false, {0, 0}, 6,
      { INV(1, 2), INV(3, 4), INV(5, 6), INV(9, 8), INV(11, 10), INV(13, 12) } },
    { "74LS08", false, false, {0, 0}, 4,
      { AND(1, 2, 3), AND(4, 5, 6), AND(9, 10, 8), AND(12, 13, 11) } },
    { "74LS257", false, false, {1, 15}, 4, MUX4 },      /* S, /G */
    { "74LS258", false, true, {1, 15}, 4, MUX4 },
    { "74LS373", false, false, {11, 1}, 8,              /* LE, /OE */
      { LATCH(3, 2), LATCH(4, 5), LATCH(7, 6), LATCH(8, 9),
        LATCH(13, 12), LATCH(14, 15), LATCH(17, 16), LATCH(18, 19) } },
    { "74LS139", false, false, {0, 0}, 2,
      { BOX("2 to 4", 1, 2, 3, 4, 5, 6, 7), BOX("2 to 4", 9, 10, 11, 12, 13, 14, 15) } },
    { "4066", false, false, {0, 0}, 4,
      { SWITCH(1, 2, 13), SWITCH(4, 3, 5), SWITCH(8, 9, 6), SWITCH(11, 10, 12) } },
    { "556 TIMER", false, false, {0, 0}, 2,
      { BOX("timer 1", 1, 2, 3, 4, 5, 6), BOX("timer 2", 8, 9, 10, 11, 12, 13) } },
    { "74LS629 VCO", false, false, {0, 0}, 1,
      { BOX("2 VCOs", 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15) } },
    { "74LS74", false, false, {0, 0}, 2,
      { BOX("D flip-flop", 1, 2, 3, 4, 5, 6), BOX("D flip-flop", 8, 9, 10, 11, 12, 13) } },
    { "74LS193", false, false, {0, 0}, 1,
      { BOX("counter", 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15) } },
    { "MC4044 PLL", false, false, {0, 0}, 1,
      { BOX("phase detector", 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13) } },
};

static const diagram_t *find_diagram(board_t *b, int part) {
    const part_t *p = &b->sim.parts[part];
    if (p->kind != PART_IC) return NULL;
    for (size_t i = 0; i < sizeof(diagrams) / sizeof(diagrams[0]); i++)
        if (!strcmp(diagrams[i].type, p->name)) return &diagrams[i];
    return NULL;
}

bool chip_has_diagram(board_t *b, int part) { return find_diagram(b, part) != NULL; }

/* ------------------------------------------------------------------------ */
/* States of the wires                                                       */
/* ------------------------------------------------------------------------ */

/* What is on the wire between a pin and the gate inside: the net if the
 * leg is soldered, otherwise what the chip sees (input) or drives. */
static int wire_state(board_t *b, const uint8_t *ns, int pin) {
    const pin_t *p = &b->sim.pins[pin];
    const part_t *part = &b->sim.parts[p->part];
    if (part->removed) return NET_NONE;
    if (p->fault != PF_OPEN && p->net >= 0) return ns[p->net];
    if (p->flags & PIN_OUT) return part->dead ? NET_FLOATING : board_pin_drive_state(b, pin);
    return (p->flags & PIN_TTL) ? NET_HIGH : NET_FLOATING;
}

/* What a gate output itself drives. */
static int drive_state(board_t *b, int pin) {
    const part_t *part = &b->sim.parts[b->sim.pins[pin].part];
    if (part->removed) return NET_NONE;
    if (part->dead) return NET_FLOATING;
    return board_pin_drive_state(b, pin);
}

/* ------------------------------------------------------------------------ */
/* Drawing the gates                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    struct nk_command_buffer *cv;
    struct nk_context *ctx;
    board_t *b;
    const uint8_t *ns;
    int part;
    const pinpos_t *pos;
    float u, pitch, t;      /* size unit, pin distance, line thickness */
    struct nk_color ink;    /* the color of the symbols */
} dc_t;

/* The drawing code works with pin numbers (1..N), like the datasheet;
 * these convert them to the board's pins for the colors. */
static int pidx(dc_t *d, int num) { return part_pin(&d->b->sim.parts[d->part], num); }

static int wstate(dc_t *d, int num) {
    int pin = pidx(d, num);
    return pin < 0 ? NET_NONE : wire_state(d->b, d->ns, pin);
}

static struct nk_color wcol(dc_t *d, int num) { return state_color(wstate(d, num)); }

static struct nk_color dcol(dc_t *d, int num) {
    int pin = pidx(d, num);
    return state_color(pin < 0 ? NET_NONE : drive_state(d->b, pin));
}

static void line(dc_t *d, float x0, float y0, float x1, float y1, struct nk_color c) {
    nk_stroke_line(d->cv, x0, y0, x1, y1, d->t, c);
}

/* Points of an arc, from angle a0 to a1 (nuklear's own stroke_arc draws a
 * pie slice, with lines to the center). */
static int arc_points(float *pts, float cx, float cy, float r, float a0, float a1, int n) {
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        pts[2 * i] = cx + r * cosf(a);
        pts[2 * i + 1] = cy + r * sinf(a);
    }
    return n + 1;
}

/* A dot showing what a gate output drives. */
static void out_mark(dc_t *d, float x, float y, int num) {
    float r = d->u * 0.09f;
    nk_fill_circle(d->cv, nk_rect(x - r, y - r, 2 * r, 2 * r), dcol(d, num));
}

/* The output wire: from the gate (x, y) a little further inward, then to
 * the height of the output pin, then back out to it. */
static void out_wire(dc_t *d, float x, float y, int dir, int num) {
    struct nk_color c = wcol(d, num);
    float xr = x + dir * d->u * 0.14f, ex = d->pos[num].x, yo = d->pos[num].y;
    line(d, x, y, xr, y, c);
    line(d, xr, y, xr, yo, c);
    line(d, xr, yo, ex, yo, c);
}

/* Input wires of a two input gate: straight in from the pins, then a jog
 * to the gate terminals (yt0 for the upper pin, yt1 for the lower one). */
static void in_wires2(dc_t *d, const gate_t *g, float x0, float yt0, float yt1) {
    int a = g->in[0], b = g->in[1];
    if (d->pos[a].y > d->pos[b].y) { int t = a; a = b; b = t; }
    float ex = d->pos[a].x, xj = (ex + x0) / 2;
    line(d, ex, d->pos[a].y, xj, d->pos[a].y, wcol(d, a));
    line(d, xj, d->pos[a].y, xj, yt0, wcol(d, a));
    line(d, xj, yt0, x0, yt0, wcol(d, a));
    line(d, ex, d->pos[b].y, xj, d->pos[b].y, wcol(d, b));
    line(d, xj, d->pos[b].y, xj, yt1, wcol(d, b));
    line(d, xj, yt1, x0, yt1, wcol(d, b));
}

static void draw_inv(dc_t *d, const diagram_t *dg, const gate_t *g) {
    int dir = -d->pos[g->in[0]].side;
    float u = d->u, y = d->pos[g->in[0]].y, ex = d->pos[g->in[0]].x;
    float x0 = ex + dir * u * 0.3f, w = u * 0.5f, h = u * 0.56f, r = u * 0.1f;
    float tip = x0 + dir * w;
    line(d, ex, y, x0, y, wcol(d, g->in[0]));
    nk_stroke_triangle(d->cv, x0, y - h / 2, x0, y + h / 2, tip, y, d->t, d->ink);
    /* The bubble is the output: filled with what the gate drives. */
    float cx = tip + dir * r;
    nk_fill_circle(d->cv, nk_rect(cx - r, y - r, 2 * r, 2 * r), dcol(d, g->out));
    nk_stroke_circle(d->cv, nk_rect(cx - r, y - r, 2 * r, 2 * r), 1, d->ink);
    if (dg->oc) {   /* open collector: a diamond with a bar under it */
        float mx = x0 + dir * w * 0.36f, s = u * 0.08f;
        float pts[8] = { mx - s, y, mx, y - s, mx + s, y, mx, y + s };
        nk_stroke_polygon(d->cv, pts, 4, 1, d->ink);
        nk_stroke_line(d->cv, mx - s, y + s * 1.6f, mx + s, y + s * 1.6f, 1, d->ink);
    }
    out_wire(d, cx + dir * r, y, dir, g->out);
}

static void draw_and(dc_t *d, const gate_t *g) {
    int dir = -d->pos[g->in[0]].side;
    float u = d->u, ex = d->pos[g->in[0]].x;
    float yc = (d->pos[g->in[0]].y + d->pos[g->in[1]].y) / 2, h = u * 0.9f, w = u * 0.75f;
    float x0 = ex + dir * u * 0.35f, flat = w - h / 2;
    in_wires2(d, g, x0, yc - h / 4, yc + h / 4);
    /* The D: flat back, then a half circle toward the output. */
    float pts[2 * 40];
    int n = 0;
    pts[n++] = x0; pts[n++] = yc + h / 2;
    pts[n++] = x0; pts[n++] = yc - h / 2;
    n += 2 * arc_points(pts + n, x0 + dir * flat, yc, h / 2, -NK_PI / 2, -NK_PI / 2 + dir * NK_PI, 24);
    nk_stroke_polygon(d->cv, pts, n / 2, d->t, d->ink);
    float tip = x0 + dir * w;
    out_wire(d, tip, yc, dir, g->out);
    out_mark(d, tip, yc, g->out);
}

static void draw_mux(dc_t *d, const diagram_t *dg, const gate_t *g) {
    int dir = -d->pos[g->in[0]].side;
    float u = d->u, ex = d->pos[g->in[0]].x;
    float yc = (d->pos[g->in[0]].y + d->pos[g->in[1]].y) / 2, h = u * 0.9f, w = u * 0.5f;
    float x0 = ex + dir * u * 0.35f, x1 = x0 + dir * w;
    in_wires2(d, g, x0, yc - h / 4, yc + h / 4);
    float pts[8] = { x0, yc - h / 2, x1, yc - h * 0.3f, x1, yc + h * 0.3f, x0, yc + h / 2 };
    nk_stroke_polygon(d->cv, pts, 4, d->t, d->ink);
    /* Which input is "0" (A, chosen while S is low). */
    if (w >= text_width(d->ctx, "0") + 10) {
        bool a_up = d->pos[g->in[0]].y < d->pos[g->in[1]].y;
        text_at(d->cv, d->ctx, x0 + dir * 3, yc - h / 4, dir < 0, a_up ? "0" : "1", COL_DIMTEXT);
        text_at(d->cv, d->ctx, x0 + dir * 3, yc + h / 4, dir < 0, a_up ? "1" : "0", COL_DIMTEXT);
    }
    /* Select (top) and enable (bottom), colored like the shared pins. */
    float r = u * 0.07f, mx = (x0 + x1) / 2;
    nk_fill_circle(d->cv, nk_rect(mx - r, yc - h * 0.4f - r, 2 * r, 2 * r), wcol(d, dg->common[0]));
    nk_fill_circle(d->cv, nk_rect(mx - r, yc + h * 0.4f - r, 2 * r, 2 * r), wcol(d, dg->common[1]));
    float tip = x1;
    if (dg->inv) {
        float rb = u * 0.1f, cx = x1 + dir * rb;
        nk_fill_circle(d->cv, nk_rect(cx - rb, yc - rb, 2 * rb, 2 * rb), dcol(d, g->out));
        nk_stroke_circle(d->cv, nk_rect(cx - rb, yc - rb, 2 * rb, 2 * rb), 1, d->ink);
        tip = cx + dir * rb;
        out_wire(d, tip, yc, dir, g->out);
    } else {
        out_wire(d, tip, yc, dir, g->out);
        out_mark(d, tip, yc, g->out);
    }
}

static void draw_latch(dc_t *d, const diagram_t *dg, const gate_t *g) {
    int dir = -d->pos[g->in[0]].side;
    float u = d->u, y = d->pos[g->in[0]].y, ex = d->pos[g->in[0]].x;
    float x0 = ex + dir * u * 0.3f, w = u * 0.65f, h = u * 0.6f, x1 = x0 + dir * w;
    line(d, ex, y, x0, y, wcol(d, g->in[0]));
    nk_stroke_rect(d->cv, nk_rect(fminf(x0, x1), y - h / 2, w, h), 0, d->t, d->ink);
    if (w >= 2 * text_width(d->ctx, "D") + 8) {
        text_at(d->cv, d->ctx, x0 + dir * 2, y, dir < 0, "D", COL_DIMTEXT);
        text_at(d->cv, d->ctx, x1 - dir * 2, y, dir > 0, "Q", COL_DIMTEXT);
    }
    /* Latch enable on top, output enable at the bottom. */
    float r = u * 0.07f, mx = (x0 + x1) / 2;
    nk_fill_circle(d->cv, nk_rect(mx - r, y - h / 2 - r, 2 * r, 2 * r), wcol(d, dg->common[0]));
    nk_fill_circle(d->cv, nk_rect(mx - r, y + h / 2 - r, 2 * r, 2 * r), wcol(d, dg->common[1]));
    out_wire(d, x1, y, dir, g->out);
    out_mark(d, x1, y, g->out);
}

static void draw_switch(dc_t *d, const gate_t *g) {
    int a = g->in[0], b = g->in[1];
    int dir = -d->pos[a].side;
    float u = d->u, ex = d->pos[a].x, ya = d->pos[a].y, yb = d->pos[b].y;
    float xs = ex + dir * u * 0.45f, r = u * 0.06f;
    line(d, ex, ya, xs, ya, wcol(d, a));
    line(d, ex, yb, xs, yb, wcol(d, b));
    nk_stroke_circle(d->cv, nk_rect(xs - r, ya - r, 2 * r, 2 * r), 1, d->ink);
    nk_stroke_circle(d->cv, nk_rect(xs - r, yb - r, 2 * r, 2 * r), 1, d->ink);
    /* The contact: closed while the control pin is high. */
    int st = wstate(d, g->ctl);
    float y0 = ya < yb ? ya : yb, y1 = ya < yb ? yb : ya;
    if (st == NET_HIGH)
        line(d, xs, y0, xs, y1, d->ink);
    else
        line(d, xs, y0, xs + dir * u * 0.3f, y1 - (y1 - y0) * 0.2f, st == NET_PULSING ? COL_PULSE : d->ink);
    /* The control: a mark with the pin number. */
    float cx = xs + dir * u * 0.55f, cy = (y0 + y1) / 2, s = u * 0.12f;
    nk_fill_rect(d->cv, nk_rect(cx - s, cy - s, 2 * s, 2 * s), 0, state_color(st));
    if (u >= 26) {
        char num[8];
        snprintf(num, sizeof(num), "%d", g->ctl);
        text_at(d->cv, d->ctx, cx + dir * (s + 2), cy, dir < 0, num, COL_DIMTEXT);
    }
}

static void draw_box(dc_t *d, const gate_t *g, struct nk_rect body) {
    float u = d->u, top = 1e9f, bot = -1e9f;
    bool left = false, right = false;
    for (int k = 0; k < 14 && g->pins[k]; k++) {
        const pinpos_t *p = &d->pos[g->pins[k]];
        if (p->y < top) top = p->y;
        if (p->y > bot) bot = p->y;
        if (p->side < 0) left = true; else right = true;
    }
    float m = u * 0.28f, mid = body.x + body.w / 2;
    float x0 = left ? body.x + m : mid + u * 0.1f, x1 = right ? body.x + body.w - m : mid - u * 0.1f;
    struct nk_rect r = nk_rect(x0, top - d->pitch * 0.4f, x1 - x0, bot - top + d->pitch * 0.8f);
    nk_stroke_rect(d->cv, r, 2, d->t, d->ink);
    /* Pins: a stub into the box, and the pin function inside if it fits
     * (in the half of the box on its side when the box spans both). */
    float fh = d->ctx->style.font->height, names_w = 0;
    bool both = left && right;
    float room = both ? r.w / 2 - 6 : r.w - 6;
    bool names = d->pitch >= fh + 2;
    for (int k = 0; k < 14 && g->pins[k] && names; k++) {
        int pin = pidx(d, g->pins[k]);
        const char *name = pin >= 0 ? sim_pin_name(&d->b->sim, pin) : NULL;
        if (name && text_width(d->ctx, name) > names_w) names_w = text_width(d->ctx, name);
    }
    if (names_w > room) names = false;
    for (int k = 0; k < 14 && g->pins[k]; k++) {
        int num = g->pins[k];
        const pinpos_t *p = &d->pos[num];
        int pin = pidx(d, num);
        if (pin < 0) continue;
        float bx = p->side < 0 ? r.x : r.x + r.w;
        line(d, p->x, p->y, bx, p->y, wcol(d, num));
        const char *name = sim_pin_name(&d->b->sim, pin);
        if (names && name) text_at(d->cv, d->ctx, bx - p->side * 3, p->y, p->side > 0, name, COL_DIMTEXT);
    }
    /* The title where the names leave room: the middle of a box that spans
     * the body, the free side of a box on one side. */
    if (!g->label) return;
    float free_w = names ? (both ? r.w - 2 * (names_w + 6) : r.w - names_w - 8) : r.w - 4;
    float fx = names && !both ? (left ? r.x + names_w + 6 : r.x + 2) : r.x + (r.w - free_w) / 2;
    if (text_width(d->ctx, g->label) < free_w - 4)
        text_centered(d->cv, d->ctx, nk_rect(fx, r.y + r.h / 2 - fh / 2, free_w, fh), g->label, COL_TEXT);
    else if (!names && text_width(d->ctx, g->label) < r.w - 4)
        text_centered(d->cv, d->ctx, nk_rect(r.x, r.y + r.h / 2 - fh / 2, r.w, fh), g->label, COL_TEXT);
}

/* A pin that belongs to no gate (power, shared controls): a short stub
 * with its name. */
static void draw_stub(dc_t *d, int num) {
    int pin = pidx(d, num);
    if (pin < 0 || !d->pos[num].valid) return;
    const pinpos_t *p = &d->pos[num];
    float len = d->u * 0.25f, x1 = p->x - p->side * len;
    struct nk_color c = (d->b->sim.pins[pin].flags & PIN_POWER) ? COL_DIMTEXT : wcol(d, num);
    line(d, p->x, p->y, x1, p->y, c);
    const char *name = sim_pin_name(&d->b->sim, pin);
    if (name && d->pitch >= d->ctx->style.font->height + 2)
        text_at(d->cv, d->ctx, x1 - p->side * 2, p->y, p->side > 0, name, c);
}

bool chip_diagram(struct nk_command_buffer *cv, struct nk_context *ctx, board_t *b,
                  const uint8_t *net_state, int part, struct nk_rect body,
                  const pinpos_t *pos, float pitch) {
    const diagram_t *dg = find_diagram(b, part);
    if (!dg) return false;
    /* The gates are sized on the pin distance, but must fit in half the
     * body (gates on the left and on the right). */
    float u = fminf(pitch, body.w / 2 / 1.3f);
    if (u < 14) return false;
    const part_t *p = &b->sim.parts[part];
    dc_t d = { cv, ctx, b, net_state, part, pos, u, pitch, fmaxf(1.0f, u / 24),
               p->removed ? COL_SOCKET : nk_rgb(215, 215, 205) };
    bool used[48] = {0};
    for (int i = 0; i < dg->n; i++) {
        const gate_t *g = &dg->g[i];
        /* Skip a gate whose pins aren't all placed (shouldn't happen). */
        int nums[18] = { g->in[0], g->in[1], g->out, g->ctl }, n = 4;
        for (int k = 0; k < 14 && g->pins[k]; k++) nums[n++] = g->pins[k];
        bool ok = true;
        for (int k = 0; k < n; k++) {
            if (!nums[k]) continue;
            used[nums[k]] = true;
            if (!pos[nums[k]].valid) ok = false;
        }
        if (!ok) continue;
        switch (g->kind) {
        case G_INV: draw_inv(&d, dg, g); break;
        case G_AND: draw_and(&d, g); break;
        case G_MUX: draw_mux(&d, dg, g); break;
        case G_LATCH: draw_latch(&d, dg, g); break;
        case G_SWITCH: draw_switch(&d, g); break;
        case G_BOX: draw_box(&d, g, body); break;
        }
    }
    for (int num = 1; num <= p->npins && num < 48; num++)
        if (!used[num]) draw_stub(&d, num);
    return true;
}

/* ------------------------------------------------------------------------ */
/* The inspector window                                                      */
/* ------------------------------------------------------------------------ */

static struct {
    int part;               /* the chip, -1 if closed */
    bool reopen;            /* place and size the window on the next frame */
    int hover_pin, press_pin;
    float act[64];          /* PLA terms: recent activity (decays slowly) */
    struct nk_rect bounds;
} I = { .part = -1, .hover_pin = -1, .press_pin = -1 };

void inspect_open(board_t *b, int part) {
    (void)b;
    I.part = part;
    I.reopen = part >= 0;
    I.hover_pin = I.press_pin = -1;
    memset(I.act, 0, sizeof(I.act));
}

int inspect_current(void) { return I.part; }

bool inspect_bounds(struct nk_rect *r) {
    if (I.part < 0) return false;
    *r = I.bounds;
    return true;
}

/* The name shown outside a pin: its net, or for the nets KiCad left
 * unnamed ("Net-(U20-Pad5)") the other chip pin on it. */
static void net_label(board_t *b, int pin, char *buf, size_t len) {
    const sim_t *s = &b->sim;
    int net = s->pins[pin].net;
    if (net < 0) { snprintf(buf, len, "n/c"); return; }
    const char *name = s->nets[net].name;
    if (s->nets[s->nets[net].root].rail == RAIL_GND) { snprintf(buf, len, "GND"); return; }
    if (strncmp(name, "Net-(", 5)) { snprintf(buf, len, "%s", name); return; }
    /* The other end: a chip pin, else a connector pin, else any part. */
    const net_t *n = &s->nets[s->nets[net].root];
    static const int prefer[] = { PART_IC, PART_CONNECTOR, PART_RESISTOR, PART_OTHER, PART_LINK };
    for (int pass = 0; pass < 5; pass++)
        for (int k = 0; k < n->npins; k++) {
            int o = n->pins[k];
            const part_t *op = &s->parts[s->pins[o].part];
            if (o == pin || op->kind != prefer[pass]) continue;
            if (op->kind == PART_IC || op->kind == PART_CONNECTOR)
                snprintf(buf, len, "to %s.%s", op->ref, b->sim.bd->pads[o].name);
            else
                snprintf(buf, len, "to %s", op->ref);
            return;
        }
    snprintf(buf, len, "unconnected");
}

/* Cut a string until it fits in w pixels. */
static void fit_text(struct nk_context *ctx, char *txt, float w) {
    size_t n = strlen(txt);
    while (n > 2 && text_width(ctx, txt) > w) {
        txt[--n] = 0;
        txt[n - 1] = '.';
    }
}

/* Layout of the pinout, computed for the chip being inspected. */
typedef struct {
    int rows;
    float pitch, sq, label_w, body_w, w, h;
    bool diagram;
} layout_t;

static layout_t pinout_layout(struct nk_context *ctx, board_t *b, int part) {
    const part_t *p = &b->sim.parts[part];
    layout_t L = { .rows = (p->npins + 1) / 2, .sq = 18, .label_w = 112 };
    L.diagram = chip_has_diagram(b, part);
    L.pitch = L.diagram ? 40 : L.rows > 14 ? 22 : 26;
    if (L.diagram) {
        L.body_w = L.pitch * 3.6f;
    } else {
        float name_w = 0;
        for (int k = 0; k < p->npins; k++) {
            const char *name = sim_pin_name(&b->sim, p->first_pin + k);
            if (name && text_width(ctx, name) > name_w) name_w = text_width(ctx, name);
        }
        L.body_w = 2 * (name_w + 8) + 64;
    }
    L.w = 2 * (L.label_w + 6 + L.sq) + L.body_w;
    L.h = L.rows * L.pitch + L.pitch;
    return L;
}

/* Draw the chip with its pins in 'r'; remember where each pin is for the
 * mouse. */
static void draw_pinout(struct nk_context *ctx, struct nk_command_buffer *cv, board_t *b,
                        const uint8_t *ns, int part, const layout_t *L, struct nk_rect r,
                        struct nk_rect *hit) {
    const sim_t *s = &b->sim;
    const part_t *p = &s->parts[part];
    struct nk_rect body = nk_rect(r.x + L->label_w + 6 + L->sq, r.y, L->body_w, L->h);
    int role = board_part_role(b, part);
    if (p->removed) {
        nk_stroke_rect(cv, body, 4, 2, COL_SOCKET);
    } else {
        nk_fill_rect(cv, body, 4, COL_CHIP);
        nk_stroke_rect(cv, body, 4, 2, role_color[role]);
    }
    /* The notch at the top, pin 1 to its left. */
    float npts[2 * 17];
    int nn = arc_points(npts, body.x + body.w / 2, body.y, 8, 0, NK_PI, 16);
    nk_stroke_polyline(cv, npts, nn, 1.5f, p->removed ? COL_SOCKET : COL_CHIP_EDGE);
    pinpos_t pos[48] = {0};
    float fh = ctx->style.font->height;
    for (int num = 1; num <= p->npins && num < 48; num++) {
        int pin = part_pin(p, num);
        if (pin < 0) continue;
        bool left = num <= L->rows;
        int row = left ? num - 1 : p->npins - num;
        float y = body.y + L->pitch * (row + 1);
        float ex = left ? body.x : body.x + body.w;
        pos[num] = (pinpos_t){ ex, y, left ? -1 : 1, true };
        /* The pin, colored by its net, with its number inside. */
        struct nk_rect sq = nk_rect(left ? ex - L->sq - 2 : ex + 2, y - L->sq / 2, L->sq, L->sq);
        const pin_t *pp = &s->pins[pin];
        struct nk_color c = pp->net < 0 ? COL_NONE : state_color(ns[pp->net]);
        nk_fill_rect(cv, sq, 0, c);
        char numtxt[8];
        snprintf(numtxt, sizeof(numtxt), "%d", num);
        text_centered(cv, ctx, sq, numtxt, nk_rgb(0, 0, 0));
        if (pp->fault != PF_OK) {
            nk_stroke_rect(cv, nk_rect(sq.x - 2, sq.y - 2, sq.w + 4, sq.h + 4), 0, 2, COL_FAULT);
            if (pp->fault == PF_OPEN) {
                nk_stroke_line(cv, sq.x, sq.y, sq.x + sq.w, sq.y + sq.h, 1.5f, COL_FAULT);
                nk_stroke_line(cv, sq.x + sq.w, sq.y, sq.x, sq.y + sq.h, 1.5f, COL_FAULT);
            }
        }
        if (pin == b->scope_pin) nk_stroke_rect(cv, nk_rect(sq.x - 4, sq.y - 4, sq.w + 8, sq.h + 8), 0, 2, COL_TRACE);
        if (pin == I.hover_pin) nk_stroke_rect(cv, nk_rect(sq.x - 1, sq.y - 1, sq.w + 2, sq.h + 2), 0, 1.5f, COL_HOVER);
        /* The net outside. */
        char lab[96];
        net_label(b, pin, lab, sizeof(lab));
        fit_text(ctx, lab, L->label_w);
        text_at(cv, ctx, left ? sq.x - 6 : sq.x + sq.w + 6, y, left, lab,
                pin == I.hover_pin ? COL_HOVER : COL_DIMTEXT);
        /* What the mouse can point at: the square and its label. */
        hit[num] = left ? nk_rect(r.x, y - L->pitch / 2, sq.x + sq.w - r.x, L->pitch)
                        : nk_rect(sq.x, y - L->pitch / 2, r.x + r.w - sq.x, L->pitch);
    }
    if (L->diagram && !p->removed) {
        chip_diagram(cv, ctx, b, ns, part, body, pos, L->pitch);
    } else {
        /* Pin names inside, the chip name in the middle. */
        for (int num = 1; num <= p->npins && num < 48; num++) {
            int pin = part_pin(p, num);
            const char *name = pin >= 0 ? sim_pin_name(s, pin) : NULL;
            if (!name || !pos[num].valid) continue;
            text_at(cv, ctx, pos[num].x - pos[num].side * 6, pos[num].y, pos[num].side > 0, name, COL_DIMTEXT);
        }
        const char *label = board_part_label(b, part);
        float cy = body.y + body.h / 2;
        text_centered(cv, ctx, nk_rect(body.x, cy - fh * 1.5f, body.w, fh), label,
                      p->removed ? COL_SOCKET : role_color[role]);
        text_centered(cv, ctx, nk_rect(body.x, cy - fh * 0.5f, body.w, fh), p->ref, COL_DIMTEXT);
        text_centered(cv, ctx, nk_rect(body.x, cy + fh * 0.5f, body.w, fh),
                      p->removed ? "pulled out" : p->name, COL_DIMTEXT);
    }
}

/* ------------------------------------------------------------------------ */
/* The PLA equations                                                         */
/* ------------------------------------------------------------------------ */

#define PLA_ROW_H  17
#define PLA_TERM_X 74       /* where the literals of a term start */

/* The inputs in the order the literals are shown: address lines, the
 * memory configuration, then the bus cycle. */
static const int pla_show_order[PLA_NIN] = {
    PI_A15, PI_A14, PI_A13, PI_A12, PI_VA14, PI_VA13, PI_VA12,
    PI_LORAM, PI_HIRAM, PI_CHAREN, PI_GAME, PI_EXROM, PI_AEC, PI_RW, PI_BA, PI_CAS };

static void term_literal(board_t *b, int t, int k, char *buf, size_t len) {
    const part_t *pla = &b->sim.parts[b->p_pla];
    snprintf(buf, len, "%s=%d", sim_pin_name(&b->sim, part_pin(pla, pla_in_pin[k])), (pla_terms[t].value >> k) & 1);
}

/* The widest term, to size the pane. */
static float pla_pane_width(struct nk_context *ctx, board_t *b) {
    float maxw = 0;
    for (int t = 0; t < pla_nterms; t++) {
        float w = 0;
        for (int k = 0; k < PLA_NIN; k++) {
            if (!((pla_terms[t].mask >> k) & 1)) continue;
            char lit[24];
            term_literal(b, t, k, lit, sizeof(lit));
            w += text_width(ctx, lit) + 7;
        }
        if (w > maxw) maxw = w;
    }
    return fmaxf(PLA_TERM_X + maxw + 8, text_width(ctx, "Product terms: lit while true. Inputs are pin levels, colored like the pins.") + 8);
}

static const struct { int out; const char *what; } pla_outputs[] = {
    { PO_BASIC, "BASIC ROM" }, { PO_KERNAL, "KERNAL ROM" }, { PO_CHAROM, "character ROM" },
    { PO_IO, "I/O chips at $D000-$DFFF" }, { PO_GRW, "color RAM write" },
    { PO_ROML, "cartridge ROM low" }, { PO_ROMH, "cartridge ROM high" },
    { PO_CASRAM, "RAM: high = NOT selected" },
};

static float pla_pane_height(void) {
    return (3 + PLA_NOUT + pla_nterms) * PLA_ROW_H + PLA_NOUT * 4 + 8;
}

/* A term belongs to the first output it feeds besides /CASRAM, or to
 * /CASRAM if it feeds only that one. */
static int term_group(int t) {
    unsigned o = pla_terms[t].outputs & ~(1u << PO_CASRAM);
    if (!o) return PO_CASRAM;
    return __builtin_ctz(o);
}

static void draw_pla(struct nk_context *ctx, struct nk_command_buffer *cv, board_t *b,
                     const uint8_t *ns, struct nk_rect r) {
    const sim_t *s = &b->sim;
    const part_t *pla = &s->parts[b->p_pla];
    float x = r.x, y = r.y + PLA_ROW_H / 2;
    /* Recent activity of every term (shown a little longer than a frame,
     * so terms true only now and then don't flicker). */
    for (int t = 0; t < pla_nterms; t++) {
        float frac = b->pla_samples ? (float)b->pla_hits[t] / b->pla_samples : 0;
        I.act[t] = fmaxf(I.act[t] * 0.92f, frac);
    }
    text_at(cv, ctx, x, y, 0, "Product terms: lit while true. Inputs are pin levels, colored like the pins.",
            COL_TEXT);
    y += PLA_ROW_H;
    text_at(cv, ctx, x, y, 0, "Every term but the GR/W one also holds /CASRAM high (RAM not selected).",
            COL_DIMTEXT);
    y += PLA_ROW_H * 1.5f;
    for (size_t o = 0; o < sizeof(pla_outputs) / sizeof(pla_outputs[0]); o++) {
        int out = pla_outputs[o].out, opin = part_pin(pla, pla_out_pin[out]);
        char head[128];
        snprintf(head, sizeof(head), "pin %d %s: %s", pla_out_pin[out], sim_pin_name(s, opin),
                 pla_outputs[o].what);
        nk_fill_rect(cv, nk_rect(x, y - 6, 12, 12), 0, state_color(wire_state(b, ns, opin)));
        text_at(cv, ctx, x + 18, y, 0, head, COL_TEXT);
        y += PLA_ROW_H;
        for (int t = 0; t < pla_nterms; t++) {
            if (term_group(t) != out) continue;
            float a = I.act[t];
            struct nk_rect box = nk_rect(x + 18, y - 5, 10, 10);
            if (a > 0) {
                float k = fminf(1.0f, 0.35f + a * 3);
                nk_fill_rect(cv, box, 0, nk_rgb((int)(90 * k), (int)(255 * k), (int)(120 * k)));
            }
            nk_stroke_rect(cv, box, 0, 1, COL_DIMTEXT);
            char pct[16] = "";
            if (b->pla_samples && b->pla_hits[t])
                snprintf(pct, sizeof(pct), "%.0f%%", ceilf(100.0f * b->pla_hits[t] / b->pla_samples));
            text_at(cv, ctx, x + 66, y, 1, pct, COL_DIMTEXT);
            float lx = x + PLA_TERM_X;
            for (int j = 0; j < PLA_NIN; j++) {
                int k = pla_show_order[j];
                if (!((pla_terms[t].mask >> k) & 1)) continue;
                int ipin = part_pin(pla, pla_in_pin[k]);
                char lit[24];
                term_literal(b, t, k, lit, sizeof(lit));
                struct nk_color c = state_color(wire_state(b, ns, ipin));
                text_at(cv, ctx, lx, y, 0, lit, c);
                lx += text_width(ctx, lit) + 7;
            }
            y += PLA_ROW_H;
        }
        y += 4;
    }
}

/* ------------------------------------------------------------------------ */
/* Details of the pin under the mouse                                        */
/* ------------------------------------------------------------------------ */

static const char *pin_kind(board_t *b, int pin) {
    const pin_t *p = &b->sim.pins[pin];
    const part_t *part = &b->sim.parts[p->part];
    if (p->flags & PIN_POWER) return "power";
    if (p->flags & PIN_SWITCH) return "switch terminal";
    if ((p->flags & PIN_IN) && (p->flags & PIN_OUT)) return "bidirectional";
    if (p->flags & PIN_OUT) return part->tech == TECH_TTL_OC ? "open collector output" : "output";
    if (p->flags & PIN_IN) return (p->flags & PIN_TTL) ? "TTL input" : "MOS input";
    return "pin";
}

/* Is this pin of a resistor a pull-up? */
static bool is_pullup(board_t *b, int pin) {
    const part_t *p = &b->sim.parts[b->sim.pins[pin].part];
    if (p->kind != PART_RESISTOR || !p->state) return false;
    const resistor_state_t *st = p->state;
    for (int i = 0; i < st->nel; i++)
        if (st->el[i].type == EL_PULLUP && (st->el[i].pin_a == pin || st->el[i].pin_b == pin)) return true;
    return false;
}

static void pin_details(board_t *b, const uint8_t *ns, int pin, char *l1, char *l2, char *l3, size_t len) {
    const sim_t *s = &b->sim;
    const pin_t *p = &s->pins[pin];
    const part_t *part = &s->parts[p->part];
    const char *name = sim_pin_name(s, pin);
    snprintf(l1, len, "pin %d %s, %s.  Net %s: %s%s%s", p->number, name ? name : "", pin_kind(b, pin),
             pin_net_name(b, pin), p->net >= 0 ? state_name(ns[p->net]) : "-",
             p->fault != PF_OK ? ".  Fault: " : "", p->fault != PF_OK ? pin_fault_name(p->fault) : "");
    /* The chip side. */
    if (part->removed) {
        snprintf(l2, len, "The chip is pulled out of its socket.");
    } else if (part->dead) {
        snprintf(l2, len, "The chip has no power: it does nothing.");
    } else if (p->flags & PIN_POWER) {
        snprintf(l2, len, "Power pin.");
    } else if (p->flags & PIN_OUT) {
        uint32_t hi = b->pst_hi[pin], lo = b->pst_lo[pin], off = b->pst_off[pin], tot = hi + lo + off;
        if (!tot) tot = 1;
        if (part->tech == TECH_TTL_OC)
            snprintf(l2, len, "The output transistor: on (pulling low) %u%%, off %u%%.",
                     100 * lo / tot, 100 * off / tot);
        else
            snprintf(l2, len, "The chip drives: high %u%%%s, low %u%%, nothing %u%%.", 100 * hi / tot,
                     b->pst_weak[pin] ? " (weakly)" : "", 100 * lo / tot, 100 * off / tot);
        if (p->fault == PF_OPEN) strncat(l2, " The leg is lifted: the net doesn't see it.", len - strlen(l2) - 1);
    } else if (p->fault == PF_OPEN) {
        snprintf(l2, len, (p->flags & PIN_TTL) ? "The leg is lifted: this TTL input reads 1."
                                               : "The leg is lifted: this MOS input floats and reads noise.");
    } else {
        snprintf(l2, len, "The chip reads it: %s.", p->net >= 0 ? state_name(ns[p->net]) : "-");
    }
    /* Who else is on the net. */
    snprintf(l3, len, "On this net:");
    if (p->root < 0) { strncat(l3, " nothing.", len - strlen(l3) - 1); return; }
    const net_t *n = &s->nets[p->root];
    int shown = 0;
    for (int k = 0; k < n->npins; k++) {
        int o = n->pins[k];
        if (o == pin) continue;
        const part_t *op = &s->parts[s->pins[o].part];
        char item[80];
        if (op->kind == PART_IC) {
            const char *on = sim_pin_name(s, o);
            snprintf(item, sizeof(item), " %s.%s %s (%s)", op->ref, b->sim.bd->pads[o].name, on ? on : "",
                     board_part_label(b, s->pins[o].part));
        } else if (op->kind == PART_CONNECTOR) {
            const char *f = board_pin_func(b, o);
            snprintf(item, sizeof(item), " %s.%s %s", op->ref, b->sim.bd->pads[o].name, f ? f : "");
        } else if (op->kind == PART_RESISTOR) {
            snprintf(item, sizeof(item), " %s %s%s", op->ref, op->name, is_pullup(b, o) ? " pull-up" : "");
        } else {
            continue;   /* ferrite beads, capacitors */
        }
        if (strlen(l3) + strlen(item) + 6 >= len) { strncat(l3, " ...", len - strlen(l3) - 1); break; }
        strncat(l3, shown ? "," : "", len - strlen(l3) - 1);
        strncat(l3, item, len - strlen(l3) - 1);
        shown++;
    }
    if (!shown) strncat(l3, " nothing else.", len - strlen(l3) - 1);
}

/* ------------------------------------------------------------------------ */
/* The window                                                                */
/* ------------------------------------------------------------------------ */

int inspect_window(struct nk_context *ctx, board_t *b, const uint8_t *ns, struct nk_rect area) {
    if (I.part < 0) return -1;
    const part_t *p = &b->sim.parts[I.part];
    layout_t L = pinout_layout(ctx, b, I.part);
    bool pla = I.part == b->p_pla;
    float pane_w = pla ? pla_pane_width(ctx, b) : 0;
    float cw = L.w + (pla ? pane_w + 16 : 0), ch = fmaxf(L.h, pla ? pla_pane_height() : 0);
    /* Title, a line of help, the drawing, three lines of details (the last
     * one wraps on two rows), the window padding. */
    char title[160];
    snprintf(title, sizeof(title), "%s %s: %s", p->ref, board_part_label(b, I.part), board_part_detail(b, I.part));
    float ww = fmaxf(fmaxf(cw, 540), text_width(ctx, title) + 40) + 20, wh = 30 + 24 + ch + 4 + 24 * 2 + 42 + 16;
    nk_flags scroll = NK_WINDOW_NO_SCROLLBAR;
    if (wh > area.h - 16) { wh = area.h - 16; scroll = 0; }
    if (I.reopen) {
        nk_window_show(ctx, "Inspector", NK_SHOWN);
        nk_window_set_bounds(ctx, "Inspector", nk_rect(area.x + 16, area.y + 16, ww, wh));
        nk_window_set_focus(ctx, "Inspector");
        I.reopen = false;
    }
    int clicked = -1;
    if (nk_begin_titled(ctx, "Inspector", title, nk_rect(area.x + 16, area.y + 16, ww, wh),
                        NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | scroll)) {
        I.bounds = nk_window_get_bounds(ctx);
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        struct nk_input *in = &ctx->input;
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, "Pins colored like on the board. Click a pin: scope and faults.",
                         NK_TEXT_LEFT, COL_DIMTEXT);
        nk_layout_row_static(ctx, ch, (int)cw, 1);
        struct nk_rect r;
        struct nk_rect hit[48];
        memset(hit, 0, sizeof(hit));
        if (nk_widget(&r, ctx) != NK_WIDGET_INVALID) {
            draw_pinout(ctx, cv, b, ns, I.part, &L, nk_rect(r.x, r.y, L.w, L.h), hit);
            if (pla) draw_pla(ctx, cv, b, ns, nk_rect(r.x + L.w + 16, r.y, pane_w, ch));
        }
        /* The pin under the mouse; a click selects it. Only if this is the
         * window on top: nuklear makes the others read only (ROM), and a
         * click on the menu above must not reach the pins below it. */
        bool on_top = !(ctx->current->layout->flags & NK_WINDOW_ROM);
        I.hover_pin = -1;
        for (int num = 1; num <= p->npins && num < 48 && on_top; num++)
            if (hit[num].w > 0 && nk_input_is_mouse_hovering_rect(in, hit[num]))
                I.hover_pin = part_pin(p, num);
        if (!on_top) I.press_pin = -1;
        if (on_top && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) I.press_pin = I.hover_pin;
        if (on_top && nk_input_is_mouse_released(in, NK_BUTTON_LEFT)) {
            if (I.press_pin >= 0 && I.press_pin == I.hover_pin) clicked = I.hover_pin;
            I.press_pin = -1;
        }
        char l1[256], l2[256], l3[512];
        if (I.hover_pin >= 0) {
            pin_details(b, ns, I.hover_pin, l1, l2, l3, sizeof(l1));
        } else {
            snprintf(l1, sizeof(l1), "Point at a pin for the details.");
            l2[0] = l3[0] = 0;
        }
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, l1, NK_TEXT_LEFT, COL_HOVER);
        nk_label(ctx, l2, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 38, 1);
        nk_label_colored_wrap(ctx, l3, COL_DIMTEXT);
    } else {
        I.part = -1;        /* closed */
    }
    nk_end(ctx);
    return clicked;
}
