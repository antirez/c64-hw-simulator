/* ui_common.h - colors and small drawing helpers shared by the interface
 * files: ui.c (board, monitor, controls, scope) and inspect.c (the chip
 * inspector and the logic diagrams). */
#ifndef UI_COMMON_H
#define UI_COMMON_H
#include "ui.h"
#include "board_data.h"
#include <stdio.h>
#include <string.h>

/* Colors. */
#define COL_PCB       nk_rgb(28, 88, 52)
#define COL_PCB_EDGE  nk_rgb(10, 45, 25)
#define COL_CHIP      nk_rgb(26, 26, 28)
#define COL_CHIP_EDGE nk_rgb(100, 100, 108)
#define COL_SOCKET    nk_rgb(200, 165, 90)
#define COL_RES       nk_rgb(205, 182, 140)
#define COL_LINK      nk_rgb(70, 70, 76)
#define COL_OTHER     nk_rgb(60, 120, 80)
#define COL_TEXT      nk_rgb(235, 235, 225)
#define COL_DIMTEXT   nk_rgb(150, 160, 150)
#define COL_HIGH      nk_rgb(240, 80, 60)
#define COL_LOW       nk_rgb(70, 140, 255)
#define COL_PULSE     nk_rgb(250, 210, 60)
#define COL_FLOAT     nk_rgb(160, 160, 160)
#define COL_FIGHT     nk_rgb(255, 60, 255)
#define COL_NONE      nk_rgb(70, 70, 70)
#define COL_FAULT     nk_rgb(255, 150, 0)
#define COL_HOVER     nk_rgb(0, 255, 255)
#define COL_TRACE     nk_rgb(90, 255, 120)

/* Chip names are colored by role (see ROLE_* in board.h). */
static const struct nk_color role_color[] = {
    [ROLE_RAM]   = { 120, 195, 255, 255 },    /* the 8 DRAMs */
    [ROLE_VRAM]  = { 225, 140, 255, 255 },    /* color RAM */
    [ROLE_ROM]   = { 255, 195, 100, 255 },
    [ROLE_PLA]   = { 255, 110, 140, 255 },
    [ROLE_CHIP]  = { 240, 240, 230, 255 },    /* CPU, VIC, SID, CIAs */
    [ROLE_GLUE]  = { 175, 195, 175, 255 },
    [ROLE_CLOCK] = { 135, 140, 135, 255 },
    [ROLE_OTHER] = { 235, 235, 225, 255 },
};

static inline float text_width(struct nk_context *ctx, const char *txt) {
    const struct nk_user_font *f = ctx->style.font;
    return f->width(f->userdata, f->height, txt, (int)strlen(txt));
}

/* Text centered in a rectangle. */
static inline void text_centered(struct nk_command_buffer *cv, struct nk_context *ctx,
                                 struct nk_rect r, const char *txt, struct nk_color c) {
    const struct nk_user_font *f = ctx->style.font;
    float w = text_width(ctx, txt);
    nk_draw_text(cv, nk_rect(r.x + (r.w - w) / 2, r.y + (r.h - f->height) / 2, w + 2, f->height),
                 txt, (int)strlen(txt), f, nk_rgba(0, 0, 0, 0), c);
}

/* Text starting at x (align 0), ending at x (align 1), vertically
 * centered on y. */
static inline void text_at(struct nk_command_buffer *cv, struct nk_context *ctx, float x, float y,
                           int align, const char *txt, struct nk_color c) {
    const struct nk_user_font *f = ctx->style.font;
    float w = text_width(ctx, txt);
    if (align) x -= w;
    nk_draw_text(cv, nk_rect(x, y - f->height / 2, w + 2, f->height), txt, (int)strlen(txt), f,
                 nk_rgba(0, 0, 0, 0), c);
}

static inline struct nk_color state_color(int st) {
    switch (st) {
    case NET_HIGH: return COL_HIGH;
    case NET_LOW: return COL_LOW;
    case NET_PULSING: return COL_PULSE;
    case NET_FLOATING: return COL_FLOAT;
    case NET_CONTENTION: return COL_FIGHT;
    default: return COL_NONE;
    }
}

static inline const char *state_name(int st) {
    switch (st) {
    case NET_HIGH: return "HIGH";
    case NET_LOW: return "LOW";
    case NET_PULSING: return "PULSING";
    case NET_FLOATING: return "FLOATING";
    case NET_CONTENTION: return "CONTENTION";
    default: return "not connected";
    }
}

static inline const char *pin_fault_name(int f) {
    switch (f) {
    case PF_OPEN: return "floating (open)";
    case PF_GND: return "shorted to GND";
    case PF_VCC: return "shorted to +5V";
    default: return "working";
    }
}

/* Short description of a pin: "U17.18 /CASRAM (PLA)", "CN4.3 ATN (SERIAL)". */
static inline void pin_desc(board_t *b, int pin, char *buf, size_t len) {
    const sim_t *s = &b->sim;
    const pin_t *p = &s->pins[pin];
    const part_t *part = &s->parts[p->part];
    const char *name = sim_pin_name(s, pin), *func = board_pin_func(b, pin);
    if (part->kind == PART_IC)
        snprintf(buf, len, "%s.%s %s (%s)", part->ref, bd_pads[pin].name, name ? name : "",
                 board_part_label(b, p->part));
    else if (func)
        snprintf(buf, len, "%s.%s %s (%s)", part->ref, bd_pads[pin].name, func,
                 board_connector_name(b, p->part));
    else
        snprintf(buf, len, "%s.%s %s", part->ref, bd_pads[pin].name, name ? name : "");
}

static inline const char *pin_net_name(board_t *b, int pin) {
    int net = b->sim.pins[pin].net;
    return net >= 0 ? b->sim.nets[net].name : "(none)";
}

#endif
