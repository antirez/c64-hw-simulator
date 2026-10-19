/* main.c - c64-hw-sim entry point.
 *
 *   c64-hw-sim [program.prg | cartridge.crt] [--harness] [--fault SPEC]...
 *       [--machine c64|zx48]
 *       Graphical simulator. The optional .prg is loaded and RUN after boot;
 *       a .crt cartridge is plugged into the expansion port before power on,
 *       --harness plugs the 586220 diagnostic harness.
 *
 *   c64-hw-sim --headless FRAMES [--shot out.png] [--text] [--fault SPEC]... [prg]
 *       Run without a window for FRAMES video frames (50 per second), then
 *       save what the monitor shows (--shot) and/or print the text read
 *       from the picture (--text). Handy for tests. Environment:
 *       --nets also lists the nets that had contentions, and who drives them.
 *       --fault-after N applies the faults after N frames (on a running C64).
 *       --at N:SPEC applies one fault at frame N; SPEC "repair" repairs all.
 *       --flashes counts the series of full screen flashes (how Dead Test
 *       reports a bad RAM chip), and says if the screen is lit at the end.
 *       C64HW_TYPE="text"  type this after boot (\n is RETURN)
 *       C64HW_CHECK=1      self-check oracle for the healthy board
 *
 *   --screenshot out.png [--frames N]
 *       In graphical mode: save the whole window after N frames and quit.
 *   --record N:DIR [--frames COUNT]
 *       In graphical mode: from emulated frame N, save the window every 4
 *       emulated frames (12.5 per second) as DIR/00000.png..., COUNT times,
 *       then quit. Made to turn into a video. --at works here too.
 *
 *   For screenshots and tests of the interface (graphical mode):
 *   --inspect REF       open the chip inspector on REF ("U8")
 *   --view X,Y,ZOOM     center the board on X,Y mm, ZOOM times the full view
 *   --mouse X,Y         keep the mouse at X,Y (window pixels): hovering
 *   --down X,Y@F, --up X,Y@F   press / release the left button at UI frame F
 *   --key NAME@F        press a key at UI frame F: up down pgup pgdn home
 *                       end enter shift-enter esc backspace ctrl-o
 *   --chars TEXT@F      type TEXT at UI frame F
 *   --browse            open the file browser
 *
 * Fault SPEC examples: U17:removed  U17.18:gnd  U21.14:vcc  U9.2:open
 *                      CN6.B:open  R51:open  RP4:short  RESET:pressed */
#include "sokol_headers.h"
#include "board.h"
#include "c64.h"
#include "zx48.h"
#include "png.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static struct {
    const char *prg;
    const char *crt;
    bool harness, flashes;
    const char *shot;
    const char *screenshot;
    int headless_frames;
    int frames;
    const char *faults[64];
    int nfaults;
    const char *scope;
    bool text, nets;
    int fault_after;        /* apply the faults after this many frames */
    struct { int frame; const char *spec; } at[64];   /* --at FRAME:SPEC */
    int nat;
    const char *record;     /* --record N:DIR */
    int record_from;
    const char *inspect;
    bool view, mouse;
    float view_x, view_y, view_zoom, mouse_x, mouse_y;
    struct { int frame; bool down; float x, y; } clicks[16];
    int nclicks;
    struct { int frame; char what[1024]; bool chars; size_t sent; } keys[16];
    int nkeys;
    bool browse;
    const machine_t *machine;
} args = { .frames = 60 };

static board_t *board;

/* The machines, for --machine. */
static const machine_t *const machines[] = { &machine_c64, &machine_zx48 };

static void apply_faults(void) {
    for (int i = 0; i < args.nfaults; i++)
        if (!board_apply_fault_spec(board, args.faults[i]))
            fprintf(stderr, "bad fault spec: %s\n", args.faults[i]);
}

/* Plug the cartridge given on the command line. */
static void insert_cart(void) {
    char msg[256];
    if (args.harness) board_set_harness(board, true);
    if (!args.crt) return;
    if (!board_load(board, args.crt, false, msg, sizeof(msg))) fprintf(stderr, "%s\n", msg);
    else printf("%s\n", msg);
}

/* --flashes: Dead Test reports a bad RAM chip by flashing the whole
 * screen N times, then a pause, then again. A lit screen for less than a
 * second is a flash; a dark pause longer than 0.8 s starts a new series. */
static struct { bool lit; int lit_since, dark_since, cur, n, series[64]; } F;

static bool screen_lit(void) {
    int w = board->m->screen_w, h = board->m->screen_h;
    uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
    board_monitor(board, px);
    uint32_t c = px[(h / 2) * w + w / 2];
    free(px);
    return ((c & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + ((c >> 16) & 0xFF) * 114) / 1000 > 128;
}

static void flash_frame(int f) {
    bool lit = screen_lit();
    if (lit && !F.lit) F.lit_since = f;
    if (!lit && F.lit) {
        if (f - F.lit_since < 50) {
            if (F.cur && F.lit_since - F.dark_since > 40 && F.n < 64) { F.series[F.n++] = F.cur; F.cur = 0; }
            F.cur++;
        }
        F.dark_since = f;
    }
    F.lit = lit;
}

static void flash_report(void) {
    if (F.cur && F.n < 64) F.series[F.n++] = F.cur;
    printf("flash series:");
    for (int i = 0; i < F.n; i++) printf(" %d", F.series[i]);
    printf("%s\nscreen at the end: %s\n", F.n ? "" : " none", screen_lit() ? "lit" : "dark");
}

static int run_headless(void) {
    char msg[256];
    insert_cart();
    if (!args.fault_after) apply_faults();
    clock_t t0 = clock();
    const char *type = getenv("C64HW_TYPE");     /* text to type after boot */
    int typed = 0, next_type = 0;
    const machine_t *m = board->m;
    int ready = (int)(m->ready_cycles / (uint64_t)m->cycles_per_frame);
    for (int f = 0; f < args.headless_frames; f++) {
        if (args.fault_after && f == args.fault_after) apply_faults();
        for (int i = 0; i < args.nat; i++) {        /* scheduled faults/repairs */
            if (args.at[i].frame != f) continue;
            if (!strcmp(args.at[i].spec, "repair")) board_clear_faults(board);
            else if (!board_apply_fault_spec(board, args.at[i].spec))
                fprintf(stderr, "bad fault spec: %s\n", args.at[i].spec);
        }
        board_run(board, m->cycles_per_frame);
        board_frame_update(board, 20000);
        if (args.flashes) flash_frame(f);
        /* Once BASIC is ready: load the program and type. */
        if (args.prg && f == ready) {
            board_load(board, args.prg, true, msg, sizeof(msg));
            printf("%s\n", msg);
        }
        /* A key every 4 frames; the same key again after 12, as the
         * Spectrum's ROM takes a key pressed again within 5 frames of its
         * release for the same key held down. */
        if (type && f >= ready + 10 && f >= next_type && f % 4 == 0 && type[typed]) {
            int c = type[typed++];
            next_type = f + (type[typed] == c ? 12 : 4);
            if (c == '\n') c = m->host_key(SAPP_KEYCODE_ENTER);
            else if (c >= 'a' && c <= 'z') c = m->host_char(c);
            board_key_down(board, c);
            board_key_up(board, c);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double emulated = args.headless_frames * (double)m->cycles_per_frame / m->cpu_hz;
    printf("emulated %.2f s in %.2f s (%.0f%% of real time)\n", emulated, secs, 100.0 * emulated / secs);
    char regs[128];
    m->cpu_regs(board, regs, sizeof(regs));
    printf("CPU: %s\n", regs);
    if (board->check) printf("self-check errors: %llu\n", (unsigned long long)board->check_errors);
    if (args.flashes) flash_report();
    if (args.text) m->print_text(board);
    if (args.nets) {    /* nets that spent time in contention, and who fights */
        sim_t *s = &board->sim;
        for (int i = 0; i < s->nnets; i++) {
            uint32_t tot = board->st_hi[i] + board->st_lo[i] + board->st_float[i] + board->st_fight[i];
            if (!board->st_fight[i] || s->nets[i].root != i) continue;
            printf("contention %3u%% on net %s:", (unsigned)(100ull * board->st_fight[i] / tot), s->nets[i].name);
            const net_t *n = &s->nets[i];
            for (int k = 0; k < n->npins; k++) {
                const pin_t *pp = &s->pins[n->pins[k]];
                const char *pname = sim_pin_name(s, n->pins[k]);
                if (pp->eff) printf(" %s.%d(%s)=%s%d", s->parts[pp->part].ref, pp->number, pname ? pname : "",
                                    (pp->eff >> 1) == S_SUPPLY ? "supply" : (pp->eff >> 1) == S_STRONG ? "" :
                                    (pp->eff >> 1) == S_RES ? "res" : "weak",
                                    pp->eff & 1);
            }
            printf("\n");
        }
    }
    if (args.shot) {
        uint32_t *pixels = malloc((size_t)m->screen_w * m->screen_h * sizeof(uint32_t));
        board_monitor(board, pixels);
        png_write(args.shot, pixels, m->screen_w, m->screen_h);
        free(pixels);
        printf("screen saved to %s\n", args.shot);
    }
    return 0;
}

static void init(void) {
    sg_setup(&(sg_desc){ .environment = sglue_environment(), .logger.func = slog_func });
    stm_setup();
    snk_setup(&(snk_desc_t){ .dpi_scale = sapp_dpi_scale(), .max_vertices = 400000,
                             .logger.func = slog_func });
    insert_cart();
    apply_faults();
    ui_init(board, args.prg, args.scope);
    if (args.inspect) ui_inspect(args.inspect);
    if (args.browse) ui_browse();
    if (args.view) ui_view(args.view_x, args.view_y, args.view_zoom);
}

/* A synthetic mouse event, as if from the window (tests, screenshots). */
static void mouse_event(sapp_event_type type, float x, float y) {
    sapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.mouse_button = SAPP_MOUSEBUTTON_LEFT;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ui_event(&ev);
}

static void frame(void) {
    static int count, recorded, next_record = -1, ui_frames;
    static bool done[64];
    if (args.mouse) mouse_event(SAPP_EVENTTYPE_MOUSE_MOVE, args.mouse_x, args.mouse_y);
    for (int i = 0; i < args.nclicks; i++) {
        if (args.clicks[i].frame != ui_frames) continue;
        mouse_event(SAPP_EVENTTYPE_MOUSE_MOVE, args.clicks[i].x, args.clicks[i].y);
        mouse_event(args.clicks[i].down ? SAPP_EVENTTYPE_MOUSE_DOWN : SAPP_EVENTTYPE_MOUSE_UP,
                    args.clicks[i].x, args.clicks[i].y);
    }
    for (int i = 0; i < args.nkeys; i++) {
        sapp_event ev;
        memset(&ev, 0, sizeof(ev));
        if (args.keys[i].chars) {
            /* nuklear takes 16 bytes of text a frame: the rest goes in the
             * next frames, as if typed. */
            const char *what = args.keys[i].what;
            if (args.keys[i].frame > ui_frames || !what[args.keys[i].sent]) continue;
            for (int n = 0; n < 8 && what[args.keys[i].sent]; n++) {
                ev.type = SAPP_EVENTTYPE_CHAR;
                ev.char_code = (uint32_t)what[args.keys[i].sent++];
                ui_event(&ev);
            }
            continue;
        }
        if (args.keys[i].frame != ui_frames) continue;
        static const struct { const char *name; sapp_keycode code; uint32_t mod; } names[] = {
            { "up", SAPP_KEYCODE_UP, 0 }, { "down", SAPP_KEYCODE_DOWN, 0 },
            { "pgup", SAPP_KEYCODE_PAGE_UP, 0 }, { "pgdn", SAPP_KEYCODE_PAGE_DOWN, 0 },
            { "home", SAPP_KEYCODE_HOME, 0 }, { "end", SAPP_KEYCODE_END, 0 },
            { "enter", SAPP_KEYCODE_ENTER, 0 }, { "shift-enter", SAPP_KEYCODE_ENTER, SAPP_MODIFIER_SHIFT },
            { "esc", SAPP_KEYCODE_ESCAPE, 0 }, { "backspace", SAPP_KEYCODE_BACKSPACE, 0 },
            { "ctrl-o", SAPP_KEYCODE_O, SAPP_MODIFIER_CTRL },
        };
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            if (strcmp(names[k].name, args.keys[i].what)) continue;
            ev.key_code = names[k].code;
            ev.modifiers = names[k].mod;
            ev.type = SAPP_EVENTTYPE_KEY_DOWN;
            ui_event(&ev);
            ev.type = SAPP_EVENTTYPE_KEY_UP;
            ui_event(&ev);
        }
    }
    ui_frames++;
    ui_frame();
    int f = ui_emulated_frame();
    for (int i = 0; i < args.nat; i++)          /* scheduled faults/repairs */
        if (!done[i] && f >= args.at[i].frame) { ui_apply_fault(args.at[i].spec); done[i] = true; }
    if (args.screenshot && ++count >= args.frames) {
        ui_save_screenshot(args.screenshot);
        sapp_request_quit();
    }
    if (args.record && f >= args.record_from && f >= next_record) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%05d.png", args.record, recorded++);
        ui_save_screenshot(path);
        next_record = (next_record < 0 ? f : next_record) + 4;
        if (recorded >= args.frames) sapp_request_quit();
    }
}

static void cleanup(void) {
    ui_shutdown();
    snk_shutdown();
    sg_shutdown();
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) args.headless_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) args.shot = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) args.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) args.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scope") && i + 1 < argc) args.scope = argv[++i];
        else if (!strcmp(argv[i], "--fault-after") && i + 1 < argc) args.fault_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--at") && i + 1 < argc && args.nat < 64) {
            const char *a = argv[++i], *colon = strchr(a, ':');
            if (colon) { args.at[args.nat].frame = atoi(a); args.at[args.nat++].spec = colon + 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            const char *a = argv[++i], *colon = strchr(a, ':');
            args.record_from = colon ? atoi(a) : 0;
            args.record = colon ? colon + 1 : a;
        }
        else if (!strcmp(argv[i], "--inspect") && i + 1 < argc) args.inspect = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc)
            args.view = sscanf(argv[++i], "%f,%f,%f", &args.view_x, &args.view_y, &args.view_zoom) == 3;
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc)
            args.mouse = sscanf(argv[++i], "%f,%f", &args.mouse_x, &args.mouse_y) == 2;
        else if ((!strcmp(argv[i], "--down") || !strcmp(argv[i], "--up")) && i + 1 < argc && args.nclicks < 16) {
            bool down = !strcmp(argv[i], "--down");
            if (sscanf(argv[++i], "%f,%f@%d", &args.clicks[args.nclicks].x, &args.clicks[args.nclicks].y,
                       &args.clicks[args.nclicks].frame) == 3)
                args.clicks[args.nclicks++].down = down;
        }
        else if (!strcmp(argv[i], "--harness")) args.harness = true;
        else if (!strcmp(argv[i], "--browse")) args.browse = true;
        else if ((!strcmp(argv[i], "--key") || !strcmp(argv[i], "--chars")) && i + 1 < argc && args.nkeys < 16) {
            bool chars = !strcmp(argv[i], "--chars");
            const char *a = argv[++i], *at = strrchr(a, '@');
            if (at) {
                snprintf(args.keys[args.nkeys].what, sizeof(args.keys[0].what), "%.*s", (int)(at - a), a);
                args.keys[args.nkeys].frame = atoi(at + 1);
                args.keys[args.nkeys++].chars = chars;
            }
        }
        else if (!strcmp(argv[i], "--machine") && i + 1 < argc) {
            const char *id = argv[++i];
            args.machine = NULL;
            for (size_t k = 0; k < sizeof(machines) / sizeof(machines[0]); k++)
                if (!strcmp(machines[k]->id, id)) args.machine = machines[k];
            if (!args.machine) { fprintf(stderr, "unknown machine %s\n", id); return 1; }
        }
        else if (!strcmp(argv[i], "--flashes")) args.flashes = true;
        else if (!strcmp(argv[i], "--text")) args.text = true;
        else if (!strcmp(argv[i], "--nets")) args.nets = true;
        else if (!strcmp(argv[i], "--fault") && i + 1 < argc && args.nfaults < 64) args.faults[args.nfaults++] = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
        else {
            /* Cartridges go in before power on, the rest is loaded once the
             * machine is ready. Spectrum files pick the Spectrum. */
            size_t n = strlen(argv[i]);
            const char *ext = n > 4 ? argv[i] + n - 4 : "";
            if (!strcasecmp(ext, ".crt") || !strcasecmp(ext, ".rom")) args.crt = argv[i];
            else args.prg = argv[i];
            if (!args.machine && (!strcasecmp(ext, ".rom") || !strcasecmp(ext, ".sna") || !strcasecmp(ext, ".z80")))
                args.machine = &machine_zx48;
        }
    }
    board = board_create(args.machine ? args.machine : &machine_c64);
    if (args.headless_frames > 0) return run_headless();
    sapp_run(&(sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = ui_event,
        .width = 1600,
        .height = 1000,
        .window_title = "c64-hw-sim - C64 ASSY 250407",
        .enable_dragndrop = true,
        .enable_clipboard = true,     /* paste a path in the file browser */
        .high_dpi = false,
        .logger.func = slog_func,
    });
    return 0;
}
