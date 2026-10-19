/* ui.c - the graphical interface.
 *
 * Left: the motherboard, drawn from the KiCad geometry. Chips are dark
 * boxes with their name, pins are small squares colored like a logic probe
 * would show them (see the legend). Mouse wheel zooms, dragging pans; the
 * more you zoom, the more the chips show: pin numbers, then the gates of
 * the logic chips (drawn by inspect.c) or the pin names. Clicking a pin
 * selects it for the oscilloscope and opens a menu to break it; clicking a
 * chip lets you pull it out of its socket or open the inspector; clicking
 * a resistor or a ferrite bead lets you open or short it.
 *
 * Right: what the monitor shows, the oscilloscope and the controls. */
#include "ui_common.h"
#include "inspect.h"
#include "png.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define GL_SILENCE_DEPRECATION
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#define RIGHT_W   470           /* width of the right column */
#define MON_W     448           /* monitor picture size on screen */
#define MON_H     311
#define SCOPE_H   250

typedef struct { float x0, y0, x1, y1; } box_t;

static struct {
    board_t *b;
    /* Board view: screen = origin + mm * zoom (origin relative to the view). */
    float zoom, ox, oy;
    bool fitted;
    box_t box[SIM_MAX_PARTS];   /* body of each part, in mm */
    int hover_pin, hover_part;
    bool dragging, drag_moved;
    float drag_x, drag_y;
    /* Fault menu (a small window at the mouse, in screen coordinates). */
    bool menu_open;
    int menu_pin, menu_part, menu_frame;
    float menu_x, menu_y;
    /* The RESET button kept pressed (the checkbox next to it). */
    bool reset_hold;
    /* Requested board view (--view): center in mm and zoom over the fit. */
    bool view_set;
    float view_cx, view_cy, view_zoom;
    /* Logic probe state of every net, refreshed every frame. */
    uint8_t *net_state;
    /* Monitor. */
    sg_image img;
    sg_view view;
    sg_sampler smp;
    snk_image_t snk_img;
    uint32_t monitor[SCREEN_W * SCREEN_H];
    /* Emulation. */
    bool paused, warp;
    double last_time, speed;
    uint64_t frame;
    const char *autoload;
    /* Scope. */
    int timebase;
    bool trigger;
    /* PRG loading. */
    char path[1024];
    char status[512];
    char dir[1024];
    char entries[256][256];
    bool entry_is_dir[256];
    int nentries;
} U;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static sim_t *S(void) { return &U.b->sim; }

static bool pin_is_interactive(int pin) {
    const part_t *p = &S()->parts[S()->pins[pin].part];
    return p->kind == PART_IC || p->kind == PART_CONNECTOR;
}

/* ------------------------------------------------------------------------ */
/* Board geometry                                                            */
/* ------------------------------------------------------------------------ */

/* The body of a part, from its pads: DIP chips get a body between the two
 * pin rows, everything else a box around the pads. */
static void compute_boxes(void) {
    sim_t *s = S();
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, pad = 0;
        for (int k = 0; k < p->npins; k++) {
            const bd_pad_t *bp = &bd_pads[p->first_pin + k];
            if (bp->x < x0) x0 = bp->x;
            if (bp->y < y0) y0 = bp->y;
            if (bp->x > x1) x1 = bp->x;
            if (bp->y > y1) y1 = bp->y;
            if (bp->w > pad) pad = bp->w;
            if (bp->h > pad) pad = bp->h;
        }
        if (p->kind == PART_IC) {
            if (x1 - x0 >= y1 - y0) U.box[i] = (box_t){ x0 - 1.4f, y0 + 0.9f, x1 + 1.4f, y1 - 0.9f };
            else U.box[i] = (box_t){ x0 + 0.9f, y0 - 1.4f, x1 - 0.9f, y1 + 1.4f };
        } else {
            float m = pad / 2 + 0.4f;
            U.box[i] = (box_t){ x0 - m, y0 - m, x1 + m, y1 + m };
        }
    }
}

static struct nk_rect to_screen(struct nk_rect area, float x0, float y0, float x1, float y1) {
    return nk_rect(area.x + U.ox + x0 * U.zoom, area.y + U.oy + y0 * U.zoom,
                   (x1 - x0) * U.zoom, (y1 - y0) * U.zoom);
}

static void fit_view(struct nk_rect area) {
    float bw = bd_outline[2] - bd_outline[0], bh = bd_outline[3] - bd_outline[1];
    U.zoom = fminf(area.w / bw, area.h / bh) * 0.98f;
    U.ox = (area.w - bw * U.zoom) / 2 - bd_outline[0] * U.zoom;
    U.oy = (area.h - bh * U.zoom) / 2 - bd_outline[1] * U.zoom;
    U.fitted = true;
}

/* Pin under the mouse (only chips and connectors), or -1. */
static int pick_pin(float mx, float my) {
    sim_t *s = S();
    for (int i = 0; i < s->npins; i++) {
        if (!pin_is_interactive(i)) continue;
        const bd_pad_t *bp = &bd_pads[i];
        float hw = fmaxf(bp->w / 2, 0.7f), hh = fmaxf(bp->h / 2, 0.7f);
        if (fabsf(mx - bp->x) <= hw && fabsf(my - bp->y) <= hh) return i;
    }
    return -1;
}

/* Part under the mouse: chips first, then resistors and the rest. */
static int pick_part(float mx, float my) {
    sim_t *s = S();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s->nparts; i++) {
            if ((s->parts[i].kind == PART_IC) != (pass == 0)) continue;
            if (!board_part_simulated(U.b, i)) continue;
            box_t *b = &U.box[i];
            if (mx >= b->x0 && mx <= b->x1 && my >= b->y0 && my <= b->y1) return i;
        }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Board view                                                                */
/* ------------------------------------------------------------------------ */

/* The name of a chip ("RAM1", "PLA", "VRAM"...), colored by role. The
 * chips stand vertical and at normal zoom they are too narrow for a word,
 * so the letters are stacked. When zooming in, the name goes horizontal and
 * the reference, the model and the address range appear under it, as many
 * lines as fit. */
static void draw_chip_name(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect r, int i) {
    part_t *p = &S()->parts[i];
    const char *label = board_part_label(U.b, i);
    struct nk_color tc = p->removed ? COL_SOCKET : role_color[board_part_role(U.b, i)];
    struct nk_color dim = p->removed ? COL_SOCKET : COL_DIMTEXT;
    float fh = ctx->style.font->height;
    int n = (int)strlen(label);

    if (text_width(ctx, label) < r.w - 4) {
        /* Horizontal: collect the lines that fit in the width. */
        const char *lines[8];
        char buf[8][32];
        struct nk_color col[8];
        int nl = 0;
        lines[nl] = label; col[nl++] = tc;
        if (text_width(ctx, p->ref) < r.w - 4) { lines[nl] = p->ref; col[nl++] = dim; }
        /* The info lines. One too wide is wrapped at a space or inside an
         * address range ("$0000-$FFFF" becomes "$0000-" and "$FFFF"); if a
         * piece still doesn't fit, the rest of that line is left out. */
        const char *info = p->removed ? "(removed)" : board_part_info(U.b, i);
        while (*info && nl < 8) {
            size_t len = strcspn(info, "\n");
            snprintf(buf[nl], sizeof(buf[nl]), "%.*s", (int)len, info);
            for (int k = (int)strlen(buf[nl]) - 1; k > 0 && text_width(ctx, buf[nl]) >= r.w - 4; k--) {
                if (buf[nl][k] == ' ') { buf[nl][k] = 0; len = (size_t)k + 1; }
                else if (buf[nl][k] == '$' && buf[nl][k - 1] == '-') { buf[nl][k] = 0; len = (size_t)k; }
            }
            if (text_width(ctx, buf[nl]) >= r.w - 4) len = strcspn(info, "\n");
            else { lines[nl] = buf[nl]; col[nl++] = dim; }
            info += len;
            if (*info == '\n') info++;
        }
        /* As many lines as the height allows (at least the name). */
        int fit = (int)((r.h - 4) / fh);
        if (fit < 1) fit = 1;
        if (nl > fit) nl = fit;
        float y = r.y + (r.h - nl * fh) / 2;
        for (int k = 0; k < nl; k++, y += fh)
            text_centered(cv, ctx, nk_rect(r.x, y, r.w, fh), lines[k], col[k]);
    } else if (r.h >= n * fh * 0.9f + 8) {
        /* Vertical: one letter under the other. */
        float step = fh * 0.9f, y = r.y + (r.h - n * step) / 2;
        for (int k = 0; k < n; k++, y += step) {
            char ch[2] = { label[k], 0 };
            text_centered(cv, ctx, nk_rect(r.x, y, r.w, step), ch, tc);
        }
    } else if (text_width(ctx, p->ref) < r.w - 2 && r.h >= fh) {
        text_centered(cv, ctx, r, p->ref, dim);
    }
}

/* Pad squares big enough to write the pin number inside (see draw_pins). */
static bool numbers_in_pads(struct nk_context *ctx) {
    return 1.6f * U.zoom * 0.85f >= text_width(ctx, "40") + 4;
}

/* Inside a chip, depending on the zoom: the name; then also the pin
 * numbers along the edges; zooming in more (numbers move into the pads)
 * the gates of the logic chips, or the pin names of the others, with the
 * name above the chip. */
static void draw_chip_inside(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area,
                             struct nk_rect r, int i) {
    part_t *p = &S()->parts[i];
    float fh = ctx->style.font->height, pitch = 2.54f * U.zoom;
    bool vertical = r.h > r.w;
    if (!vertical || pitch < fh) { draw_chip_name(cv, ctx, r, i); return; }
    /* Where each pin enters the body. */
    pinpos_t pos[48] = {0};
    for (int num = 1; num <= p->npins && num < 48; num++) {
        int pin = part_pin(p, num);
        if (pin < 0) continue;
        float px = area.x + U.ox + bd_pads[pin].x * U.zoom, py = area.y + U.oy + bd_pads[pin].y * U.zoom;
        int side = px < r.x + r.w / 2 ? -1 : 1;
        pos[num] = (pinpos_t){ side < 0 ? r.x : r.x + r.w, py, side, true };
    }
    char lab[48];
    snprintf(lab, sizeof(lab), "%s %s", p->ref, board_part_label(U.b, i));
    struct nk_color tc = p->removed ? COL_SOCKET : role_color[board_part_role(U.b, i)];
    if (numbers_in_pads(ctx)) {
        if (!p->removed && chip_diagram(cv, ctx, U.b, U.net_state, i, r, pos, pitch)) {
            text_centered(cv, ctx, nk_rect(r.x - 40, r.y - fh - 2, r.w + 80, fh), lab, tc);
            return;
        }
        /* Pin names along the edges, if they fit in half the body. */
        float name_w = 0;
        for (int num = 1; num <= p->npins && num < 48; num++) {
            const char *name = pos[num].valid ? sim_pin_name(S(), part_pin(p, num)) : NULL;
            if (name && text_width(ctx, name) > name_w) name_w = text_width(ctx, name);
        }
        if (name_w + 6 < r.w / 2) {
            for (int num = 1; num <= p->npins && num < 48; num++) {
                const char *name = pos[num].valid ? sim_pin_name(S(), part_pin(p, num)) : NULL;
                if (name) text_at(cv, ctx, pos[num].x - pos[num].side * 4, pos[num].y, pos[num].side > 0, name, COL_DIMTEXT);
            }
            float center = r.w - 2 * (name_w + 8);
            if (center > 12) draw_chip_name(cv, ctx, nk_rect(r.x + name_w + 8, r.y, center, r.h), i);
            else text_centered(cv, ctx, nk_rect(r.x - 40, r.y - fh - 2, r.w + 80, fh), lab, tc);
            return;
        }
        draw_chip_name(cv, ctx, r, i);
        return;
    }
    /* Pin numbers along the edges, the name in what is left in the middle. */
    float num_w = text_width(ctx, "40") + 4, center = r.w - 2 * num_w;
    if (center < 10) { draw_chip_name(cv, ctx, r, i); return; }
    for (int num = 1; num <= p->npins && num < 48; num++) {
        if (!pos[num].valid) continue;
        char txt[8];
        snprintf(txt, sizeof(txt), "%d", num);
        text_at(cv, ctx, pos[num].x - pos[num].side * 2, pos[num].y, pos[num].side > 0, txt, COL_DIMTEXT);
    }
    draw_chip_name(cv, ctx, nk_rect(r.x + num_w, r.y, center, r.h), i);
}

static void draw_part(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area, int i) {
    sim_t *s = S();
    part_t *p = &s->parts[i];
    box_t *b = &U.box[i];
    struct nk_rect r = to_screen(area, b->x0, b->y0, b->x1, b->y1);
    bool sim = board_part_simulated(U.b, i);
    switch (p->kind) {
    case PART_IC: {
        if (p->removed) {
            nk_stroke_rect(cv, r, 0, 2, COL_SOCKET);
        } else {
            /* The memories and the PLA get an edge in their role color, so
             * they stand out. */
            int role = board_part_role(U.b, i);
            bool mem = role == ROLE_RAM || role == ROLE_VRAM || role == ROLE_ROM || role == ROLE_PLA;
            nk_fill_rect(cv, r, 2, COL_CHIP);
            nk_stroke_rect(cv, r, 2, mem ? 2 : 1, mem ? role_color[role] : COL_CHIP_EDGE);
        }
        /* Pin 1 marker. */
        int p1 = part_pin(p, 1);
        if (p1 >= 0) {
            float px = area.x + U.ox + bd_pads[p1].x * U.zoom, py = area.y + U.oy + bd_pads[p1].y * U.zoom;
            float cx = fminf(fmaxf(px, r.x + 6), r.x + r.w - 6), cy = fminf(fmaxf(py, r.y + 6), r.y + r.h - 6);
            nk_fill_circle(cv, nk_rect(cx - 3, cy - 3, 6, 6), p->removed ? COL_SOCKET : COL_CHIP_EDGE);
        }
        draw_chip_inside(cv, ctx, area, r, i);
        break;
    }
    case PART_RESISTOR:
    case PART_LINK:
        if (!sim) { nk_stroke_rect(cv, r, 2, 1, COL_OTHER); break; }
        nk_fill_rect(cv, r, 2, p->kind == PART_LINK ? COL_LINK : COL_RES);
        if (p->rfault != RF_OK) {
            nk_stroke_rect(cv, r, 2, 2, COL_FAULT);
            nk_stroke_line(cv, r.x, r.y, r.x + r.w, r.y + r.h, 2, COL_FAULT);
        }
        if (r.w > text_width(ctx, p->ref) + 4 && r.h > ctx->style.font->height)
            text_centered(cv, ctx, r, p->ref, p->kind == PART_LINK ? COL_TEXT : nk_rgb(40, 30, 20));
        break;
    case PART_CONNECTOR: {
        /* A cartridge in the expansion port: a board under the pins. */
        const char *cart = i == U.b->p_cn6 ? board_cart_name(U.b) : NULL;
        /* A dongle of the 586220 harness: an outline around the connector. */
        static const char *const harness_cn[] = { "CN1", "CN2", "CN3", "CN4", "CN8", "CN9" };
        bool dongle = false;
        for (size_t k = 0; U.b->harness && k < sizeof(harness_cn) / sizeof(harness_cn[0]); k++)
            if (!strcmp(p->ref, harness_cn[k])) dongle = true;
        if (dongle) nk_stroke_rect(cv, nk_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8), 3, 2, COL_HARNESS);
        if (cart) {
            nk_fill_rect(cv, nk_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12), 3, nk_rgb(60, 50, 40));
            nk_stroke_rect(cv, nk_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12), 3, 2, COL_SOCKET);
        }
        nk_stroke_rect(cv, r, 0, 1, COL_DIMTEXT);
        /* "CN4 SERIAL" if it fits, else just "CN4". */
        char lab[96];
        const char *cname = board_connector_name(U.b, i);
        if (cart) snprintf(lab, sizeof(lab), "%s cartridge: %s", p->ref, cart);
        else snprintf(lab, sizeof(lab), "%s %s%s", p->ref, cname ? cname : "", dongle ? " + harness" : "");
        if (!cart && (!cname || text_width(ctx, lab) > r.w + 30))
            snprintf(lab, sizeof(lab), "%s%s", p->ref, dongle ? " + harness" : "");
        if (cart)       /* always shown: a cartridge is easy to forget */
            text_centered(cv, ctx, nk_rect(r.x - 100, r.y - 22, r.w + 200, 14), lab, COL_SOCKET);
        else if (dongle)
            text_centered(cv, ctx, nk_rect(r.x - 60, r.y - 18, r.w + 120, 14), lab, COL_HARNESS);
        else if (r.w + 30 > text_width(ctx, lab))
            text_centered(cv, ctx, nk_rect(r.x - 15, r.y - 14, r.w + 30, 14), lab, COL_DIMTEXT);
        break;
    }
    default:
        if (sim) {      /* the crystal */
            nk_fill_rect(cv, r, 2, nk_rgb(170, 170, 180));
            if (p->rfault != RF_OK) nk_stroke_rect(cv, r, 2, 2, COL_FAULT);
        } else {
            nk_stroke_rect(cv, r, 2, 1, COL_OTHER);
        }
        break;
    }
}

static void draw_pins(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    sim_t *s = S();
    float ctx_font_h = ctx->style.font->height;
    int hover_root = U.hover_pin >= 0 ? s->pins[U.hover_pin].root : -2;
    for (int i = 0; i < s->npins; i++) {
        const bd_pad_t *bp = &bd_pads[i];
        const pin_t *p = &s->pins[i];
        float sz = fmaxf(fminf(bp->w, bp->h) * U.zoom * 0.85f, 2.0f);
        float x = area.x + U.ox + bp->x * U.zoom, y = area.y + U.oy + bp->y * U.zoom;
        struct nk_rect r = nk_rect(x - sz / 2, y - sz / 2, sz, sz);
        if (!pin_is_interactive(i)) {
            nk_fill_rect(cv, r, sz / 2, nk_rgb(150, 140, 110));
            continue;
        }
        bool removed = s->parts[p->part].removed;
        struct nk_color c = p->net < 0 ? COL_NONE : state_color(U.net_state[p->net]);
        if (removed) c = nk_rgb(90, 80, 60);
        nk_fill_rect(cv, r, 0, c);
        if (p->fault != PF_OK) {
            nk_stroke_rect(cv, nk_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4), 0, 2, COL_FAULT);
            if (p->fault == PF_OPEN) {
                nk_stroke_line(cv, r.x, r.y, r.x + r.w, r.y + r.h, 1.5f, nk_rgb(0, 0, 0));
                nk_stroke_line(cv, r.x + r.w, r.y, r.x, r.y + r.h, 1.5f, nk_rgb(0, 0, 0));
            }
        }
        /* The pin number (or letter) inside the pad, when it fits. */
        if (!removed && sz >= ctx_font_h - 1 && sz >= text_width(ctx, bp->name) + 3)
            text_centered(cv, ctx, r, bp->name, nk_rgb(0, 0, 0));
        if (p->root == hover_root && p->root >= 0)
            nk_stroke_rect(cv, nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 0, 1.5f, COL_HOVER);
        if (i == U.b->scope_pin)
            nk_stroke_rect(cv, nk_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6), 0, 2, COL_TRACE);
    }
}

static void select_pin(int pin) {
    U.b->scope_pin = pin;
    U.b->scope_pos = 0;
    memset(U.b->scope, 0, sizeof(U.b->scope));
}

static void board_input(struct nk_context *ctx, struct nk_rect area) {
    struct nk_input *in = &ctx->input;
    float mx = (in->mouse.pos.x - area.x - U.ox) / U.zoom;
    float my = (in->mouse.pos.y - area.y - U.oy) / U.zoom;
    bool inside = nk_input_is_mouse_hovering_rect(in, area) && nk_window_is_hovered(ctx);
    struct nk_rect ir;     /* the inspector floats above the board */
    if (inspect_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    U.hover_pin = U.hover_part = -1;
    if (U.menu_open) return;
    if (inside) {
        U.hover_pin = pick_pin(mx, my);
        if (U.hover_pin < 0) U.hover_part = pick_part(mx, my);
        /* Zoom around the mouse. */
        if (in->mouse.scroll_delta.y != 0) {
            float f = powf(1.15f, in->mouse.scroll_delta.y);
            float nz = fminf(fmaxf(U.zoom * f, 1.0f), 60.0f);
            U.ox = in->mouse.pos.x - area.x - mx * nz;
            U.oy = in->mouse.pos.y - area.y - my * nz;
            U.zoom = nz;
        }
        if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT) || nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT)) {
            U.dragging = true;
            U.drag_moved = false;
            U.drag_x = in->mouse.pos.x;
            U.drag_y = in->mouse.pos.y;
        }
    }
    if (U.dragging) {
        if (fabsf(in->mouse.pos.x - U.drag_x) + fabsf(in->mouse.pos.y - U.drag_y) > 4) U.drag_moved = true;
        if (U.drag_moved) { U.ox += in->mouse.delta.x; U.oy += in->mouse.delta.y; }
        bool released = nk_input_is_mouse_released(in, NK_BUTTON_LEFT) || nk_input_is_mouse_released(in, NK_BUTTON_RIGHT);
        if (released) {
            U.dragging = false;
            /* A click (not a drag): select the pin and open the fault menu. */
            if (!U.drag_moved && inside && (U.hover_pin >= 0 || U.hover_part >= 0)) {
                U.menu_open = true;
                U.menu_pin = U.hover_pin;
                U.menu_part = U.hover_part;
                U.menu_x = in->mouse.pos.x + 10;
                U.menu_y = in->mouse.pos.y + 10;
                U.menu_frame = (int)U.frame;
                if (U.hover_pin >= 0) select_pin(U.hover_pin);
            }
        }
    }
}

/* A radio option that applies an action when chosen. */
static bool option(struct nk_context *ctx, const char *label, bool active) {
    return nk_option_label(ctx, label, active) && !active;
}

/* The menu of a pin or a part, at the mouse. A window of its own, so it
 * opens from the board and from the inspector alike. */
static void fault_menu(struct nk_context *ctx) {
    if (!U.menu_open) return;
    sim_t *s = S();
    char title[128], info[128] = "";
    int rows;
    bool power = false, pullup = false, faultable = true;
    if (U.menu_pin >= 0) {
        const pin_t *pp = &s->pins[U.menu_pin];
        pin_desc(U.b, U.menu_pin, title, sizeof(title));
        power = (pp->flags & PIN_POWER) != 0;
        /* A connector pin on a rail or on a part of the board that isn't
         * simulated (video output, power supply): nothing to break. */
        if (s->parts[pp->part].kind == PART_CONNECTOR) {
            if (pp->net < 0 || !board_net_simulated(U.b, pp->net)) {
                faultable = false;
                snprintf(info, sizeof(info), "not simulated (analog or power part)");
            } else if (s->nets[s->nets[pp->net].root].rail) {
                faultable = false;
                snprintf(info, sizeof(info), "power rail: break the chips' power pins");
            }
        }
        rows = !faultable ? 1 : power ? 2 : 4;
    } else {
        part_t *p = &s->parts[U.menu_part];
        snprintf(title, sizeof(title), "%s %s", p->ref, board_part_label(U.b, U.menu_part));
        if (p->kind == PART_RESISTOR && p->state) {
            resistor_state_t *st = p->state;
            for (int i = 0; i < st->nel; i++) if (st->el[i].type == EL_PULLUP) pullup = true;
        }
        rows = p->kind == PART_IC ? 3 : (pullup ? 3 : 2);
    }
    float w = fmaxf(260, fmaxf(text_width(ctx, title), text_width(ctx, info)) + 30);
    float h = 46 + rows * 24 + (U.menu_pin >= 0 ? 22 : 0);
    float x = fminf(U.menu_x, sapp_widthf() - w - 4), y = fminf(U.menu_y, sapp_heightf() - h - 4);
    struct nk_rect r = nk_rect(x, y, w, h);
    nk_window_set_bounds(ctx, "Menu", r);
    if (nk_begin(ctx, "Menu", r, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, title, NK_TEXT_LEFT);
        if (U.menu_pin >= 0) {
            int pin = U.menu_pin, f = s->pins[pin].fault;
            char net[96];
            snprintf(net, sizeof(net), "net %s", pin_net_name(U.b, pin));
            nk_label_colored(ctx, net, NK_TEXT_LEFT, COL_DIMTEXT);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (!faultable) {
                nk_label_colored(ctx, info, NK_TEXT_LEFT, COL_DIMTEXT);
            } else {
                if (option(ctx, "Working", f == PF_OK)) { board_set_pin_fault(U.b, pin, PF_OK); U.menu_open = false; }
                if (option(ctx, "Floating (open, lifted leg)", f == PF_OPEN)) { board_set_pin_fault(U.b, pin, PF_OPEN); U.menu_open = false; }
                if (!power) {
                    if (option(ctx, "Shorted to GND", f == PF_GND)) { board_set_pin_fault(U.b, pin, PF_GND); U.menu_open = false; }
                    if (option(ctx, "Shorted to +5V", f == PF_VCC)) { board_set_pin_fault(U.b, pin, PF_VCC); U.menu_open = false; }
                }
            }
        } else {
            int part = U.menu_part;
            part_t *p = &s->parts[part];
            nk_layout_row_dynamic(ctx, 22, 1);
            if (p->kind == PART_IC) {
                if (option(ctx, "In its socket", !p->removed)) { board_set_removed(U.b, part, false); U.menu_open = false; }
                if (option(ctx, "Pulled out of the socket", p->removed)) { board_set_removed(U.b, part, true); U.menu_open = false; }
                if (nk_button_label(ctx, "Inspect: pins, gates, live states")) { inspect_open(U.b, part); U.menu_open = false; }
            } else {
                int f = p->rfault;
                if (option(ctx, "Working", f == RF_OK)) { board_set_rfault(U.b, part, RF_OK); U.menu_open = false; }
                if (option(ctx, p->kind == PART_OTHER ? "Broken (no oscillation)" : "Open", f == RF_OPEN)) {
                    board_set_rfault(U.b, part, RF_OPEN); U.menu_open = false;
                }
                if (pullup && option(ctx, "Shorted", f == RF_SHORT)) { board_set_rfault(U.b, part, RF_SHORT); U.menu_open = false; }
            }
        }
    }
    nk_end(ctx);
    if ((int)U.frame == U.menu_frame) nk_window_set_focus(ctx, "Menu");
    /* Clicking outside the menu closes it. */
    if ((int)U.frame > U.menu_frame + 1 && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        !nk_input_is_mouse_hovering_rect(&ctx->input, r))
        U.menu_open = false;
}

/* The tooltip next to the mouse on the board: what the pin or part is. */
static void board_tooltip(struct nk_context *ctx) {
    if (U.menu_open || U.dragging) return;
    char tip[256];
    if (U.hover_pin >= 0) {
        char pd[128];
        pin_desc(U.b, U.hover_pin, pd, sizeof(pd));
        int net = S()->pins[U.hover_pin].net;
        bool sim = board_net_simulated(U.b, net);
        snprintf(tip, sizeof(tip), "%s   net %s: %s", pd, pin_net_name(U.b, U.hover_pin),
                 !sim ? "not simulated" : net >= 0 ? state_name(U.net_state[net]) : "-");
    } else if (U.hover_part >= 0) {
        const part_t *hp = &S()->parts[U.hover_part];
        if (hp->kind == PART_IC)
            snprintf(tip, sizeof(tip), "%s %s: %s", hp->ref, board_part_label(U.b, U.hover_part),
                     board_part_detail(U.b, U.hover_part));
        else
            snprintf(tip, sizeof(tip), "%s %s", hp->ref, board_part_label(U.b, U.hover_part));
    } else {
        return;
    }
    nk_tooltip(ctx, tip);
}

static void board_window(struct nk_context *ctx, float w, float h) {
    nk_window_set_bounds(ctx, "Board", nk_rect(0, 0, w, h));
    if (nk_begin(ctx, "Board", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        struct nk_rect area = nk_window_get_content_region(ctx);
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        if (!U.fitted) fit_view(area);
        if (U.view_set) {       /* --view: center on a point of the board */
            U.zoom *= U.view_zoom;
            U.ox = area.w / 2 - U.view_cx * U.zoom;
            U.oy = area.h / 2 - U.view_cy * U.zoom;
            U.view_set = false;
        }
        board_input(ctx, area);
        nk_push_scissor(cv, area);
        nk_fill_rect(cv, area, 0, nk_rgb(32, 34, 36));
        struct nk_rect pcb = to_screen(area, bd_outline[0], bd_outline[1], bd_outline[2], bd_outline[3]);
        nk_fill_rect(cv, pcb, 8, COL_PCB);
        nk_stroke_rect(cv, pcb, 8, 2, COL_PCB_EDGE);
        sim_t *s = S();
        for (int pass = 0; pass < 2; pass++)          /* passives below, chips above */
            for (int i = 0; i < s->nparts; i++)
                if ((s->parts[i].kind == PART_IC) == (pass == 1)) draw_part(cv, ctx, area, i);
        draw_pins(cv, ctx, area);
        if (U.hover_part >= 0) {
            box_t *b = &U.box[U.hover_part];
            nk_stroke_rect(cv, to_screen(area, b->x0, b->y0, b->x1, b->y1), 2, 2, COL_HOVER);
        }
        board_tooltip(ctx);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Monitor                                                                   */
/* ------------------------------------------------------------------------ */

static void monitor_window(struct nk_context *ctx, float x, float y) {
    board_monitor(U.b, U.monitor);
    sg_update_image(U.img, &(sg_image_data){ .mip_levels[0] = SG_RANGE(U.monitor) });
    struct nk_rect r = nk_rect(x, y, RIGHT_W, MON_H + 44);
    nk_window_set_bounds(ctx, "Monitor", r);
    if (nk_begin(ctx, "Monitor", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_static(ctx, MON_H, MON_W, 1);
        nk_image(ctx, nk_image_handle(snk_nkhandle(U.snk_img)));
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Oscilloscope                                                              */
/* ------------------------------------------------------------------------ */

static const int timebase_cycles[] = { 16, 64, 256, 1024, 4096 };
static const char *timebase_names[] = { "1.6 us/div", "6.5 us/div", "26 us/div", "104 us/div", "416 us/div" };

static void draw_scope(struct nk_command_buffer *cv, struct nk_rect r) {
    board_t *b = U.b;
    nk_fill_rect(cv, r, 0, nk_rgb(8, 16, 10));
    /* Graticule: 10 x 4 divisions. */
    for (int i = 0; i <= 10; i++) {
        float x = r.x + r.w * i / 10;
        nk_stroke_line(cv, x, r.y, x, r.y + r.h, 1, nk_rgb(30, 60, 36));
    }
    for (int i = 0; i <= 4; i++) {
        float y = r.y + r.h * i / 4;
        nk_stroke_line(cv, r.x, y, r.x + r.w, y, 1, nk_rgb(30, 60, 36));
    }
    if (b->scope_pin < 0) return;
    uint32_t end = b->scope_pos;
    uint32_t span = (uint32_t)timebase_cycles[U.timebase] * STEPS_PER_CYCLE;
    uint32_t avail = end < SCOPE_SAMPLES ? end : SCOPE_SAMPLES;
    if (avail < span) return;
    uint32_t start = end - span;
    /* Trigger on a rising edge, placed at 10% of the screen. */
    if (U.trigger) {
        uint32_t pre = span / 10, oldest = end - avail + 1;
        for (uint32_t t = end - span + pre; t > oldest + pre; t--) {
            uint8_t a = b->scope[(t - 1) & (SCOPE_SAMPLES - 1)], c = b->scope[t & (SCOPE_SAMPLES - 1)];
            if (!(a & (SC_LEVEL | SC_FLOATING)) && (c & SC_LEVEL) && !(c & SC_FLOATING)) {
                start = t - pre;
                break;
            }
        }
    }
    float y_hi = r.y + r.h * 0.2f, y_lo = r.y + r.h * 0.8f, y_mid = r.y + r.h * 0.5f;
    float prev_y = -1;
    int cols = (int)r.w;
    for (int x = 0; x < cols; x++) {
        uint32_t i0 = start + (uint32_t)((uint64_t)span * x / cols);
        uint32_t i1 = start + (uint32_t)((uint64_t)span * (x + 1) / cols);
        if (i1 <= i0) i1 = i0 + 1;
        bool hi = false, lo = false, fl = false, fight = false;
        for (uint32_t i = i0; i < i1; i++) {
            uint8_t v = b->scope[i & (SCOPE_SAMPLES - 1)];
            if (v & SC_FIGHT) fight = true;
            if (v & SC_FLOATING) fl = true;
            else if (v & SC_LEVEL) hi = true;
            else lo = true;
        }
        float px = r.x + x;
        if (fight) {
            nk_stroke_line(cv, px, y_hi, px, y_lo, 1, COL_FIGHT);
            prev_y = y_mid;
        } else if (hi && lo) {
            nk_stroke_line(cv, px, y_hi, px, y_lo, 1, COL_TRACE);
            prev_y = -1;
        } else if (hi || lo) {
            float y = hi ? y_hi : y_lo;
            if (prev_y >= 0 && prev_y != y) nk_stroke_line(cv, px, prev_y, px, y, 1, COL_TRACE);
            nk_stroke_line(cv, px, y, px + 1, y, 1.5f, COL_TRACE);
            prev_y = y;
        } else if (fl) {
            /* Floating: dashed line in the middle, nobody drives the net. */
            if ((x / 3) % 2 == 0) nk_stroke_line(cv, px, y_mid, px + 1, y_mid, 1.5f, COL_PULSE);
            prev_y = -1;
        }
    }
}

static void scope_window(struct nk_context *ctx, float x, float y, float h) {
    struct nk_rect r = nk_rect(x, y, RIGHT_W, h);
    nk_window_set_bounds(ctx, "Oscilloscope", r);
    if (nk_begin(ctx, "Oscilloscope", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        char label[160] = "click a pin on the board";
        if (U.b->scope_pin >= 0) {
            char pd[96];
            pin_desc(U.b, U.b->scope_pin, pd, sizeof(pd));
            snprintf(label, sizeof(label), "%s   net %s", pd, pin_net_name(U.b, U.b->scope_pin));
        }
        nk_layout_row_begin(ctx, NK_STATIC, 22, 3);
        nk_layout_row_push(ctx, 250);
        nk_label(ctx, label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 110);
        U.timebase = nk_combo(ctx, timebase_names, 5, U.timebase, 20, nk_vec2(120, 140));
        nk_layout_row_push(ctx, 80);
        nk_bool trig = U.trigger;
        nk_checkbox_label(ctx, "trigger", &trig);
        U.trigger = trig;
        nk_layout_row_end(ctx);
        struct nk_rect area = nk_window_get_content_region(ctx);
        float plot_h = area.y + area.h - (ctx->current->layout->at_y + 30) - 4;
        nk_layout_row_dynamic(ctx, plot_h > 40 ? plot_h : 40, 1);
        struct nk_rect plot;
        if (nk_widget(&plot, ctx) != NK_WIDGET_INVALID) draw_scope(nk_window_get_canvas(ctx), plot);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Controls                                                                  */
/* ------------------------------------------------------------------------ */

static int cmp_entries(const void *a, const void *b) { return strcmp(a, b); }

static void scan_dir(void) {
    U.nentries = 0;
    DIR *d = opendir(U.dir);
    if (!d) return;
    /* ".." first, then directories, then .prg files. */
    strcpy(U.entries[U.nentries], "..");
    U.entry_is_dir[U.nentries++] = true;
    char files[256][256];
    int nfiles = 0, ndirs_start = U.nentries;
    struct dirent *e;
    while ((e = readdir(d)) && U.nentries < 200 && nfiles < 200) {
        if (e->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", U.dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            snprintf(U.entries[U.nentries], 256, "%s", e->d_name);
            U.entry_is_dir[U.nentries++] = true;
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && (!strcasecmp(e->d_name + n - 4, ".prg") || !strcasecmp(e->d_name + n - 4, ".crt")))
                snprintf(files[nfiles++], 256, "%s", e->d_name);
        }
    }
    closedir(d);
    qsort(U.entries[ndirs_start], U.nentries - ndirs_start, 256, cmp_entries);
    qsort(files, nfiles, 256, cmp_entries);
    for (int i = 0; i < nfiles && U.nentries < 256; i++) {
        memcpy(U.entries[U.nentries], files[i], 256);
        U.entry_is_dir[U.nentries++] = false;
    }
}

static void enter_dir(const char *name) {
    char path[2048];
    if (!strcmp(name, "..")) {
        char *slash = strrchr(U.dir, '/');
        if (slash && slash != U.dir) *slash = 0;
        else strcpy(U.dir, "/");
    } else {
        snprintf(path, sizeof(path), "%s%s%s", U.dir, strcmp(U.dir, "/") ? "/" : "", name);
        snprintf(U.dir, sizeof(U.dir), "%s", path);
    }
    scan_dir();
}

/* A .prg goes into RAM; a .crt cartridge into the expansion port. */
static void load_prg(const char *path, bool run) {
    snprintf(U.path, sizeof(U.path), "%s", path);
    size_t n = strlen(path);
    if (n > 4 && !strcasecmp(path + n - 4, ".crt")) {
        if (board_insert_cart(U.b, path, U.status, sizeof(U.status))) select_pin(U.b->scope_pin);
    } else {
        board_load_prg(U.b, path, run, U.status, sizeof(U.status));
    }
}

static void fault_list(struct nk_context *ctx) {
    sim_t *s = S();
    char line[200];
    int count = 0;
    nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
    for (int i = 0; i < s->npins; i++) {
        if (s->pins[i].fault == PF_OK) continue;
        char pd[96];
        pin_desc(U.b, i, pd, sizeof(pd));
        snprintf(line, sizeof(line), "%s: %s", pd, pin_fault_name(s->pins[i].fault));
        nk_layout_row_push(ctx, 340); nk_label(ctx, line, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 70);
        if (nk_button_label(ctx, "repair")) board_set_pin_fault(U.b, i, PF_OK);
        count++;
    }
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if (!p->removed && p->rfault == RF_OK) continue;
        snprintf(line, sizeof(line), "%s %s: %s", p->ref, board_part_label(U.b, i),
                 p->removed ? "pulled out" : p->rfault == RF_OPEN ? "open" : "shorted");
        nk_layout_row_push(ctx, 340); nk_label(ctx, line, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 70);
        if (nk_button_label(ctx, "repair")) {
            if (p->removed) board_set_removed(U.b, i, false);
            else board_set_rfault(U.b, i, RF_OK);
        }
        count++;
    }
    nk_layout_row_end(ctx);
    if (!count) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, "none: the board is healthy", NK_TEXT_LEFT, COL_DIMTEXT);
    }
}

static void legend_item(struct nk_context *ctx, struct nk_color c, const char *txt) {
    nk_layout_row_push(ctx, 12);
    struct nk_rect r;
    if (nk_widget(&r, ctx) != NK_WIDGET_INVALID)
        nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(r.x, r.y + 4, 10, 10), 0, c);
    nk_layout_row_push(ctx, 72);
    nk_label(ctx, txt, NK_TEXT_LEFT);
}

static void controls_window(struct nk_context *ctx, float x, float y, float h) {
    struct nk_rect r = nk_rect(x, y, RIGHT_W, h);
    nk_window_set_bounds(ctx, "Controls", r);
    if (nk_begin(ctx, "Controls", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
        board_t *b = U.b;
        nk_layout_row_dynamic(ctx, 26, 4);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Switch the C64 off and on again: a restart (the faults stay)");
        if (nk_button_label(ctx, "Power cycle")) { board_power_on(b); select_pin(b->scope_pin); }
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Undo every fault");
        if (nk_button_label(ctx, "Repair all")) board_clear_faults(b);
        if (nk_button_label(ctx, U.paused ? "Resume" : "Pause")) U.paused = !U.paused;
        nk_bool warp = U.warp;
        nk_checkbox_label(ctx, "Warp", &warp);
        U.warp = warp;
        /* RESET: pressed while the mouse holds it, or kept pressed. It
         * shorts /RESET to GND on the user port, like a reset cartridge:
         * the CPU, the CIAs and the SID reset, the VIC (no reset pin) and
         * the RAM don't. */
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Hold to press RESET: shorts /RESET to GND (user port pin 3 to pin 1)");
        nk_button_push_behavior(ctx, NK_BUTTON_REPEATER);
        bool down = nk_button_label(ctx, b->reset_button ? "RESET pressed" : "RESET");
        nk_button_pop_behavior(ctx);
        nk_layout_row_push(ctx, 150);
        nk_bool hold = U.reset_hold;
        nk_checkbox_label(ctx, "keep it pressed", &hold);
        U.reset_hold = hold;
        nk_layout_row_end(ctx);
        if ((down || U.reset_hold) != b->reset_button) board_set_reset_button(b, down || U.reset_hold);
        char info[200];
        snprintf(info, sizeof(info), "speed %.0f%%   time %.2f s   CPU %s",
                 U.speed * 100, (double)b->sim.cycle / C64_FREQ,
                 !b->cpu_running ? "stopped (no clock)" : b->cpu_in_reset ? "in reset" : "running");
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, info, NK_TEXT_LEFT);

        /* What is under the mouse. */
        char hover[220] = "";
        if (U.hover_pin >= 0) {
            char pd[96];
            pin_desc(U.b, U.hover_pin, pd, sizeof(pd));
            int net = S()->pins[U.hover_pin].net;
            snprintf(hover, sizeof(hover), "%s  net %s  %s", pd, pin_net_name(U.b, U.hover_pin),
                     net >= 0 ? state_name(U.net_state[net]) : "");
        } else if (U.hover_part >= 0) {
            const part_t *hp = &S()->parts[U.hover_part];
            if (hp->kind == PART_IC)
                snprintf(hover, sizeof(hover), "%s %s: %s", hp->ref, board_part_label(b, U.hover_part),
                         board_part_detail(b, U.hover_part));
            else
                snprintf(hover, sizeof(hover), "%s  %s (click to break it)", hp->ref,
                         board_part_label(b, U.hover_part));
        }
        nk_label_colored(ctx, hover[0] ? hover : "hover a pin or a chip, click to break it",
                         NK_TEXT_LEFT, COL_HOVER);

        nk_layout_row_begin(ctx, NK_STATIC, 18, 10);
        legend_item(ctx, COL_HIGH, "high");
        legend_item(ctx, COL_LOW, "low");
        legend_item(ctx, COL_PULSE, "pulsing");
        legend_item(ctx, COL_FLOAT, "floating");
        legend_item(ctx, COL_FIGHT, "fight");
        nk_layout_row_end(ctx);

        if (nk_tree_push(ctx, NK_TREE_TAB, "Faults", NK_MAXIMIZED)) {
            fault_list(ctx);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Programs and cartridges", NK_MAXIMIZED)) {
            /* The loopback harness of the 586220 diagnostic cartridge. */
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Dongles on the user, cassette, serial, control ports and keyboard that loop "
                                "them back, for the Diagnostic 586220 cartridge (the C64 is switched off and on)");
            nk_bool harness = b->harness;
            if (nk_checkbox_label(ctx, "586220 test harness plugged in", &harness) && harness != b->harness) {
                board_set_harness(b, harness);
                select_pin(b->scope_pin);
            }
            const char *cart = board_cart_name(b);
            if (cart) {
                char line[96];
                snprintf(line, sizeof(line), "Cartridge in the expansion port: %s", cart);
                nk_layout_row_begin(ctx, NK_STATIC, 22, 2);
                nk_layout_row_push(ctx, 330);
                nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_SOCKET);
                nk_layout_row_push(ctx, 80);
                if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Pull the cartridge out (the C64 is switched off and on)");
                if (nk_button_label(ctx, "remove")) { board_remove_cart(b); select_pin(b->scope_pin); }
                nk_layout_row_end(ctx);
            }
            nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
            nk_layout_row_push(ctx, 250);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, U.path, sizeof(U.path), nk_filter_default);
            nk_layout_row_push(ctx, 70);
            if (nk_button_label(ctx, "Load")) load_prg(U.path, false);
            nk_layout_row_push(ctx, 90);
            if (nk_button_label(ctx, "Load+RUN")) load_prg(U.path, true);
            nk_layout_row_end(ctx);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, U.status[0] ? U.status : "or drop a .prg or .crt file on the window",
                             NK_TEXT_LEFT, COL_DIMTEXT);
            nk_label(ctx, U.dir, NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 140, 1);
            if (nk_group_begin(ctx, "files", NK_WINDOW_BORDER)) {
                nk_layout_row_dynamic(ctx, 18, 1);
                for (int i = 0; i < U.nentries; i++) {
                    char name[270];
                    snprintf(name, sizeof(name), "%s%s", U.entries[i], U.entry_is_dir[i] ? "/" : "");
                    if (nk_select_label(ctx, name, NK_TEXT_LEFT, 0)) {
                        if (U.entry_is_dir[i]) { enter_dir(U.entries[i]); break; }
                        char full[2048];
                        snprintf(full, sizeof(full), "%s/%s", U.dir, U.entries[i]);
                        load_prg(full, true);
                    }
                }
                nk_group_end(ctx);
            }
            nk_tree_pop(ctx);
        }
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "Keys: ESC=RUN/STOP  PgUp=RESTORE  TAB=CTRL  ALT=C=",
                         NK_TEXT_LEFT, COL_DIMTEXT);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* The simulation thread                                                     */
/* ------------------------------------------------------------------------ */

/* The simulation runs in its own thread, so it can use a whole CPU core.
 * The interface takes the lock only while it looks at or changes the
 * board; when it is waiting for the lock the simulation steps aside. */
static pthread_t sim_thread;
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool ui_waiting, sim_quit;

static void lock_board(void) {
    atomic_store(&ui_waiting, true);
    pthread_mutex_lock(&sim_lock);
    atomic_store(&ui_waiting, false);
}

static void unlock_board(void) { pthread_mutex_unlock(&sim_lock); }

#define SIM_CHUNK 1000      /* cycles run while holding the lock */

static void *sim_main(void *arg) {
    (void)arg;
    double ref_time = stm_sec(stm_now());
    uint64_t ref_cycle = 0;
    int frame_cycles = 0;
    while (!atomic_load(&sim_quit)) {
        pthread_mutex_lock(&sim_lock);
        bool running = !U.paused, warp = U.warp;
        if (running) {
            board_run(U.b, SIM_CHUNK);
            /* Once per emulated video frame: keyboard housekeeping. */
            if ((frame_cycles += SIM_CHUNK) >= CYCLES_PER_FRAME) {
                frame_cycles -= CYCLES_PER_FRAME;
                board_frame_update(U.b, 20000);
            }
        }
        uint64_t cycle = U.b->sim.cycle;
        pthread_mutex_unlock(&sim_lock);
        while (atomic_load(&ui_waiting)) sched_yield();
        /* Never run faster than a real C64 (unless warp), never try to
         * catch up when slower. */
        double now = stm_sec(stm_now());
        if (!running) { usleep(10000); ref_time = now; ref_cycle = cycle; continue; }
        double ahead = (double)(cycle - ref_cycle) / C64_FREQ - (now - ref_time);
        if (warp || cycle < ref_cycle || ahead < -0.05) { ref_time = now; ref_cycle = cycle; }
        else if (ahead > 0.002) usleep((useconds_t)(ahead * 1e6));
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Frame                                                                     */
/* ------------------------------------------------------------------------ */

/* Speed of the simulation (fraction of a real C64), and the program to
 * load once BASIC is ready (given on the command line). */
static void update_status(void) {
    static uint64_t last_cycle;
    double now = stm_sec(stm_now());
    double dt = U.last_time ? now - U.last_time : 0;
    uint64_t cycle = U.b->sim.cycle;
    if (dt > 0 && cycle >= last_cycle) {
        double sp = U.paused ? 0 : (double)(cycle - last_cycle) / (dt * C64_FREQ);
        U.speed = U.speed * 0.9 + sp * 0.1;
    }
    U.last_time = now;
    last_cycle = cycle;
    if (U.autoload && cycle > 3 * C64_FREQ) {
        load_prg(U.autoload, true);
        U.autoload = NULL;
    }
}

void ui_frame(void) {
    lock_board();
    update_status();
    /* Logic probe state for the pin colors, from this frame's samples. */
    for (int i = 0; i < S()->nnets; i++) U.net_state[i] = (uint8_t)board_net_state(U.b, i);

    struct nk_context *ctx = snk_new_frame();
    /* Make checked checkboxes visible with the default style. */
    ctx->style.checkbox.cursor_normal = nk_style_item_color(COL_TRACE);
    ctx->style.checkbox.cursor_hover = nk_style_item_color(COL_TRACE);
    float W = sapp_widthf(), H = sapp_heightf();
    float bw = W - RIGHT_W;
    board_window(ctx, bw, H);
    monitor_window(ctx, bw, 0);
    float y = MON_H + 44;
    float ctrl_h = H - y - SCOPE_H;
    controls_window(ctx, bw, y, ctrl_h);
    scope_window(ctx, bw, y + ctrl_h, SCOPE_H);
    int clicked = inspect_window(ctx, U.b, U.net_state, nk_rect(0, 0, bw, H));
    if (clicked >= 0) {         /* a pin clicked in the inspector */
        select_pin(clicked);
        U.menu_open = true;
        U.menu_pin = clicked;
        U.menu_part = -1;
        U.menu_x = ctx->input.mouse.pos.x + 10;
        U.menu_y = ctx->input.mouse.pos.y + 10;
        U.menu_frame = (int)U.frame;
    }
    fault_menu(ctx);
    /* Start the statistics over (the drawing above read this frame's). */
    board_stats_reset(U.b);
    unlock_board();

    sg_begin_pass(&(sg_pass){ .action = { .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                              .clear_value = { 0.12f, 0.13f, 0.14f, 1.0f } } },
                              .swapchain = sglue_swapchain() });
    snk_render(sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
    U.frame++;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

static int special_key(sapp_keycode k) {
    switch (k) {
    case SAPP_KEYCODE_ENTER: return C64_KEY_RETURN;
    case SAPP_KEYCODE_BACKSPACE: return C64_KEY_DEL;
    case SAPP_KEYCODE_LEFT: return C64_KEY_CSRLEFT;
    case SAPP_KEYCODE_RIGHT: return C64_KEY_CSRRIGHT;
    case SAPP_KEYCODE_UP: return C64_KEY_CSRUP;
    case SAPP_KEYCODE_DOWN: return C64_KEY_CSRDOWN;
    case SAPP_KEYCODE_HOME: return C64_KEY_HOME;
    case SAPP_KEYCODE_ESCAPE: return C64_KEY_STOP;
    case SAPP_KEYCODE_PAGE_UP: return C64_KEY_RESTORE;
    case SAPP_KEYCODE_TAB: return C64_KEY_CTRL;
    case SAPP_KEYCODE_LEFT_ALT: return C64_KEY_CBM;
    case SAPP_KEYCODE_F1: return C64_KEY_F1;
    case SAPP_KEYCODE_F3: return C64_KEY_F3;
    case SAPP_KEYCODE_F5: return C64_KEY_F5;
    case SAPP_KEYCODE_F7: return C64_KEY_F7;
    default: return 0;
    }
}

static void handle_event(const sapp_event *ev);

void ui_event(const sapp_event *ev) {
    lock_board();
    handle_event(ev);
    unlock_board();
}

static void handle_event(const sapp_event *ev) {
    bool nk_wants_keys = snk_handle_event(ev);
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED && sapp_get_num_dropped_files() > 0) {
        load_prg(sapp_get_dropped_file_path(0), true);
        return;
    }
    if (nk_wants_keys) return;      /* typing in a text field */
    if (ev->type == SAPP_EVENTTYPE_CHAR) {
        int c = (int)ev->char_code;
        if (c < 32 || c >= 127 || (ev->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER))) return;
        /* The C64 types uppercase letters without shift. */
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z') c += 32;
        board_key_down(U.b, c);
        board_key_up(U.b, c);
    } else if (ev->type == SAPP_EVENTTYPE_KEY_DOWN || ev->type == SAPP_EVENTTYPE_KEY_UP) {
        int k = special_key(ev->key_code);
        if (!k) return;
        if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) board_key_down(U.b, k);
        else board_key_up(U.b, k);
    }
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

void ui_init(board_t *b, const char *prg, const char *scope_pin) {
    U.b = b;
    U.hover_pin = U.hover_part = -1;
    U.trigger = true;
    U.timebase = 1;
    U.autoload = prg;
    U.net_state = calloc(b->sim.nnets, 1);
    b->pin_stats = true;        /* what each pin drives, for the inspector */
    compute_boxes();
    U.img = sg_make_image(&(sg_image_desc){ .width = SCREEN_W, .height = SCREEN_H,
                                            .pixel_format = SG_PIXELFORMAT_RGBA8,
                                            .usage.dynamic_update = true });
    U.view = sg_make_view(&(sg_view_desc){ .texture.image = U.img });
    U.smp = sg_make_sampler(&(sg_sampler_desc){ .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
                                                .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    U.snk_img = snk_make_image(&(snk_image_desc_t){ .texture_view = U.view, .sampler = U.smp });
    if (!getcwd(U.dir, sizeof(U.dir))) strcpy(U.dir, ".");
    scan_dir();
    if (scope_pin) {    /* --scope REF.PIN */
        char ref[16];
        int num;
        if (sscanf(scope_pin, "%15[^.].%d", ref, &num) == 2) {
            int part = sim_find_part(&b->sim, ref);
            if (part >= 0 && part_pin(&b->sim.parts[part], num) >= 0) select_pin(part_pin(&b->sim.parts[part], num));
        }
    }
    pthread_create(&sim_thread, NULL, sim_main, NULL);
}

void ui_shutdown(void) {
    atomic_store(&sim_quit, true);
    pthread_join(sim_thread, NULL);
}

/* Open the inspector on a chip by reference ("U8"), for --inspect. */
void ui_inspect(const char *ref) {
    lock_board();
    int part = sim_find_part(S(), ref);
    if (part >= 0) inspect_open(U.b, part);
    else fprintf(stderr, "no part %s\n", ref);
    unlock_board();
}

/* Center the board view on (cx, cy) mm, zoom times the "fit" zoom. */
void ui_view(float cx, float cy, float zoom) {
    U.view_set = true;
    U.view_cx = cx;
    U.view_cy = cy;
    U.view_zoom = zoom;
}

/* Emulated video frames since power on (50 per second). */
int ui_emulated_frame(void) {
    lock_board();
    int f = (int)(U.b->sim.cycle / CYCLES_PER_FRAME);
    unlock_board();
    return f;
}

/* Apply a fault spec ("U5:removed", "U6.17:open"...), or "repair" to
 * repair everything, as if done with the mouse. */
void ui_apply_fault(const char *spec) {
    lock_board();
    if (!strcmp(spec, "repair")) board_clear_faults(U.b);
    else if (!board_apply_fault_spec(U.b, spec)) fprintf(stderr, "bad fault spec: %s\n", spec);
    unlock_board();
}

void ui_save_screenshot(const char *path) {
    int w = sapp_width(), h = sapp_height();
    uint32_t *px = malloc((size_t)w * h * 4), *flip = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) flip[y * w + x] = px[(h - 1 - y) * w + x] | 0xFF000000u;
    png_write(path, flip, w, h);
    free(px);
    free(flip);
}
