/* ulacheck.c - check the ULA model of c64-hw-sim (src/ula.c) against a
 * gate level simulation of the ULA 6C001 netlist recovered from the die by
 * the emu-russia/ula project (github.com/emu-russia/ula, CC0: the netlist
 * is netlist/ula6c001.v there; it is not part of this repository).
 *
 *   cc -O2 -o ulacheck tools/ulacheck.c
 *   ZXHW_ULALOG=ula.log ./c64-hw-sim --machine zx48 --headless 10
 *   ./ulacheck ula6c001.v ula.log [H]
 *
 * The simulator writes, for every half period of the 14 MHz oscillator,
 * the ULA's counters, the levels it read on its input pins and what it
 * drove. This program puts the same levels on the inputs of the gates,
 * each gate taking one unit of delay (H units per half period, default
 * 10: a gate delay of about 3.6 ns), and compares the outputs at the end
 * of every half period: /RAS, /CAS, the address pins and their enable, the
 * CPU clock, /INT, the data pins pulled low by a read of port 0xFE. Then
 * the picture: the colour of every pixel, once the delay of the gates'
 * pixel pipeline is found. The border latch powers on with any value, so
 * border pixels count only after the first write to the port in the log.
 *
 * The model is simpler than the gates in three documented ways (see the
 * top of src/ula.c), counted apart: the address pins switch from row to
 * column half a period later (after /RAS fell, before /CAS falls: the same
 * for the DRAM); a one gate delay /RAS glitch between the bitmap and the
 * attribute fetch is not there; and when the CPU ends a port read, the
 * model lets the data pins go in the same half period.
 *
 * To look closer: ULACHECK_ONLY=k shows only the differences of kind k
 * (0 /RAS ... 6 port read), ULACHECK_PIXELS=n the first n different
 * pixels, ULACHECK_LINE=v line v of the last frame (ULACHECK_RAW=1: every
 * half period), ULACHECK_PEEK=step the gates' latches around that step and
 * ULACHECK_NETS=w1,w2... more nets there. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2000
#define MAXG 800

static char *net_name[MAXN];
static int nnets;
static uint8_t val[MAXN];
typedef struct { int in[8], nin, out; } gate_t;     /* NOR (NOT is a one input NOR) */
static gate_t gates[MAXG];
static int ngates;
static int *fanout[MAXN], nfan[MAXN];
static int dirty[MAXG], ndirty;
static uint8_t gdirty[MAXG];

static int net(const char *name) {
    for (int i = 0; i < nnets; i++) if (!strcmp(net_name[i], name)) return i;
    net_name[nnets] = strdup(name);
    return nnets++;
}

static void parse(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line, type[64];
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "ula_", 4) || sscanf(p, "ula_%63s", type) != 1) continue;
        if (strcmp(type, "not") && strncmp(type, "nor", 3)) continue;
        char *paren = strchr(p, '(');
        if (!paren) continue;
        gate_t *g = &gates[ngates++];
        for (char *q = paren + 1; (q = strchr(q, '.')); q++) {
            char pn[32], pw[32];
            if (sscanf(q, ".%31[^(](%31[^)])", pn, pw) != 2) continue;
            if (!strcmp(pn, "x")) g->out = net(pw);
            else g->in[g->nin++] = net(pw);
        }
    }
    fclose(f);
    for (int i = 0; i < ngates; i++)
        for (int k = 0; k < gates[i].nin; k++) {
            int n = gates[i].in[k];
            fanout[n] = realloc(fanout[n], (nfan[n] + 1) * sizeof(int));
            fanout[n][nfan[n]++] = i;
        }
}

static void set_net(int n, int v) {
    if (val[n] == v) return;
    val[n] = (uint8_t)v;
    for (int k = 0; k < nfan[n]; k++) {
        int g = fanout[n][k];
        if (!gdirty[g]) { gdirty[g] = 1; dirty[ndirty++] = g; }
    }
}

/* The nets of the pads (netlist/ula6c001.v, the ula_pad_* cells). */
static int N_OSC, N_MREQ, N_IOREQ, N_RD, N_WR, N_A14, N_A15, N_A0IN, N_A0OUT, N_NAE;
static int N_EAR, N_RAS, N_RASOE, N_CAS, N_CPUCLK, N_INT, N_DIN[8], N_DOUT[8], N_KB[5], N_KB0OUT, N_A[7];
static int N_C[9], N_V[9];
static int N_RED, N_GREEN, N_BLUE, N_HL, N_NHBLANK, N_VSYNC;
#define MAXPIX (6000000LL)
static uint8_t model_pix[MAXPIX / 4 + 1], gate_pix[MAXPIX];
static uint16_t model_c[MAXPIX / 4 + 1], model_v[MAXPIX / 4 + 1];

/* What the outside world puts on the pads (pin levels, as logged). */
static uint32_t inputs = 0x1FFF7F;
static int osc;

static void apply_inputs(void) {
    set_net(N_OSC, osc);
    set_net(N_MREQ, inputs & 1);
    set_net(N_IOREQ, (inputs >> 1) & 1);
    set_net(N_RD, (inputs >> 2) & 1);
    set_net(N_WR, (inputs >> 3) & 1);
    set_net(N_A14, (inputs >> 4) & 1);
    set_net(N_A15, (inputs >> 5) & 1);
    set_net(N_EAR, (inputs >> 7) & 1);     /* the comparator on the EAR/MIC/speaker pin */
    /* A0: when the ULA drives the pad it reads its own output. */
    set_net(N_A0IN, val[N_NAE] ? (inputs >> 6) & 1 : val[N_A[0]]);
    for (int b = 0; b < 8; b++) {
        int v = (inputs >> (8 + b)) & 1;
        if (N_DOUT[b] >= 0 && !val[N_DOUT[b]]) v = 0;      /* open collector pulling low */
        set_net(N_DIN[b], v);
    }
    for (int k = 0; k < 5; k++) {
        int v = (inputs >> (16 + k)) & 1;
        /* KB0 is pulled low in the test mode only (K0 high): the pad cell
         * of the netlist has it the other way round, which would hold the
         * first keyboard line low all the time. */
        if (k == 0 && val[N_KB0OUT]) v = 0;
        set_net(N_KB[k], v);
    }
}

/* One gate delay: every gate takes one unit, all at once. Two cross-
 * coupled gates (the core of a latch) hit by a glitch shorter than they
 * can follow would then oscillate in lockstep forever, which real gates
 * don't: a pulse too short to flip a latch leaves it as it was. So a gate
 * that keeps flipping within one half period goes back to the value it had
 * when the half period began, and stays there until the next one, when
 * it is evaluated again. */
static uint8_t flips[MAXG], frozen[MAXG], start_val[MAXG];

static void gate_step(void) {
    static int cur[MAXG], chn[MAXG], chg[MAXG];
    static uint8_t chv[MAXG];
    int nd = ndirty, nch = 0;
    memcpy(cur, dirty, nd * sizeof(int));
    ndirty = 0;
    for (int i = 0; i < nd; i++) gdirty[cur[i]] = 0;
    for (int i = 0; i < nd; i++) {
        int gi = cur[i];
        if (frozen[gi]) continue;
        gate_t *g = &gates[gi];
        int any = 0;
        for (int k = 0; k < g->nin; k++) any |= val[g->in[k]];
        if (val[g->out] != !any) { chn[nch] = g->out; chv[nch] = (uint8_t)!any; chg[nch] = gi; nch++; }
    }
    for (int i = 0; i < nch; i++) {
        int gi = chg[i];
        if (++flips[gi] > 6) {      /* oscillating: give up the glitch */
            frozen[gi] = 1;
            set_net(chn[i], start_val[gi]);
            continue;
        }
        set_net(chn[i], chv[i]);
    }
    apply_inputs();
}

static int H = 10;
static long long half_count;

static int watch_units;      /* print unit level changes of a few nets */

/* One half period of the oscillator. */
static void half(void) {
    osc = (int)(half_count & 1);
    apply_inputs();
    for (int i = 0; i < ngates; i++) {
        /* A gate frozen in the last half period looks at its inputs again. */
        if (frozen[i] && !gdirty[i]) { gdirty[i] = 1; dirty[ndirty++] = i; }
        flips[i] = 0; frozen[i] = 0; start_val[i] = val[gates[i].out];
    }
    static const char *wn[] = { "w340", "w590", "w506", "w418", "w339", "w338", "w72", "w208" };
    int before[8];
    for (int i = 0; i < 8; i++) before[i] = val[net(wn[i])];
    for (int k = 0; k < H; k++) {
        gate_step();
        if (!watch_units) continue;
        for (int i = 0; i < 8; i++) {
            int v = val[net(wn[i])];
            if (v != before[i]) printf("   unit %d: %s=%d\n", k, wn[i], v);
            before[i] = v;
        }
    }
    half_count++;
}

static int counter(const int *n, int bits) {
    int v = 0;
    for (int i = 0; i < bits; i++) v |= val[n[i]] << i;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ulacheck ula6c001.v ula.log [H]\n"); return 1; }
    parse(argv[1]);
    if (argc > 3) H = atoi(argv[3]);
    N_OSC = net("w440"); N_MREQ = net("w314"); N_IOREQ = net("w259"); N_RD = net("w309"); N_WR = net("w244");
    N_A15 = net("w358"); N_A14 = net("w407"); N_A0IN = net("w310"); N_A0OUT = net("w173"); N_NAE = net("w667");
    N_EAR = net("w513");
    N_RAS = net("w439"); N_RASOE = net("w438"); N_CAS = net("w421"); N_CPUCLK = net("w415"); N_INT = net("w117");
    static const char *din[8] = { "w7", "w478", "w512", "w608", "w522", "w416", "w516", "w417" };
    static const char *dout[8] = { "w10", "w6", "w644", "w582", "w536", NULL, "w515", NULL };
    for (int b = 0; b < 8; b++) { N_DIN[b] = net(din[b]); N_DOUT[b] = dout[b] ? net(dout[b]) : -1; }
    static const char *kb[5] = { "w13", "w9", "w645", "w581", "w647" };
    for (int k = 0; k < 5; k++) N_KB[k] = net(kb[k]);
    N_KB0OUT = net("w276");
    static const char *aw[7] = { "w173", "w191", "w327", "w323", "w322", "w274", "w275" };
    for (int i = 0; i < 7; i++) N_A[i] = net(aw[i]);
    static const char *cw[9] = { "w338", "w72", "w208", "w253", "w113", "w112", "w17", "w16", "w31" };
    static const char *vw[9] = { "w25", "w86", "w91", "w279", "w187", "w179", "w178", "w272", "w261" };
    for (int i = 0; i < 9; i++) { N_C[i] = net(cw[i]); N_V[i] = net(vw[i]); }
    N_RED = net("w51"); N_GREEN = net("w56"); N_BLUE = net("w18"); N_HL = net("w5");      /* nHL, the DAC input */
    N_NHBLANK = net("w67"); N_VSYNC = net("w30");
    fprintf(stderr, "%d gates, %d nets\n", ngates, nnets);

    /* Power on: settle from all zeros one gate at a time in a random order
     * (no real chip switches all its gates at once), then unit delays. */
    apply_inputs();
    srand(1);
    for (int iter = 0; iter < 2000; iter++) {
        int changes = 0;
        for (int k = 0; k < ngates; k++) {
            gate_t *g = &gates[rand() % ngates];
            int any = 0;
            for (int j = 0; j < g->nin; j++) any |= val[g->in[j]];
            if (val[g->out] != !any) { val[g->out] = (uint8_t)!any; changes++; }
        }
        apply_inputs();
        if (!changes && iter > 10) break;
    }
    for (int i = 0; i < ngates; i++) { gdirty[i] = 1; dirty[ndirty++] = i; }

    FILE *lf = fopen(argv[2], "r");
    if (!lf) { perror(argv[2]); return 1; }
    char line[256];
    if (!fgets(line, sizeof(line), lf)) return 1;
    unsigned long long step;
    unsigned c0, v0, h0;
    if (sscanf(line, "%llu %u %u %u", &step, &c0, &v0, &h0) != 4) { fprintf(stderr, "bad log\n"); return 1; }

    /* Sync: find a tick boundary where only C0 changes (no ripple), so the
     * half periods of each tick are known; then wait for the tick and half
     * of the first line of the log. */
    for (int i = 0; i < 4000; i++) half();
    int prev = counter(N_C, 9), phase0 = -1;
    for (int i = 0; i < 4000 && phase0 < 0; i++) {
        half();
        int c = counter(N_C, 9);
        if ((prev & 1) == 0 && c == prev + 1) phase0 = (int)((half_count - 1) & 3);
        prev = c;
    }
    if (phase0 < 0) { fprintf(stderr, "can't find the ticks of the gates\n"); return 1; }
    /* Find the tick before the first logged one, its counters read two
     * halves into it (settled): then its last half, and the halves of the
     * first logged tick before the first logged one. */
    unsigned pc = c0 ? c0 - 1 : 447, pv = c0 ? v0 : (v0 ? v0 - 1 : 311);
    long long guard = 0;
    for (;;) {
        int h = (int)((half_count - phase0) & 3);
        half();
        if (h == 2 && (unsigned)counter(N_C, 9) == pc && (unsigned)counter(N_V, 9) == pv) break;
        if (++guard > 448LL * 312 * 4 * 2) { fprintf(stderr, "can't sync to C=%u V=%u\n", pc, pv); return 1; }
    }
    half();
    for (unsigned k = 0; k < h0; k++) half();

    long long lines = 0, bad[8] = {0}, addr_late = 0, ras_glitch = 0, port_end = 0, shown = 0;
    /* The border latch powers on with whatever value: its pixels are only
     * compared once the first write to the port in the log has gone
     * through the output latch (at C mod 8 = 5) to the screen (from the
     * next C mod 8 = 0). */
    int border_known = 0;
    int prev_clk = 1;
    const char *only = getenv("ULACHECK_ONLY");
    int show_kind = only ? atoi(only) : -1;
    static const char *what[8] = { "/RAS", "/CAS", "address", "CPU clock", "/INT", "nAE", "port read" };
    do {
        unsigned long long st;
        unsigned c, v, h, in;
        int ras, cas, ae, a, clk, irq, ccas;
        unsigned dlow, pix;
        if (sscanf(line, "%llu %u %u %u in=%x ras=%d cas=%d ae=%d a=%x clk=%d int=%d ccas=%d dlow=%x pix=%x", &st, &c,
                   &v, &h, &in, &ras, &cas, &ae, &a, &clk, &irq, &ccas, &dlow, &pix) != 14) continue;
        inputs = in;
        static long long peek = -2;
        if (peek == -2) { const char *pk = getenv("ULACHECK_PEEK"); peek = pk ? atoll(pk) : -1; }
        watch_units = peek >= 0 && (long long)st >= peek - 2 && (long long)st <= peek + 2;
        if (watch_units) printf("half for step %llu C=%u.%u:\n", st, c, h);
        half();
        /* ULACHECK_PEEK=step: the gates' latches around that step. */
        if (peek >= 0 && (long long)st >= peek - 8 && (long long)st <= peek + 64) {
            static const char *ndl[8] = { "w479", "w532", "w596", "w593", "w524", "w632", "w501", "w517" };
            static const char *al[8] = { "w506", "w585", "w575", "w491", "w527", "w494", "w550", "w631" };
            int d = 0, at = 0, din = 0;
            for (int b = 0; b < 8; b++) {
                d |= !val[net(ndl[b])] << b; at |= val[net(al[b])] << b; din |= val[N_DIN[b]] << b;
            }
            static const char *ao[8] = { "w590", "w624", "w589", "w558", "w579", "w564", "w14", "w198" };
            int aov = 0;
            for (int b = 0; b < 8; b++) aov |= val[net(ao[b])] << b;
            printf("peek step %llu C=%u.%u D=%02X data=%02X attr=%02X nDL=%d nAL=%d CAS=%d AO=%02X nAO=%d VidEn=%d "
                   "nBorder=%d nHBlank=%d VSync=%d nDataSel=%d RGB=%d%d%d nHL=%d\n", st, c, h, din, d, at,
                   val[net("w447")], val[net("w418")], val[N_CAS], aov, val[net("w340")], val[net("w351")],
                   val[net("w260")], val[N_NHBLANK], val[N_VSYNC], val[net("w66")], val[N_RED], val[N_GREEN],
                   val[N_BLUE], val[N_HL]);
            /* ULACHECK_NETS=w1,w2,...: and these nets. */
            const char *nl = getenv("ULACHECK_NETS");
            if (nl) {
                char buf[512], *tok, *save;
                snprintf(buf, sizeof(buf), "%s", nl);
                printf("   nets");
                for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
                    printf(" %s=%d", tok, val[net(tok)]);
                printf("\n");
            }
        }
        /* The picture: the model's pixel of each tick, the gates' colour
         * (or 0xFF while they blank) at every half; compared at the end. */
        if (!(in & 0x4A) && !border_known) border_known = 1;      /* /IOREQ, /WR and A0 low */
        if (border_known == 1 && (c & 7) == 5) border_known = 2;
        if (border_known == 2 && (c & 7) == 0) border_known = 3;
        if (lines < MAXPIX) {
            bool paper = v < 192 && c >= 16 && c < 272;
            if (h == 0) {
                model_pix[lines / 4] = paper || border_known == 3 ? (uint8_t)pix : 0xFE;
                model_c[lines / 4] = (uint16_t)c; model_v[lines / 4] = (uint16_t)v;
            }
            gate_pix[lines] = (!val[N_NHBLANK] || val[N_VSYNC]) ? 0xFF :
                              (uint8_t)(val[N_BLUE] | val[N_RED] << 1 | val[N_GREEN] << 2 | !val[N_HL] << 3);
        }
        lines++;
        bool ram16 = (in >> 4 & 1) && !(in >> 5 & 1) && !(in & 1);
        int m_ras = !(ras || ram16), m_cas = !(cas || ccas);
        int g_ras = val[N_RASOE] ? 1 : val[N_RAS], g_cas = val[N_CAS], g_nae = val[N_NAE];
        int g_a = counter(N_A, 7), g_clk = val[N_CPUCLK], g_int = val[N_INT];
        unsigned g_dlow = 0;
        for (int b = 0; b < 8; b++) if (N_DOUT[b] >= 0 && !val[N_DOUT[b]]) g_dlow |= 1u << b;
        int diff[7] = { m_ras != g_ras, m_cas != g_cas, ae && !g_nae && a != g_a, clk != g_clk, (!irq) != g_int,
                        (!ae) != g_nae, dlow != g_dlow };
        /* The known simplifications. */
        if (diff[2] && h == 1 && (c & 3) == 0 && m_ras == 0) { addr_late++; diff[2] = 0; }
        if (diff[0] && g_ras == 1 && h == 0 && ras) { ras_glitch++; diff[0] = 0; }
        /* The CPU ends an I/O read on the falling edge of its clock: in the
         * simulator its pins change in that same half period, and the
         * model (logged after that) has already let the data go. */
        if (diff[6] && !dlow && !clk && prev_clk) { port_end++; diff[6] = 0; }
        prev_clk = clk;
        for (int k = 0; k < 7; k++) {
            if (!diff[k]) continue;
            bad[k]++;
            if ((show_kind < 0 || show_kind == k) && shown++ < 30)
                printf("step %llu C=%u.%u V=%u: %s model %d gates %d (in=%06X)\n", st, c, h, v, what[k],
                       k == 0 ? m_ras : k == 1 ? m_cas : k == 2 ? a : k == 3 ? clk : k == 4 ? !irq : k == 5 ? !ae : (int)dlow,
                       k == 0 ? g_ras : k == 1 ? g_cas : k == 2 ? g_a : k == 3 ? g_clk : k == 4 ? g_int : k == 5 ? g_nae :
                       (int)g_dlow, in);
        }
        if ((unsigned)counter(N_C, 9) != c && h == 2) {
            printf("step %llu: lost sync (gates C=%d, model C=%u)\n", st, counter(N_C, 9), c);
            break;
        }
    } while (fgets(line, sizeof(line), lf));
    /* The pixels come out of a pipeline: find its delay (in half periods)
     * and how many pixels then differ. */
    long long best_match = -1, best_n = 0;
    int best_d = 0;
    long long ticks = (lines < MAXPIX ? lines : MAXPIX) / 4;
    for (int d = -64; d <= 64; d++) {
        long long match = 0, n = 0;
        for (long long t = 16; t + 32 < ticks; t++) {
            long long g = t * 4 + d;
            if (g < 0 || g >= ticks * 4 || gate_pix[g] == 0xFF || model_pix[t] == 0xFE) continue;
            n++;
            if (gate_pix[g] == model_pix[t]) match++;
        }
        if (match > best_match) { best_match = match; best_n = n; best_d = d; }
    }
    printf("pixels: with the gates %d half periods behind the model, %lld of %lld are the same colour\n", best_d,
           best_match, best_n);
    {   /* Where the different pixels are, frame by frame: until the CPU
         * first writes the border (after its 70 ms of reset) the border
         * latch holds whatever it powered on with, which is undefined. */
        static long long hist[64][10];
        long long frame = 0;
        for (long long t = 16; t + 32 < ticks; t++) {
            if (t > 16 && model_v[t] == 0 && model_c[t] == 0) frame++;
            long long g = t * 4 + best_d;
            if (g < 0 || gate_pix[g] == 0xFF || model_pix[t] == 0xFE || gate_pix[g] == model_pix[t] || frame >= 64)
                continue;
            hist[frame][model_v[t] / 32]++;
            /* ULACHECK_PIXELS=n: show the first n different pixels. */
            static long long npix = -2;
            if (npix == -2) { const char *e = getenv("ULACHECK_PIXELS"); npix = e ? atoll(e) : 0; }
            if (npix > 0 && npix--)
                printf("pixel V=%u C=%u: model %X gates %X\n", model_v[t], model_c[t], model_pix[t], gate_pix[g]);
        }
        printf("different pixels per frame:");
        for (int f = 0; f <= frame && f < 64; f++) {
            long long n = 0;
            for (int k = 0; k < 10; k++) n += hist[f][k];
            printf(" %lld", n);
        }
        printf("\n");
    }
    /* ULACHECK_LINE=v: show that line (in the last frame logged). */
    const char *dl = getenv("ULACHECK_LINE");
    if (dl) {
        long long t0 = -1;
        for (long long t = ticks - 1; t >= 0; t--) if (model_v[t] == atoi(dl) && model_c[t] == 0) { t0 = t; break; }
        if (getenv("ULACHECK_RAW") && t0 >= 0) {
            for (long long g = t0 * 4 - 64; g < t0 * 4 + 400; g++)
                printf("half %+lld (C=%lld.%lld) gates %X\n", g - t0 * 4, (g - t0 * 4) / 4, (g - t0 * 4) & 3, gate_pix[g]);
            for (long long t = t0 - 16; t < t0 + 100; t++) printf("model C=%u %X\n", model_c[t], model_pix[t]);
            return 0;
        }
        for (long long t = t0; t0 >= 0 && t < t0 + 448 && t < ticks; t += 1) {
            long long g = t * 4 + best_d;
            printf("C=%3u model %X gates %X%s\n", model_c[t], model_pix[t], g >= 0 ? gate_pix[g] : 0xEE,
                   g >= 0 && gate_pix[g] != 0xFF && gate_pix[g] != model_pix[t] ? "  <<" : "");
        }
    }
    printf("%lld half periods compared. Differences:", lines);
    long long tot = 0;
    for (int k = 0; k < 7; k++) { printf(" %s %lld", what[k], bad[k]); tot += bad[k]; }
    printf("\nknown simplifications: address half a period late %lld, /RAS glitch absent %lld, "
           "port read released with the clock edge %lld\n", addr_late, ras_glitch, port_end);
    return tot != 0;
}
