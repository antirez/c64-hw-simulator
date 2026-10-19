/* browser.c - the file browser to open what the machine on the bench
 * takes (C64 programs and cartridges, Spectrum snapshots), made of
 * nuklear's pieces since it has no file dialog.
 *
 * A window over the board with:
 *  - the folder as a row of buttons, one per level (click to go there),
 *    and shortcuts to carts/, the start folder, home;
 *  - a filter field that always has the keyboard: typing narrows the
 *    list; a path (starting with / or ~) and Enter goes there;
 *  - the list: folders, then the files the machine takes, each with what
 *    it is, read from the file by the machine (a BASIC program, machine
 *    code and its address, a cartridge's name and kind, a snapshot, or
 *    why it can't be used);
 *  - arrows, Page Up/Down, Home/End move; Enter or a double click opens
 *    (a program is loaded and RUN; Shift+Enter or "Load" only loads it);
 *    Backspace with an empty filter goes up; Esc clears the filter, then
 *    closes the browser. */
#include "browser.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FE_UP, FE_DIR, FE_FILE };

typedef struct {
    char name[256];
    int kind;           /* FE_* */
    long size;
    char info[96];      /* what it is */
    bool bad;           /* a file the machine can't use */
    const file_kind_t *fk;
} fentry_t;

#define ROW_H 20

static struct {
    const machine_t *m;
    bool open, reopen;
    char dir[PATH_MAX];
    char start_dir[PATH_MAX], carts_dir[PATH_MAX], home_dir[PATH_MAX];
    fentry_t *all;      /* the folder */
    int nall, cap;
    int *shown, nshown; /* the entries passing the filter */
    char filter[256], prev_filter[256];
    int sel;            /* in shown[] */
    nk_uint scroll_x, scroll_y;
    bool ensure_visible;
    char error[300];
    /* Keys from browser_key(), handled in the next frame. */
    int move;
    bool enter, enter_norun, up, esc;
    double last_click;
    int last_click_sel;
    struct nk_rect bounds;
    char result[PATH_MAX + 260];
    char picked[256];   /* the last file picked, selected when reopening */
    char recent[6][PATH_MAX];
    int nrecent;
} B;

/* ------------------------------------------------------------------------ */
/* Reading a folder                                                          */
/* ------------------------------------------------------------------------ */

/* Does s contain sub, ignoring case? (strcasestr isn't everywhere) */
static bool contains_nocase(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return true;
    return n == 0;
}

static int cmp_entries(const void *a, const void *b) {
    const fentry_t *x = a, *y = b;
    if (x->kind != y->kind) {
        /* the parent first, then folders, then files */
        int kx = x->kind == FE_UP ? 0 : x->kind == FE_DIR ? 1 : 2;
        int ky = y->kind == FE_UP ? 0 : y->kind == FE_DIR ? 1 : 2;
        if (kx != ky) return kx - ky;
    }
    return strcasecmp(x->name, y->name);
}

static fentry_t *add_entry(void) {
    if (B.nall == B.cap) {
        B.cap = B.cap ? B.cap * 2 : 256;
        B.all = realloc(B.all, (size_t)B.cap * sizeof(fentry_t));
    }
    fentry_t *e = &B.all[B.nall++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void apply_filter(void) {
    free(B.shown);
    B.shown = malloc((size_t)(B.nall + 1) * sizeof(int));
    B.nshown = 0;
    /* A path being typed doesn't filter. While filtering ".." is left
     * out: Enter with nothing matching must not go up. */
    bool path = B.filter[0] == '/' || B.filter[0] == '~';
    for (int i = 0; i < B.nall; i++) {
        const fentry_t *e = &B.all[i];
        if (!path && B.filter[0] && (e->kind == FE_UP || !contains_nocase(e->name, B.filter))) continue;
        B.shown[B.nshown++] = i;
    }
    /* Select the first file or folder, not the parent. */
    B.sel = B.nshown > 1 && !path && B.all[B.shown[0]].kind == FE_UP ? 1 : 0;
    B.scroll_y = 0;
    B.ensure_visible = true;
}

static void scan(const char *select_name) {
    B.nall = 0;
    DIR *d = opendir(B.dir);
    if (strcmp(B.dir, "/")) {
        fentry_t *e = add_entry();
        snprintf(e->name, sizeof(e->name), "..");
        e->kind = FE_UP;
        snprintf(e->info, sizeof(e->info), "the folder above");
    }
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[PATH_MAX + 260];
        snprintf(full, sizeof(full), "%s%s%s", B.dir, strcmp(B.dir, "/") ? "/" : "", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        char info[96];
        bool bad = false;
        const file_kind_t *fk = NULL;
        if (!S_ISDIR(st.st_mode)) {
            fk = B.m ? B.m->file_kind(full, info, sizeof(info), &bad) : NULL;
            if (!fk) continue;
        }
        fentry_t *e = add_entry();
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->kind = fk ? FE_FILE : FE_DIR;
        e->size = (long)st.st_size;
        e->fk = fk;
        e->bad = bad;
        if (fk) snprintf(e->info, sizeof(e->info), "%s", info);
    }
    if (d) closedir(d);
    else snprintf(B.error, sizeof(B.error), "can't read %s", B.dir);
    qsort(B.all, (size_t)B.nall, sizeof(fentry_t), cmp_entries);
    B.filter[0] = B.prev_filter[0] = 0;
    apply_filter();
    /* Going up: select the folder we came from. */
    for (int i = 0; select_name && i < B.nshown; i++)
        if (!strcmp(B.all[B.shown[i]].name, select_name)) B.sel = i;
    B.ensure_visible = true;
}

/* Go to a folder (absolute, or relative to the current one). */
static bool go(const char *path) {
    char want[PATH_MAX * 2], real[PATH_MAX];
    if (path[0] == '/') snprintf(want, sizeof(want), "%s", path);
    else snprintf(want, sizeof(want), "%s/%s", B.dir, path);
    if (!realpath(want, real)) return false;
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    /* Remember the child, to select it when going up. */
    char child[256] = "";
    size_t n = strlen(real);
    if (!strncmp(B.dir, real, n) && (B.dir[n] == '/' || n == 1)) {
        const char *rest = B.dir + n + (n > 1);
        snprintf(child, sizeof(child), "%.*s", (int)strcspn(rest, "/"), rest);
    }
    snprintf(B.dir, sizeof(B.dir), "%s", real);
    B.error[0] = 0;
    scan(child[0] ? child : NULL);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Opening                                                                   */
/* ------------------------------------------------------------------------ */

void browser_init(void) {
    if (!getcwd(B.start_dir, sizeof(B.start_dir))) strcpy(B.start_dir, "/");
    const char *home = getenv("HOME");
    snprintf(B.home_dir, sizeof(B.home_dir), "%s", home ? home : "/");
    char carts[PATH_MAX + 16];
    snprintf(carts, sizeof(carts), "%s/carts", B.start_dir);
    struct stat st;
    if (stat(carts, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(B.carts_dir, sizeof(B.carts_dir), "%s", carts);
    snprintf(B.dir, sizeof(B.dir), "%s", B.carts_dir[0] ? B.carts_dir : B.start_dir);
}

void browser_open(void) {
    if (B.open) return;
    B.open = B.reopen = true;
    B.error[0] = 0;
    B.move = 0;
    B.enter = B.up = B.esc = false;
    if (!go(B.dir)) go(B.start_dir);
    for (int i = 0; i < B.nshown; i++)
        if (!strcmp(B.all[B.shown[i]].name, B.picked)) B.sel = i;
}

void browser_close(void) { B.open = false; }

void browser_set_machine(const machine_t *m) {
    B.m = m;
    if (B.open) go(B.dir);
}
bool browser_is_open(void) { return B.open; }

void browser_error(const char *msg) { snprintf(B.error, sizeof(B.error), "%s", msg); }

bool browser_bounds(struct nk_rect *r) {
    if (!B.open) return false;
    *r = B.bounds;
    return true;
}

void browser_add_recent(const char *path) {
    char real[PATH_MAX];
    if (!realpath(path, real)) snprintf(real, sizeof(real), "%s", path);
    int i, n = B.nrecent;
    for (i = 0; i < n && strcmp(B.recent[i], real); i++) ;
    if (i == n && n < 6) B.nrecent = ++n;
    if (i == n) i = n - 1;
    memmove(B.recent[1], B.recent[0], (size_t)i * sizeof(B.recent[0]));
    snprintf(B.recent[0], sizeof(B.recent[0]), "%s", real);
}

int browser_recent_count(void) { return B.nrecent; }
const char *browser_recent(int i) { return B.recent[i]; }

/* Open the selected entry: go into a folder, or pick a file. */
static const char *open_selected(bool run, bool *runp) {
    if (B.sel < 0 || B.sel >= B.nshown) return NULL;
    const fentry_t *e = &B.all[B.shown[B.sel]];
    if (e->kind == FE_UP || e->kind == FE_DIR) {
        go(e->name);
        return NULL;
    }
    if (e->bad) {
        snprintf(B.error, sizeof(B.error), "%s: %s", e->name, e->info);
        return NULL;
    }
    snprintf(B.result, sizeof(B.result), "%s/%s", B.dir, e->name);
    snprintf(B.picked, sizeof(B.picked), "%s", e->name);
    *runp = run;
    return B.result;
}

/* Enter on a path typed in the filter: go there, or open that file. */
static const char *open_typed(bool run, bool *runp) {
    char path[PATH_MAX];
    if (B.filter[0] == '~') snprintf(path, sizeof(path), "%s%s", B.home_dir, B.filter + 1);
    else snprintf(path, sizeof(path), "%s", B.filter);
    if (go(path)) return NULL;
    struct stat st;
    char info[96];
    bool bad = false;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && B.m && B.m->file_kind(path, info, sizeof(info), &bad)) {
        snprintf(B.result, sizeof(B.result), "%s", path);
        *runp = run;
        return B.result;
    }
    snprintf(B.error, sizeof(B.error), "no folder or file for the %s at %s", B.m ? B.m->name : "machine", path);
    return NULL;
}

void browser_key(const sapp_event *ev) {
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    switch (ev->key_code) {
    case SAPP_KEYCODE_UP: B.move -= 1; break;
    case SAPP_KEYCODE_DOWN: B.move += 1; break;
    case SAPP_KEYCODE_PAGE_UP: B.move -= 10; break;
    case SAPP_KEYCODE_PAGE_DOWN: B.move += 10; break;
    case SAPP_KEYCODE_HOME: if (!B.filter[0]) B.move = -100000; break;
    case SAPP_KEYCODE_END: if (!B.filter[0]) B.move = 100000; break;
    case SAPP_KEYCODE_ENTER:
    case SAPP_KEYCODE_KP_ENTER:
        B.enter = true;
        B.enter_norun = (ev->modifiers & SAPP_MODIFIER_SHIFT) != 0;
        break;
    case SAPP_KEYCODE_ESCAPE: B.esc = true; break;
    case SAPP_KEYCODE_BACKSPACE: if (!B.filter[0]) B.up = true; break;
    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/* The window                                                                */
/* ------------------------------------------------------------------------ */

static struct nk_color kind_color(const fentry_t *e) {
    if (e->bad) return nk_rgb(200, 110, 110);
    switch (e->kind) {
    case FE_UP:
    case FE_DIR: return nk_rgb(140, 190, 255);
    default: return e->fk && e->fk->role >= 0 ? role_color[e->fk->role] : COL_TEXT;
    }
}

/* Cut a string until it fits in w pixels. */
static void fit(struct nk_context *ctx, char *txt, float w) {
    size_t n = strlen(txt);
    while (n > 3 && text_width(ctx, txt) > w) {
        txt[--n] = 0;
        txt[n - 1] = txt[n - 2] = '.';
    }
}

/* The same, cutting the beginning: the end of a path is what matters. */
static void fit_left(struct nk_context *ctx, char *txt, float w) {
    if (text_width(ctx, txt) <= w) return;
    size_t n = strlen(txt), cut = 0;
    char buf[PATH_MAX + 400];
    do {
        cut++;
        snprintf(buf, sizeof(buf), "...%s", txt + cut);
    } while (cut + 1 < n && text_width(ctx, buf) > w);
    memcpy(txt, buf, strlen(buf) + 1);
}

static void size_text(long size, char *buf, size_t len) {
    if (size < 1024) snprintf(buf, len, "%ld B", size);
    else snprintf(buf, len, "%.1f KB", size / 1024.0);
}

/* The folder as buttons, one per level; the leading ones are left out
 * (as "...") when they don't fit. */
static void path_row(struct nk_context *ctx, float width) {
    static char prefix[64][PATH_MAX], names[64][256];
    const char *label[64];
    int n = 0;
    snprintf(prefix[n], PATH_MAX, "/");
    label[n++] = "/";
    for (const char *s = B.dir + 1; *s && n < 64; ) {
        size_t len = strcspn(s, "/");
        snprintf(names[n], sizeof(names[n]), "%.*s", (int)len, s);
        snprintf(prefix[n], PATH_MAX, "%.*s", (int)(s - B.dir + (long)len), B.dir);
        label[n] = names[n];
        n++;
        s += len + (s[len] == '/');
    }
    float pad = 16, dots = text_width(ctx, "...") + pad;
    int first = 0;
    float total = 0;
    for (int i = 0; i < n; i++) total += text_width(ctx, label[i]) + pad + 4;
    while (first < n - 1 && total + (first ? dots : 0) > width) total -= text_width(ctx, label[first++]) + pad + 4;
    nk_layout_row_begin(ctx, NK_STATIC, 24, n - first + (first ? 1 : 0));
    if (first) {
        nk_layout_row_push(ctx, dots);
        nk_label_colored(ctx, "...", NK_TEXT_CENTERED, COL_DIMTEXT);
    }
    for (int i = first; i < n; i++) {
        nk_layout_row_push(ctx, text_width(ctx, label[i]) + pad);
        if (nk_button_label(ctx, label[i]) && i != n - 1) go(prefix[i]);
    }
    nk_layout_row_end(ctx);
}

const char *browser_window(struct nk_context *ctx, struct nk_rect area, bool *run) {
    if (!B.open) return NULL;
    const char *picked = NULL;

    /* The keys of the last frame. */
    if (B.esc) {
        if (B.filter[0]) { B.filter[0] = 0; }
        else { B.open = false; B.esc = false; return NULL; }
        B.esc = false;
    }
    if (B.up) { B.up = false; go(".."); }
    if (B.move && B.nshown) {
        B.sel += B.move;
        if (B.sel < 0) B.sel = 0;
        if (B.sel >= B.nshown) B.sel = B.nshown - 1;
        B.ensure_visible = true;
    }
    B.move = 0;
    if (B.enter) {
        B.enter = false;
        bool path = B.filter[0] == '/' || B.filter[0] == '~';
        picked = path ? open_typed(!B.enter_norun, run) : open_selected(!B.enter_norun, run);
    }

    float w = fminf(820, area.w - 40), h = fminf(600, area.h - 40);
    struct nk_rect r = nk_rect(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
    if (B.reopen) {
        nk_window_show(ctx, "Browser", NK_SHOWN);
        nk_window_set_bounds(ctx, "Browser", r);
        nk_window_set_focus(ctx, "Browser");
        B.reopen = false;
    }
    if (!nk_begin_titled(ctx, "Browser", B.m ? B.m->files_title : "Open a file", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
        B.open = false;
        nk_end(ctx);
        return picked;
    }
    B.bounds = nk_window_get_bounds(ctx);
    struct nk_rect region = nk_window_get_content_region(ctx);
    struct nk_input *in = &ctx->input;
    bool on_top = !(ctx->current->layout->flags & NK_WINDOW_ROM);
    float sp = ctx->style.window.spacing.y;

    /* The folder, then the shortcuts and the filter. */
    path_row(ctx, region.w - 8);
    const char *places[3] = { "carts", "start folder", "home" };
    const char *place_dir[3] = { B.carts_dir, B.start_dir, B.home_dir };
    nk_layout_row_begin(ctx, NK_STATIC, 24, 5);
    float used = 0;
    for (int i = 0; i < 3; i++) {
        float bw = text_width(ctx, places[i]) + 18;
        nk_layout_row_push(ctx, bw);
        used += bw + 4;
        if (!place_dir[i][0]) { nk_label(ctx, "", NK_TEXT_LEFT); continue; }
        if (nk_button_label(ctx, places[i])) go(place_dir[i]);
    }
    nk_layout_row_push(ctx, 48);
    nk_label_colored(ctx, "find:", NK_TEXT_RIGHT, COL_DIMTEXT);
    nk_layout_row_push(ctx, region.w - used - 60);
    /* The filter always has the keyboard. */
    nk_edit_focus(ctx, 0);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_GOTO_END_ON_ACTIVATE, B.filter, sizeof(B.filter),
                                   nk_filter_default);
    nk_layout_row_end(ctx);
    if (strcmp(B.filter, B.prev_filter)) {
        snprintf(B.prev_filter, sizeof(B.prev_filter), "%s", B.filter);
        apply_filter();
    }

    /* Column titles. */
    float cx[4] = { 6, 0.38f, 0.47f, 0.58f };    /* name, type, size, what it is */
    nk_layout_row_dynamic(ctx, 18, 1);
    struct nk_rect hr;
    if (nk_widget(&hr, ctx) != NK_WIDGET_INVALID) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        text_at(cv, ctx, hr.x + cx[0], hr.y + 9, 0, "name", COL_DIMTEXT);
        text_at(cv, ctx, hr.x + hr.w * cx[1], hr.y + 9, 0, "type", COL_DIMTEXT);
        text_at(cv, ctx, hr.x + hr.w * cx[3] - 12, hr.y + 9, 1, "size", COL_DIMTEXT);
        text_at(cv, ctx, hr.x + hr.w * cx[3], hr.y + 9, 0, "what it is", COL_DIMTEXT);
    }

    /* The list. */
    float list_h = region.y + region.h - (ctx->current->layout->at_y + ctx->current->layout->row.height) -
                   (20 + 28 + 4 * sp) - 6;
    if (list_h < 60) list_h = 60;
    if (B.ensure_visible && B.nshown) {
        float pitch = ROW_H + sp, view = list_h - 2 * ctx->style.window.group_padding.y - 4;
        float top = B.sel * pitch;
        if (top < B.scroll_y) B.scroll_y = (nk_uint)top;
        if (top + pitch > B.scroll_y + view) B.scroll_y = (nk_uint)(top + pitch - view);
        B.ensure_visible = false;
    }
    nk_layout_row_dynamic(ctx, list_h, 1);
    if (nk_group_scrolled_offset_begin(ctx, &B.scroll_x, &B.scroll_y, "browser files", NK_WINDOW_BORDER)) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        for (int i = 0; i < B.nshown; i++) {
            const fentry_t *e = &B.all[B.shown[i]];
            nk_layout_row_dynamic(ctx, ROW_H, 1);
            struct nk_rect rr;
            if (nk_widget(&rr, ctx) == NK_WIDGET_INVALID) continue;
            bool hover = on_top && nk_input_is_mouse_hovering_rect(in, rr);
            if (i == B.sel) nk_fill_rect(cv, rr, 2, nk_rgb(45, 70, 120));
            else if (hover) nk_fill_rect(cv, rr, 2, nk_rgb(40, 44, 52));
            char name[300], sz[32] = "";
            snprintf(name, sizeof(name), "%s%s", e->name, e->kind == FE_DIR ? "/" : "");
            fit(ctx, name, rr.w * cx[1] - cx[0] - 8);
            float y = rr.y + rr.h / 2;
            struct nk_color c = kind_color(e);
            text_at(cv, ctx, rr.x + cx[0], y, 0, name, c);
            const char *type = e->fk ? e->fk->type : e->kind == FE_DIR ? "folder" : "";
            text_at(cv, ctx, rr.x + rr.w * cx[1], y, 0, type, COL_DIMTEXT);
            if (e->kind == FE_FILE) size_text(e->size, sz, sizeof(sz));
            text_at(cv, ctx, rr.x + rr.w * cx[3] - 12, y, 1, sz, COL_DIMTEXT);
            char info[128];
            snprintf(info, sizeof(info), "%s", e->info);
            fit(ctx, info, rr.w * (1 - cx[3]) - 8);
            text_at(cv, ctx, rr.x + rr.w * cx[3], y, 0, info, e->bad ? c : COL_DIMTEXT);
            /* A click selects, a second one soon after opens. */
            if (hover && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) {
                double now = stm_sec(stm_now());
                bool twice = B.last_click_sel == i && now - B.last_click < 0.4;
                B.sel = i;
                B.last_click = twice ? 0 : now;
                B.last_click_sel = i;
                B.error[0] = 0;
                if (twice && !picked) picked = open_selected(true, run);
            }
        }
        nk_group_scrolled_end(ctx);
    }

    /* What is selected, or what went wrong. */
    const fentry_t *s = B.sel >= 0 && B.sel < B.nshown ? &B.all[B.shown[B.sel]] : NULL;
    char line[PATH_MAX + 400];
    nk_layout_row_dynamic(ctx, 20, 1);
    if (B.error[0]) {
        nk_label_colored(ctx, B.error, NK_TEXT_LEFT, nk_rgb(240, 110, 100));
    } else if (B.filter[0] == '/' || B.filter[0] == '~') {
        nk_label_colored(ctx, "Enter: go to this folder, or open this file", NK_TEXT_LEFT, COL_HOVER);
    } else if (s && s->kind == FE_FILE) {
        snprintf(line, sizeof(line), "%s/%s: %s", B.dir, s->name, s->info);
        fit_left(ctx, line, region.w - 8);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, s->bad ? kind_color(s) : COL_HOVER);
    } else {
        nk_label_colored(ctx, B.nshown == 0 && B.filter[0] ? "nothing matches" :
                         "Enter or double click: open.  Shift+Enter: load without RUN.  Esc: close.",
                         NK_TEXT_LEFT, COL_DIMTEXT);
    }

    /* The buttons, for what is selected. */
    float bw = 150;
    nk_layout_row_begin(ctx, NK_STATIC, 28, 4);
    nk_layout_row_push(ctx, region.w - 3 * (bw + 4) - 4);
    nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, bw);
    if (s && s->fk && s->fk->norun) {
        if (nk_button_label(ctx, "Load") && !picked) picked = open_selected(false, run);
    } else {
        nk_label(ctx, "", NK_TEXT_LEFT);
    }
    nk_layout_row_push(ctx, bw);
    const char *main_label = !s ? "" : s->fk ? s->fk->action : "Open the folder";
    if (s && nk_button_label(ctx, main_label) && !picked) picked = open_selected(true, run);
    else if (!s) nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, bw);
    if (nk_button_label(ctx, "Close")) B.open = false;
    nk_layout_row_end(ctx);
    nk_end(ctx);
    return picked;
}
