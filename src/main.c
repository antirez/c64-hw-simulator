/* main.c - c64-hw-sim entry point.
 *
 *   c64-hw-sim [program.prg] [--fault SPEC]...
 *       Graphical simulator. The optional .prg is loaded and RUN after boot.
 *
 *   c64-hw-sim --headless FRAMES [--shot out.png] [--text] [--fault SPEC]... [prg]
 *       Run without a window for FRAMES video frames (50 per second), then
 *       save what the monitor shows (--shot) and/or print the text read
 *       from the picture (--text). Handy for tests. Environment:
 *       --nets also lists the nets that had contentions, and who drives them.
 *       --fault-after N applies the faults after N frames (on a running C64).
 *       --at N:SPEC applies one fault at frame N; SPEC "repair" repairs all.
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
 * Fault SPEC examples: U17:removed  U17.18:gnd  U21.14:vcc  U9.2:open
 *                      R51:open  RP4:short */
#include "sokol_headers.h"
#include "board.h"
#include "png.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    const char *prg;
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
} args = { .frames = 60 };

static board_t *board;

static void apply_faults(void) {
    for (int i = 0; i < args.nfaults; i++)
        if (!board_apply_fault_spec(board, args.faults[i]))
            fprintf(stderr, "bad fault spec: %s\n", args.faults[i]);
}

/* Read the text on the monitor picture, like a person would: match every
 * 8x8 cell of the text area against the character ROM glyphs. Cells that
 * match no glyph are shown as '?'. This looks at the PICTURE, not at the
 * screen memory, so it shows what a broken machine really displays. */
static int match_cell(const uint32_t *px, int x0, int y0, uint32_t bg) {
    extern unsigned char dump_c64_char_bin[4096];
    uint8_t cell[8];
    for (int y = 0; y < 8; y++) {
        cell[y] = 0;
        for (int x = 0; x < 8; x++)
            if (px[(y0 + y) * SCREEN_W + x0 + x] != bg) cell[y] |= (uint8_t)(0x80 >> x);
    }
    for (int c = 0; c < 128; c++) {
        bool same = true, inv = true;
        for (int y = 0; y < 8; y++) {
            if (dump_c64_char_bin[c * 8 + y] != cell[y]) same = false;
            if ((uint8_t)~dump_c64_char_bin[c * 8 + y] != cell[y]) inv = false;
        }
        if (same || inv) return c;
    }
    return -1;
}

static void monitor_text(void) {
    static uint32_t px[SCREEN_W * SCREEN_H];
    board_monitor(board, px);
    /* The text window starts where the border ends (32,36 on a healthy
     * machine); if there is no border, assume the normal position. */
    uint32_t border = px[2 * SCREEN_W + 2];
    int ox = 32, oy = 36;
    for (int y = 20; y < 60; y++) if (px[y * SCREEN_W + SCREEN_W / 2] != border) { oy = y; break; }
    for (int x = 16; x < 60; x++) if (px[(oy + 100) * SCREEN_W + x] != border) { ox = x; break; }
    uint32_t bg = px[(oy + 100) * SCREEN_W + ox + 4];
    for (int row = 0; row < 25; row++) {
        char line[41];
        for (int col = 0; col < 40; col++) {
            int code = match_cell(px, ox + col * 8, oy + row * 8, bg);
            char ch = '?';
            if (code == 0) ch = '@';
            else if (code >= 1 && code <= 26) ch = (char)('A' + code - 1);
            else if (code >= 27 && code < 64) ch = "[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?"[code - 27];
            else if (code >= 64) ch = '#';
            line[col] = ch;
        }
        line[40] = 0;
        int end = 40;
        while (end > 0 && line[end - 1] == ' ') line[--end] = 0;
        printf("|%s\n", line);
    }
}

static int run_headless(void) {
    char msg[256];
    if (!args.fault_after) apply_faults();
    clock_t t0 = clock();
    const char *type = getenv("C64HW_TYPE");     /* text to type after boot */
    int typed = 0;
    for (int f = 0; f < args.headless_frames; f++) {
        if (args.fault_after && f == args.fault_after) apply_faults();
        for (int i = 0; i < args.nat; i++) {        /* scheduled faults/repairs */
            if (args.at[i].frame != f) continue;
            if (!strcmp(args.at[i].spec, "repair")) board_clear_faults(board);
            else if (!board_apply_fault_spec(board, args.at[i].spec))
                fprintf(stderr, "bad fault spec: %s\n", args.at[i].spec);
        }
        board_run(board, CYCLES_PER_FRAME);
        board_frame_update(board, 20000);
        /* After 3 seconds BASIC is ready: load the program and type. */
        if (args.prg && f == 150) {
            board_load_prg(board, args.prg, true, msg, sizeof(msg));
            printf("%s\n", msg);
        }
        if (type && f >= 160 && f % 4 == 0 && type[typed]) {
            int c = type[typed++];
            if (c == '\n') c = C64_KEY_RETURN;
            else if (c >= 'a' && c <= 'z') c -= 32;
            board_key_down(board, c);
            board_key_up(board, c);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double emulated = args.headless_frames * (double)CYCLES_PER_FRAME / C64_FREQ;
    printf("emulated %.2f s in %.2f s (%.0f%% of real time)\n", emulated, secs, 100.0 * emulated / secs);
    printf("CPU: PC=%04X A=%02X X=%02X Y=%02X S=%02X %s\n", m6502_pc(&board->cpu), m6502_a(&board->cpu),
           m6502_x(&board->cpu), m6502_y(&board->cpu), m6502_s(&board->cpu),
           !board->cpu_running ? "(no clock)" : board->cpu_in_reset ? "(in reset)" : "");
    if (board->check) printf("self-check errors: %llu\n", (unsigned long long)board->check_errors);
    if (args.text) monitor_text();
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
                                    (pp->eff >> 1) == S_SUPPLY ? "supply" : (pp->eff >> 1) == S_STRONG ? "" : "weak",
                                    pp->eff & 1);
            }
            printf("\n");
        }
    }
    if (args.shot) {
        static uint32_t pixels[SCREEN_W * SCREEN_H];
        board_monitor(board, pixels);
        png_write(args.shot, pixels, SCREEN_W, SCREEN_H);
        printf("screen saved to %s\n", args.shot);
    }
    return 0;
}

static void init(void) {
    sg_setup(&(sg_desc){ .environment = sglue_environment(), .logger.func = slog_func });
    stm_setup();
    snk_setup(&(snk_desc_t){ .dpi_scale = sapp_dpi_scale(), .max_vertices = 400000,
                             .logger.func = slog_func });
    apply_faults();
    ui_init(board, args.prg, args.scope);
}

static void frame(void) {
    static int count, recorded, next_record = -1;
    static bool done[64];
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
        else if (!strcmp(argv[i], "--text")) args.text = true;
        else if (!strcmp(argv[i], "--nets")) args.nets = true;
        else if (!strcmp(argv[i], "--fault") && i + 1 < argc && args.nfaults < 64) args.faults[args.nfaults++] = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
        else args.prg = argv[i];
    }
    board = board_create();
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
        .high_dpi = false,
        .logger.func = slog_func,
    });
    return 0;
}
