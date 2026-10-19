//------------------------------------------------------------------------------
//  wave.c
//
//  Waveform test for the edge-stepped z80.h: runs small programs on a
//  pin-level board, records all pins on every clock edge, and compares the
//  recorded waveforms half-cycle by half-cycle against expected waveforms
//  written by hand from the Zilog Z80 CPU User Manual (UM0080) timing
//  diagrams:
//
//    Figure 5  Instruction Opcode Fetch (M1)
//    Figure 6  Memory Read or Write Cycles
//    Figure 7  Input or Output Cycles (automatic wait state TW*)
//    Figure 8  Bus Request/Acknowledge Cycle
//    Figure 9  Interrupt Request/Acknowledge Cycle (two automatic TW*)
//    Figure 10 Non-Maskable Interrupt Request Operation
//    Figure 12 HALT Exit
//    + wait states (WAIT sampled on the falling edge of T2 / TW*)
//
//  Inside the checked window the board drives VALID read data only on the
//  exact edge where the CPU must sample it (opcode/INTA vector: rising edge
//  of T3, memory/IO read: falling edge of T3) and garbage on every other
//  edge, so wrong sampling edges are detected through the register results.
//  Some scenarios also prove the sampling edges of WAIT and INT, and the
//  edge-triggered NMI, by asserting them on single edges only.
//
//  The recorded waveform of one instance of each kind of cycle is printed.
//------------------------------------------------------------------------------
#define CHIPS_IMPL
#include "../../chips/z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//--- expected machine cycle templates ----------------------------------------
// One character per half T-state, starting with the rising-edge half of T1.
// Active-low lines: '0' = asserted (low), '1' = inactive (high).
// DOE: '1' = CPU drives D0..D7.
// Address: 'P' = the cycle's address, 'R' = refresh address (I:R),
//          'H' = held (unchanged from previous half), '.' = not driven
typedef struct {
    const char* name;
    int halves;
    int wait_idx;       // half where WAIT is sampled (TW states repeat this half's outputs)
    int sample_idx;     // half where read data is sampled (-1: none)
    const char *m1, *mreq, *iorq, *rd, *wr, *rfsh, *doe, *addr;
    const char* tlabels;    // T-state labels (one per T-state, 2 chars each)
} tmpl_t;

//                        name    halves wait sample
static const tmpl_t T_M1    = { "M1",   8, 3, 4,
    /*M1  */ "00001111", /*MREQ*/ "10001001", /*IORQ*/ "11111111", /*RD  */ "10001111",
    /*WR  */ "11111111", /*RFSH*/ "11110000", /*DOE */ "00000000", /*A   */ "PPPPRRRR", "T1T2T3T4" };
static const tmpl_t T_MR    = { "MR",   6, 3, 5,
    "111111", "100001", "111111", "100001", "111111", "111111", "000000", "PPPPPP", "T1T2T3" };
static const tmpl_t T_MW    = { "MW",   6, 3, -1,
    "111111", "100001", "111111", "111111", "111001", "111111", "011111", "PPPPPP", "T1T2T3" };
static const tmpl_t T_IOR   = { "IOR",  8, 5, 7,
    "11111111", "11111111", "11000001", "11000001", "11111111", "11111111", "00000000", "PPPPPPPP", "T1T2TwT3" };
static const tmpl_t T_IOW   = { "IOW",  8, 5, -1,
    "11111111", "11111111", "11000001", "11111111", "11000001", "11111111", "01111111", "PPPPPPPP", "T1T2TwT3" };
static const tmpl_t T_INTA  = { "INTA", 14, 7, 8,
    "00000000111111", "11111111100111", "11111000111111", "11111111111111",
    "11111111111111", "11111111000011", "00000000000000", "PPPPPPPPRRRRRR", "T1T2TwTwT3T4T5" };
static const tmpl_t T_INTA0 = { "INTA", 12, 7, 8,
    "000000001111", "111111111001", "111110001111", "111111111111",
    "111111111111", "111111110000", "000000000000", "PPPPPPPPRRRR", "T1T2TwTwT3T4" };
static const tmpl_t T_NMI   = { "NMI",  10, 3, -1,
    "0000111111", "1000100111", "1111111111", "1000111111",
    "1111111111", "1111000011", "0000000000", "PPPPRRRRRR", "T1T2T3T4T5" };
static const tmpl_t T_INT   = { "",     2, -1, -1,
    "11", "11", "11", "11", "11", "11", "00", "HH", "Ti" };

enum { C_M1, C_MR, C_MW, C_IOR, C_IOW, C_INTA, C_INTA0, C_NMI, C_INT, C_TX, C_RES, C_DEAD };
enum { H_HIGH = 0, H_LOW, H_FALL_T4L, H_RISE_T4L };

typedef struct {
    int type;
    uint16_t addr;      // P address (PC for INTA/NMI)
    int waits;          // additional wait states
    int data;           // expected data for MW/IOW (-1: don't care)
    int halt;           // /HALT behaviour
    int n;              // number of T-states (C_INT, C_TX, C_RES)
} cyc_t;

static const tmpl_t* tmpl_of(int type) {
    switch (type) {
        case C_M1: return &T_M1;
        case C_MR: return &T_MR;
        case C_MW: return &T_MW;
        case C_IOR: return &T_IOR;
        case C_IOW: return &T_IOW;
        case C_INTA: return &T_INTA;
        case C_INTA0: return &T_INTA0;
        case C_NMI: return &T_NMI;
        default: return &T_INT;
    }
}

//--- expected waveform built from a cycle list ----------------------------------
#define MAXH (4096)
typedef struct {
    int len;
    char m1[MAXH], mreq[MAXH], iorq[MAXH], rd[MAXH], wr[MAXH], rfsh[MAXH], doe[MAXH], addr[MAXH];
    char halt[MAXH], busak[MAXH], aoe[MAXH];
    uint16_t paddr[MAXH];
    int data[MAXH];         // expected CPU-driven data (-1 don't care)
    int sample[MAXH];       // 1 for the half where read data is sampled
    int cyc[MAXH];          // index of the machine cycle of each half
    char tlabel[MAXH][3];   // T-state label at H halves
} expect_t;

static expect_t ex;

static void build_expect(const cyc_t* cycles, int num) {
    memset(&ex, 0, sizeof(ex));
    int h = 0;
    for (int c = 0; c < num; c++) {
        const cyc_t* cy = &cycles[c];
        const int start = h;
        if ((cy->type == C_TX) || (cy->type == C_RES) || (cy->type == C_DEAD)) {
            const int n = (cy->type == C_DEAD) ? 1 : cy->n;
            for (int i = 0; i < 2*n; i++, h++) {
                ex.m1[h] = ex.mreq[h] = ex.iorq[h] = ex.rd[h] = ex.wr[h] = ex.rfsh[h] = '1';
                ex.doe[h] = '0';
                ex.addr[h] = '.';
                ex.aoe[h] = '0';
                ex.busak[h] = (cy->type == C_TX) ? ((i == 2*n-1) ? '1' : '0') : '1';
                ex.halt[h] = (cy->type == C_TX) ? ((cy->halt == H_LOW) ? '0' : '1') : '1';
                ex.data[h] = -1;
                ex.cyc[h] = c;
                if (!(i & 1)) {
                    strcpy(ex.tlabel[h], (cy->type == C_TX) ? "Tx" : (cy->type == C_RES) ? "Tr" : "Td");
                }
            }
            continue;
        }
        const tmpl_t* t = tmpl_of(cy->type);
        const int reps = (cy->type == C_INT) ? cy->n : 1;
        for (int r = 0; r < reps; r++) {
            int tl = 0;
            for (int i = 0; i < t->halves; i++) {
                const int copies = (i == t->wait_idx) ? 1 + 2*cy->waits : 1;
                for (int k = 0; k < copies; k++, h++) {
                    // the extra (TW) halves repeat the outputs of the WAIT-sampling half
                    ex.m1[h] = t->m1[i]; ex.mreq[h] = t->mreq[i]; ex.iorq[h] = t->iorq[i];
                    ex.rd[h] = t->rd[i]; ex.wr[h] = t->wr[i]; ex.rfsh[h] = t->rfsh[i];
                    ex.doe[h] = t->doe[i]; ex.addr[h] = t->addr[i];
                    ex.paddr[h] = cy->addr;
                    ex.aoe[h] = '1'; ex.busak[h] = '1';
                    ex.data[h] = (t->doe[i] == '1') ? cy->data : -1;
                    ex.sample[h] = (i == t->sample_idx);
                    ex.cyc[h] = c;
                    char hl = '1';
                    switch (cy->halt) {
                        case H_LOW: hl = '0'; break;
                        case H_FALL_T4L: hl = (i == 7) ? '0' : '1'; break;
                        case H_RISE_T4L: hl = (i == 7) ? '1' : '0'; break;
                        default: break;
                    }
                    ex.halt[h] = hl;
                    if (!((h - start) & 1)) {
                        if ((i == t->wait_idx) && (k > 0)) {
                            strcpy(ex.tlabel[h], "Tw");
                        }
                        else if (!(i & 1)) {
                            ex.tlabel[h][0] = t->tlabels[tl*2]; ex.tlabel[h][1] = t->tlabels[tl*2+1];
                        }
                    }
                    if (k == 0 && (i & 1)) { tl++; }
                }
            }
        }
    }
    ex.len = h;
}

//--- board ---------------------------------------------------------------------
#define MAXREC (1<<16)
static struct { uint64_t in, out; } rec[MAXREC];
static int num_rec;
static uint8_t mem[1<<16];
static uint8_t io_data;         // value of IO reads
static uint8_t vector;          // interrupt vector (data bus during INTA)
static int win_start;           // edge index of the window's first half, -1 before
static uint16_t win_addr;       // window starts at T1 of the M1 at this address...
static int win_skip;            // ...after skipping this many earlier occurrences
static int seen_inta;
typedef uint64_t (*stim_t)(int rel, uint64_t prev_out);   // returns WAIT/INT/NMI/BUSRQ/RES inputs
static stim_t stim;

static uint8_t bus_value(uint64_t p) {
    const uint16_t a = Z80_GET_ADDR(p);
    if ((p & (Z80_MREQ|Z80_RD)) == (Z80_MREQ|Z80_RD)) return mem[a];
    if ((p & (Z80_IORQ|Z80_M1)) == (Z80_IORQ|Z80_M1)) return vector;
    if ((p & (Z80_IORQ|Z80_RD)) == (Z80_IORQ|Z80_RD)) return io_data;
    return 0xFF;
}

// run until 'num_edges' edges after the window start (or a limit)
static z80_t cpu;
static void run(int num_edges) {
    num_rec = 0;
    win_start = -1;
    seen_inta = 0;
    uint64_t out = z80_init(&cpu);
    uint64_t clk = 0;
    int skip = win_skip;
    while (num_rec < MAXREC - 1) {
        clk ^= 1;
        const int edge = num_rec;
        const int rel = (win_start >= 0) ? (edge - win_start) : -1;
        uint64_t in = clk ? Z80_CLK : 0;
        uint8_t data;
        if (rel >= 0 && rel < ex.len) {
            // inside the window: valid data only on the expected sampling edge
            // (the sampled values are those of the strobes active in the previous half)
            data = ex.sample[rel] ? bus_value(out) : (uint8_t)(0xA5 ^ (rel * 37));
        }
        else {
            data = bus_value(out);
        }
        in |= ((uint64_t)data) << 16;
        if (stim) {
            in |= stim(rel, out);
        }
        out = z80_tick(&cpu, in);
        rec[num_rec].in = in;
        rec[num_rec].out = out;
        num_rec++;
        if ((out & (Z80_M1|Z80_IORQ)) == (Z80_M1|Z80_IORQ)) {
            seen_inta = 1;
        }
        if ((out & Z80_MREQ) && (out & Z80_WR)) {
            mem[Z80_GET_ADDR(out)] = Z80_GET_DATA(out);
        }
        if (win_start < 0) {
            // T1 rising edge of an M1 at the window address
            const uint64_t prev = (edge > 0) ? rec[edge-1].out : 0;
            if ((out & Z80_CLK) && (out & Z80_M1) && !(out & Z80_IORQ) && (Z80_GET_ADDR(out) == win_addr) &&
                !((prev & Z80_M1) && (Z80_GET_ADDR(prev) == win_addr)))
            {
                if (skip == 0) {
                    win_start = edge;
                }
                else {
                    skip--;
                }
            }
        }
        if ((win_start >= 0) && (edge - win_start >= num_edges)) {
            break;
        }
    }
}

//--- checking and printing --------------------------------------------------------
static char lvl(uint64_t p, uint64_t mask) { return (p & mask) ? '0' : '1'; }   // active-low line level

static void print_wave(const char* title, int from, int len, const cyc_t* cycles) {
    printf("\n%s\n", title);
    char line[8192];
    // cycle names
    int p = sprintf(line, "%-8s", "cycle");
    int last_c = -1;
    for (int i = 0; i < len; i++) {
        char cell[16] = "   ";
        const int c = ex.cyc[i];
        if (c != last_c && i < ex.len) {
            const cyc_t* cy = &cycles[c];
            const char* nm = (cy->type == C_TX) ? "BUSAK" : (cy->type == C_RES) ? "RESET" : (cy->type == C_DEAD) ? "" : tmpl_of(cy->type)->name;
            if (cy->type == C_INT) nm = "int";
            snprintf(cell, sizeof(cell), "%s", nm);
            last_c = c;
            p += sprintf(line + p, "%-3s", cell);
            // allow names to spill over
            int extra = (int)strlen(cell) - 3;
            while (extra > 0 && i+1 < len && ex.cyc[i+1] == c) { i++; extra -= 3; }
            if (extra < 0) { p += sprintf(line+p, "%*s", 0, ""); }
            continue;
        }
        p += sprintf(line + p, "   ");
    }
    printf("%s\n", line);
    // T-state labels
    p = sprintf(line, "%-8s", "T");
    for (int i = 0; i < len; i++) {
        p += sprintf(line + p, "%-3s", (i < ex.len) ? ex.tlabel[i] : "");
    }
    printf("%s\n", line);
    // clock
    p = sprintf(line, "%-8s", "CLK");
    for (int i = 0; i < len; i++) {
        p += sprintf(line + p, "%s", (rec[from+i].out & Z80_CLK) ? "---" : "___");
    }
    printf("%s\n", line);
    // address bus
    p = sprintf(line, "%-8s", "A");
    for (int i = 0; i < len; ) {
        const uint64_t o = rec[from+i].out;
        const uint64_t po = rec[from+i-1].out;
        if (!(o & Z80_AOE)) {
            p += sprintf(line + p, "zzz"); i++;
        }
        else if ((i == 0) || (Z80_GET_ADDR(o) != Z80_GET_ADDR(po)) || !(po & Z80_AOE)) {
            p += sprintf(line + p, "%04X  ", Z80_GET_ADDR(o)); i += 2;
        }
        else {
            p += sprintf(line + p, "   "); i++;
        }
    }
    printf("%s\n", line);
    // data bus: CPU-driven (DOE) or the value on the bus at the sampling edge
    p = sprintf(line, "%-8s", "D");
    for (int i = 0; i < len; i++) {
        const uint64_t o = rec[from+i].out;
        if (o & Z80_DOE) {
            const uint64_t po = rec[from+i-1].out;
            if (!(po & Z80_DOE) || (Z80_GET_DATA(po) != Z80_GET_DATA(o))) p += sprintf(line + p, ">%02X", Z80_GET_DATA(o));
            else p += sprintf(line + p, "===");
        }
        else if (i < ex.len && ex.sample[i]) {
            p += sprintf(line + p, "<%02X", Z80_GET_DATA(rec[from+i].in));
        }
        else {
            p += sprintf(line + p, "   ");
        }
    }
    printf("%s\n", line);
    struct { const char* name; uint64_t mask; int input; int active_low; } sig[] = {
        { "/M1", Z80_M1, 0, 1 }, { "/MREQ", Z80_MREQ, 0, 1 }, { "/IORQ", Z80_IORQ, 0, 1 },
        { "/RD", Z80_RD, 0, 1 }, { "/WR", Z80_WR, 0, 1 }, { "/RFSH", Z80_RFSH, 0, 1 },
        { "/HALT", Z80_HALT, 0, 1 }, { "/WAIT", Z80_WAIT, 1, 1 }, { "/INT", Z80_INT, 1, 1 },
        { "/NMI", Z80_NMI, 1, 1 }, { "/RESET", Z80_RES, 1, 1 }, { "/BUSRQ", Z80_BUSRQ, 1, 1 },
        { "/BUSAK", Z80_BUSAK, 0, 1 }, { "DOE", Z80_DOE, 0, 0 }, { "AOE", Z80_AOE, 0, 0 },
    };
    for (size_t s = 0; s < sizeof(sig)/sizeof(sig[0]); s++) {
        bool any = false;
        for (int i = 0; i < len; i++) {
            const uint64_t v = sig[s].input ? rec[from+i].in : rec[from+i].out;
            if (sig[s].active_low ? (v & sig[s].mask) : !(v & sig[s].mask)) any = true;
        }
        if (!any && (sig[s].input || sig[s].mask == Z80_HALT || sig[s].mask == Z80_BUSAK || sig[s].mask == Z80_AOE)) {
            continue;   // don't print constant inactive inputs and HALT/BUSAK/AOE
        }
        p = sprintf(line, "%-8s", sig[s].name);
        for (int i = 0; i < len; i++) {
            const uint64_t v = sig[s].input ? rec[from+i].in : rec[from+i].out;
            const bool high = sig[s].active_low ? !(v & sig[s].mask) : (v & sig[s].mask);
            p += sprintf(line + p, "%s", high ? "---" : "___");
        }
        printf("%s\n", line);
    }
}

// compact rendering: one character per half clock cycle
static int compact;
static void print_compact(const char* title, int from, int len, const cyc_t* cycles) {
    printf("\n%s\n", title);
    static char line[MAXH*2];
    #define ROW_BEGIN(name) memset(line, ' ', sizeof(line)); int p = sprintf(line, "%-7s", name); line[p] = ' '; (void)p
    #define ROW_END() { int e = 7 + len; line[e] = 0; while (e > 0 && line[e-1] == ' ') line[--e] = 0; printf("%s\n", line); }
    {   // machine cycle names
        ROW_BEGIN("cycle");
        int last = -1;
        for (int i = 0; i < len; i++) {
            const int c = ex.cyc[i];
            if (c != last) {
                const cyc_t* cy = &cycles[c];
                const char* nm = (cy->type == C_TX) ? "Tx" : (cy->type == C_RES) ? "RES" : (cy->type == C_DEAD) ? "" : (cy->type == C_INT) ? "int" : tmpl_of(cy->type)->name;
                // truncate the name to the length of the machine cycle
                int n = (int)strlen(nm), k = 0;
                while ((k < n) && (i + k < len) && (ex.cyc[i+k] == c)) k++;
                memcpy(line + 7 + i, nm, k);
                last = c;
            }
        }
        ROW_END();
    }
    {   // T-states
        ROW_BEGIN("T");
        for (int i = 0; i < len; i++) {
            if (ex.tlabel[i][0]) line[7+i] = (ex.tlabel[i][1] == 'w') ? 'w' : ex.tlabel[i][1];
        }
        ROW_END();
    }
    {   ROW_BEGIN("CLK");
        for (int i = 0; i < len; i++) line[7+i] = (rec[from+i].out & Z80_CLK) ? '-' : '_';
        ROW_END();
    }
    {   // address bus: 4 hex digits where it changes
        ROW_BEGIN("A");
        for (int i = 0; i < len; i++) {
            const uint64_t o = rec[from+i].out, po = rec[from+i-1].out;
            if (!(o & Z80_AOE)) { line[7+i] = 'z'; continue; }
            if ((i == 0) || (Z80_GET_ADDR(o) != Z80_GET_ADDR(po)) || !(po & Z80_AOE)) {
                char hx[8]; snprintf(hx, sizeof(hx), "%04X", Z80_GET_ADDR(o));
                memcpy(line + 7 + i, hx, 4);
                i += 3;
            }
        }
        ROW_END();
    }
    {   // data bus: CPU driven value ('=' while held), or the value sampled by the CPU
        ROW_BEGIN("D");
        for (int i = 0; i < len; i++) {
            const uint64_t o = rec[from+i].out, po = rec[from+i-1].out;
            char hx[8];
            if (o & Z80_DOE) {
                if (!(po & Z80_DOE) || (Z80_GET_DATA(po) != Z80_GET_DATA(o))) {
                    snprintf(hx, sizeof(hx), "%02X", Z80_GET_DATA(o)); memcpy(line+7+i, hx, 2); i++;
                    if (i < len && (rec[from+i].out & Z80_DOE)) {} else continue;
                }
                else line[7+i] = '=';
            }
            else if (i < ex.len && ex.sample[i]) {
                snprintf(hx, sizeof(hx), "%02X", Z80_GET_DATA(rec[from+i].in)); memcpy(line+7+i, hx, 2); i++;
            }
        }
        ROW_END();
    }
    struct { const char* name; uint64_t mask; int input; int active_low; } sig[] = {
        { "/M1", Z80_M1, 0, 1 }, { "/MREQ", Z80_MREQ, 0, 1 }, { "/IORQ", Z80_IORQ, 0, 1 },
        { "/RD", Z80_RD, 0, 1 }, { "/WR", Z80_WR, 0, 1 }, { "/RFSH", Z80_RFSH, 0, 1 },
        { "/HALT", Z80_HALT, 0, 1 }, { "/WAIT", Z80_WAIT, 1, 1 }, { "/INT", Z80_INT, 1, 1 },
        { "/NMI", Z80_NMI, 1, 1 }, { "/RESET", Z80_RES, 1, 1 }, { "/BUSRQ", Z80_BUSRQ, 1, 1 },
        { "/BUSAK", Z80_BUSAK, 0, 1 }, { "DOE", Z80_DOE, 0, 0 }, { "AOE", Z80_AOE, 0, 0 },
    };
    for (size_t k = 0; k < sizeof(sig)/sizeof(sig[0]); k++) {
        bool any = false;
        for (int i = 0; i < len; i++) {
            const uint64_t v = sig[k].input ? rec[from+i].in : rec[from+i].out;
            if (sig[k].active_low ? (v & sig[k].mask) : !(v & sig[k].mask)) any = true;
        }
        if (!any && (sig[k].input || sig[k].mask == Z80_HALT || sig[k].mask == Z80_BUSAK || sig[k].mask == Z80_AOE || sig[k].mask == Z80_IORQ || sig[k].mask == Z80_WR)) continue;
        if (!any && sig[k].mask == Z80_DOE) continue;
        ROW_BEGIN(sig[k].name);
        for (int i = 0; i < len; i++) {
            const uint64_t v = sig[k].input ? rec[from+i].in : rec[from+i].out;
            const bool high = sig[k].active_low ? !(v & sig[k].mask) : (v & sig[k].mask);
            line[7+i] = high ? '-' : '_';
        }
        ROW_END();
    }
    #undef ROW_BEGIN
    #undef ROW_END
}

static int num_checked_halves, num_scenarios, num_failed;

static bool check(const char* title, const cyc_t* cycles, int num, bool print) {
    num_scenarios++;
    const int from = win_start;
    if (from < 0) {
        printf("FAIL %s: window start not found\n", title);
        num_failed++;
        return false;
    }
    if (from + ex.len > num_rec) {
        printf("FAIL %s: recording too short\n", title);
        num_failed++;
        return false;
    }
    int bad = -1;
    const char* what = "";
    int last_r = -1;
    for (int i = 0; i < ex.len && bad < 0; i++) {
        const uint64_t o = rec[from+i].out;
        const uint64_t po = rec[from+i-1].out;
        #define CHK(sigstr, mask, name) if (sigstr[i] != lvl(o, mask)) { bad = i; what = name; break; }
        CHK(ex.m1, Z80_M1, "/M1");
        CHK(ex.mreq, Z80_MREQ, "/MREQ");
        CHK(ex.iorq, Z80_IORQ, "/IORQ");
        CHK(ex.rd, Z80_RD, "/RD");
        CHK(ex.wr, Z80_WR, "/WR");
        CHK(ex.rfsh, Z80_RFSH, "/RFSH");
        CHK(ex.halt, Z80_HALT, "/HALT");
        CHK(ex.busak, Z80_BUSAK, "/BUSAK");
        #undef CHK
        if (ex.doe[i] != ((o & Z80_DOE) ? '1' : '0')) { bad = i; what = "DOE"; break; }
        if (ex.aoe[i] != ((o & Z80_AOE) ? '1' : '0')) { bad = i; what = "AOE"; break; }
        const uint16_t a = Z80_GET_ADDR(o);
        switch (ex.addr[i]) {
            case 'P': if (a != ex.paddr[i]) { bad = i; what = "address"; } break;
            case 'H': if (a != Z80_GET_ADDR(po)) { bad = i; what = "address (held)"; } break;
            case 'R':
                // refresh address: I in A8..A15, R in A0..A7, R bits 0..6 count up by one per M1
                if (((a >> 8) != cpu.i)) { bad = i; what = "refresh address (I)"; }
                else if ((i == 0 || ex.addr[i-1] != 'R')) {
                    if ((last_r >= 0) && ((a & 0x7F) != ((last_r + 1) & 0x7F))) { bad = i; what = "refresh address (R)"; }
                    last_r = a & 0x7F;
                }
                else if (a != Z80_GET_ADDR(po)) { bad = i; what = "refresh address (held)"; }
                break;
            default: break;
        }
        if (bad >= 0) break;
        if ((ex.data[i] >= 0) && (Z80_GET_DATA(o) != ex.data[i])) { bad = i; what = "data"; break; }
    }
    num_checked_halves += ex.len;
    if (bad >= 0) {
        printf("FAIL %s: %s mismatch at half %d (T-state %d %s)\n", title, what, bad, bad/2, (bad & 1) ? "falling" : "rising");
        num_failed++;
        print_wave(title, from, ex.len, cycles);
        return false;
    }
    if (print) {
        if (compact) print_compact(title, from, ex.len, cycles);
        else print_wave(title, from, ex.len, cycles);
    }
    printf("OK   %-64s (%d half-cycles)\n", title, ex.len);
    return true;
}

static void load(uint16_t addr, const uint8_t* bytes, int n) {
    memcpy(&mem[addr], bytes, n);
}

static void reset_board(void) {
    memset(mem, 0, sizeof(mem));
    stim = 0;
    win_skip = 0;
    io_data = 0x3C;
    vector = 0xFF;
}

static void result(const char* what, bool ok) {
    num_scenarios++;
    if (ok) printf("OK   %s\n", what);
    else { printf("FAIL %s\n", what); num_failed++; }
}

//--- stimulus functions ----------------------------------------------------------
static int wait_rel_from, wait_rel_to, wait_except;     // WAIT active for rel in [from,to] (except 'wait_except')
static uint64_t stim_wait(int rel, uint64_t prev) {
    (void)prev;
    if (rel < 0) return 0;
    return ((rel >= wait_rel_from) && (rel <= wait_rel_to) && (rel != wait_except)) ? Z80_WAIT : 0;
}
static uint64_t stim_int_until_ack(int rel, uint64_t prev) {
    (void)prev;
    return ((rel >= 0) && !seen_inta) ? Z80_INT : 0;
}
static int int_rel_from, int_rel_to, int_except;
static uint64_t stim_int_window(int rel, uint64_t prev) {
    (void)prev;
    return ((rel >= int_rel_from) && (rel <= int_rel_to) && (rel != int_except)) ? Z80_INT : 0;
}
static int nmi_rel;
static uint64_t stim_nmi_pulse(int rel, uint64_t prev) {
    (void)prev;
    return (rel == nmi_rel) ? Z80_NMI : 0;
}
static uint64_t stim_int_wait(int rel, uint64_t prev) {
    return stim_int_until_ack(rel, prev) | stim_wait(rel, prev);
}
static int halt_ints_after;     // assert INT after this many halted M1 cycles were seen
static uint64_t stim_halt_int(int rel, uint64_t prev) {
    static int count;
    if (rel < 0) { count = 0; return 0; }
    if ((prev & Z80_CLK) && (prev & Z80_M1) && !(prev & Z80_IORQ) && (prev & Z80_HALT) && !(prev & (Z80_MREQ|Z80_RFSH))) {
        count++;    // T1 rising edge of a halted M1
    }
    return ((count > halt_ints_after) && !seen_inta) ? Z80_INT : 0;
}
static int brq_from, brq_to;
static uint64_t stim_busrq(int rel, uint64_t prev) {
    (void)prev;
    return ((rel >= brq_from) && (rel <= brq_to)) ? Z80_BUSRQ : 0;
}
static int res_from, res_to;
static uint64_t stim_res(int rel, uint64_t prev) {
    (void)prev;
    return ((rel >= res_from) && (rel <= res_to)) ? Z80_RES : 0;
}

//--- scenarios ---------------------------------------------------------------------
// common setup code at 0000h: SP=8000h, A=5Ah, BC=1234h, then JP 0100h
static const uint8_t setup_code[] = {
    0x31, 0x00, 0x80,       // LD SP,8000h
    0x3E, 0x5A,             // LD A,5Ah
    0x01, 0x34, 0x12,       // LD BC,1234h
    0xC3, 0x00, 0x01,       // JP 0100h
};

#define NUMC(c) ((int)(sizeof(c)/sizeof(c[0])))

static void scenario(const char* title, const cyc_t* cyc, int n, bool print) {
    build_expect(cyc, n);
    run(ex.len + 4);
    build_expect(cyc, n);
    check(title, cyc, n, print);
}

int main(int argc, char* argv[]) {
    compact = (argc > 1) && (0 == strcmp(argv[1], "-c"));
    printf("Waveform test: recorded pins vs. expected waveforms from the Zilog UM0080 timing diagrams\n");
    if (compact) {
        printf("(one column per half clock cycle, T-state numbers at the rising edge half;\n");
        printf(" '-' line high, '_' line low; A: address where it changes, 'z' floating;\n");
        printf(" D: hex value starting at the CPU's sampling edge for reads, or at the edge where\n");
        printf(" the CPU starts driving it for writes ('=' still driven))\n");
    }
    else {
        printf("(waveform legend: '-' line high, '_' line low; A: address when it changes, 'zzz' floating;\n");
        printf(" D: '>xx' CPU drives xx, '===' CPU keeps driving, '<xx' value on the bus at the CPU's sampling edge)\n");
    }

    //--- M1 + memory reads: LD A,(1234h)
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x3A, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x1234] = 0xC7;
        win_addr = 0x0100;
        const cyc_t c[] = {
            { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MR, 0x1234 }, { C_M1, 0x0103 },
        };
        scenario("LD A,(1234h): opcode fetch M1, memory read MR x3", c, NUMC(c), true);
        result("  ... A loaded from memory sampled at T3 falling edge (A=C7h)", cpu.a == 0xC7);
    }
    //--- memory write: LD (1234h),A
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x32, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = {
            { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MW, 0x1234, 0, 0x5A }, { C_M1, 0x0103 },
        };
        scenario("LD (1234h),A: memory write MW", c, NUMC(c), true);
        result("  ... memory written (1234h)=5Ah", mem[0x1234] == 0x5A);
    }
    //--- internal T-states: INC BC (address bus holds the refresh address)
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x03, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INT, 0, 0, -1, 0, 2 }, { C_M1, 0x0101 } };
        scenario("INC BC: M1 + 2 internal T-states", c, NUMC(c), false);
    }
    //--- PUSH BC: internal T-state, two back-to-back writes
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xC5, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INT, 0, 0, -1, 0, 1 }, { C_MW, 0x7FFF, 0, 0x12 }, { C_MW, 0x7FFE, 0, 0x34 }, { C_M1, 0x0101 } };
        scenario("PUSH BC: M1 + internal + MW x2", c, NUMC(c), false);
    }
    //--- IO read: IN A,(FEh)
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xDB, 0xFE, 0x00 };
        load(0x0100, prog, sizeof(prog));
        io_data = 0x3C;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_IOR, 0x5AFE }, { C_M1, 0x0102 } };
        scenario("IN A,(FEh): IO read IOR (automatic TW)", c, NUMC(c), true);
        result("  ... A loaded from IO data sampled at T3 falling edge (A=3Ch)", cpu.a == 0x3C);
    }
    //--- IO write: OUT (FEh),A
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xD3, 0xFE, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_IOW, 0x5AFE, 0, 0x5A }, { C_M1, 0x0102 } };
        scenario("OUT (FEh),A: IO write IOW (automatic TW)", c, NUMC(c), true);
    }
    //--- IN r,(C) and OUT (C),r via ED prefix
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xED, 0x78, 0xED, 0x79, 0x00 };   // IN A,(C); OUT (C),A
        load(0x0100, prog, sizeof(prog));
        io_data = 0x99;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_M1, 0x0101 }, { C_IOR, 0x1234 }, { C_M1, 0x0102 }, { C_M1, 0x0103 }, { C_IOW, 0x1234, 0, 0x99 }, { C_M1, 0x0104 } };
        scenario("IN A,(C) / OUT (C),A: ED prefix M1 x2, IOR, IOW", c, NUMC(c), false);
    }
    //--- maskable interrupt IM1
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0xED, 0x56, 0xFB, 0xC3, 0x00, 0x01 };  // LD SP; IM 1; EI; JP 0100h
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        stim = stim_int_until_ack;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INTA, 0x0101 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 }, { C_M1, 0x0038 } };
        scenario("IM1 interrupt: NOP, INTA (2 automatic TW + 1 extra T), push PC, M1 at 0038h", c, NUMC(c), true);
    }
    //--- maskable interrupt IM2 (vector sampled at T3 rising edge of INTA)
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0x3E, 0x90, 0xED, 0x47, 0xED, 0x5E, 0xFB, 0xC3, 0x00, 0x01 };  // LD SP; LD A,90h; LD I,A; IM 2; EI; JP 0100h
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x9040] = 0x00; mem[0x9041] = 0x20;     // vector table entry => 2000h
        vector = 0x40;
        win_addr = 0x0100;
        stim = stim_int_until_ack;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INTA, 0x0101 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 },
                            { C_MR, 0x9040 }, { C_MR, 0x9041 }, { C_M1, 0x2000 } };
        scenario("IM2 interrupt: INTA (vector 40h, I=90h), push PC, read vector table, M1 at 2000h", c, NUMC(c), true);
    }
    //--- maskable interrupt IM0 (RST 38h on the data bus)
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0xED, 0x46, 0xFB, 0xC3, 0x00, 0x01 };  // LD SP; IM 0; EI; JP 0100h
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        vector = 0xFF;  // RST 38h
        win_addr = 0x0100;
        stim = stim_int_until_ack;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INTA0, 0x0101 }, { C_INT, 0, 0, -1, 0, 1 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 }, { C_M1, 0x0038 } };
        scenario("IM0 interrupt: INTA with RST 38h on the data bus", c, NUMC(c), false);
    }
    //--- NMI: a single half-cycle pulse on /NMI in the middle of an instruction
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        stim = stim_nmi_pulse;
        nmi_rel = 3;    // /NMI low only during T2 L of the NOP's M1
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_NMI, 0x0101 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 }, { C_M1, 0x0066 } };
        scenario("NMI: /NMI pulse (one half-cycle) during NOP, NMI M1 (5 T), push PC, M1 at 0066h", c, NUMC(c), true);
    }
    //--- HALT, halted M1 cycles at HALT+1, exit with INT
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0xED, 0x56, 0xFB, 0xC3, 0x00, 0x01 };  // LD SP; IM 1; EI; JP 0100h
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x76, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        halt_ints_after = 2;
        stim = stim_halt_int;
        const cyc_t c[] = {
            { C_M1, 0x0100, 0, -1, H_FALL_T4L },
            { C_M1, 0x0101, 0, -1, H_LOW }, { C_M1, 0x0101, 0, -1, H_LOW }, { C_M1, 0x0101, 0, -1, H_RISE_T4L },
            { C_INTA, 0x0101 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 }, { C_M1, 0x0038 } };
        scenario("HALT: /HALT low, NOP M1 cycles at HALT+1 with refresh, INT exits", c, NUMC(c), true);
    }
    //--- WAIT in a memory read: a slow device holds WAIT for one sample
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x3A, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x1234] = 0x77;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MR, 0x1234, 1 }, { C_M1, 0x0103 } };
        // the MR(1234h) starts at half 8+6+6=20: WAIT active from its T1 falling edge
        // (half 21) up to the TW rising edge (half 24), released before TW falling
        stim = stim_wait; wait_rel_from = 21; wait_rel_to = 24; wait_except = -1;
        scenario("WAIT in memory read: one TW state inserted", c, NUMC(c), true);
        result("  ... A loaded from memory sampled at T3 falling edge after TW (A=77h)", cpu.a == 0x77);
    }
    //--- WAIT sampling edge proof: WAIT active on every edge of the MR except T2 falling => no TW
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x3A, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x1234] = 0x78;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MR, 0x1234 }, { C_M1, 0x0103 } };
        stim = stim_wait; wait_rel_from = 20; wait_rel_to = 25; wait_except = 23;
        scenario("WAIT active on all MR edges except T2 falling: no wait state", c, NUMC(c), false);
        result("  ... A=78h", cpu.a == 0x78);
    }
    //--- ...and WAIT active only on the T2 falling edge => exactly one TW
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x3A, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x1234] = 0x79;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MR, 0x1234, 1 }, { C_M1, 0x0103 } };
        stim = stim_wait; wait_rel_from = 23; wait_rel_to = 23; wait_except = -1;
        scenario("WAIT active only on the MR's T2 falling edge: one wait state", c, NUMC(c), false);
        result("  ... A=79h", cpu.a == 0x79);
    }
    //--- WAIT in M1 (2 TW), in MW (1 TW), in IOR (1 extra TW), in IOW, in INTA
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100, 2 }, { C_M1, 0x0101 } };
        stim = stim_wait; wait_rel_from = 3; wait_rel_to = 5; wait_except = 4;   // sampled at halves 3 and 5
        scenario("WAIT in opcode fetch: two TW states", c, NUMC(c), true);
    }
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x32, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_MR, 0x0102 }, { C_MW, 0x1234, 1, 0x5A }, { C_M1, 0x0103 } };
        stim = stim_wait; wait_rel_from = 23; wait_rel_to = 23; wait_except = -1;
        scenario("WAIT in memory write: one TW state, /WR stays low", c, NUMC(c), true);
        result("  ... memory written (1234h)=5Ah", mem[0x1234] == 0x5A);
    }
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xDB, 0xFE, 0x00 };
        load(0x0100, prog, sizeof(prog));
        io_data = 0x42;
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_IOR, 0x5AFE, 1 }, { C_M1, 0x0102 } };
        // IOR starts at half 14, its automatic TW falls at half 14+5 = 19 => WAIT sampled there
        // WAIT on T2 falling (half 17) must be ignored in IO cycles
        stim = stim_wait; wait_rel_from = 17; wait_rel_to = 19; wait_except = 18;
        scenario("WAIT in IO read: sampled at the automatic TW falling edge (not T2), one extra TW", c, NUMC(c), true);
        result("  ... A=42h", cpu.a == 0x42);
    }
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0xD3, 0xFE, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_IOW, 0x5AFE, 2, 0x5A }, { C_M1, 0x0102 } };
        stim = stim_wait; wait_rel_from = 19; wait_rel_to = 21; wait_except = -1;
        scenario("WAIT in IO write: two extra TW states", c, NUMC(c), false);
    }
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0x3E, 0x90, 0xED, 0x47, 0xED, 0x5E, 0xFB, 0xC3, 0x00, 0x01 };
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x9040] = 0x00; mem[0x9041] = 0x20;
        vector = 0x40;
        win_addr = 0x0100;
        // INT during the NOP (until acknowledged), WAIT active only on the falling
        // edge of the second automatic TW of the INTA (half 8+7 = 15)
        wait_rel_from = 15; wait_rel_to = 15; wait_except = -1;
        stim = stim_int_wait;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INTA, 0x0101, 1 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 },
                            { C_MR, 0x9040 }, { C_MR, 0x9041 }, { C_M1, 0x2000 } };
        scenario("WAIT in interrupt acknowledge: one extra TW after the two automatic TW", c, NUMC(c), false);
    }
    //--- INT sampling edge proof: INT active only on the rising edge of the last T-state
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0xED, 0x56, 0xFB, 0xC3, 0x00, 0x01 };
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        stim = stim_int_window; int_rel_from = 6; int_rel_to = 6; int_except = -1;   // T4 rising edge of the NOP
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_INTA, 0x0101 }, { C_MW, 0x7FFF, 0, 0x01 }, { C_MW, 0x7FFE, 0, 0x01 }, { C_M1, 0x0038 } };
        scenario("INT active only at the rising edge of the last T-state (T4): accepted", c, NUMC(c), false);
    }
    {
        reset_board();
        const uint8_t setup[] = { 0x31, 0x00, 0x80, 0xED, 0x56, 0xFB, 0xC3, 0x00, 0x01 };
        load(0x0000, setup, sizeof(setup));
        const uint8_t prog[] = { 0x00, 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        // INT active on every edge of the NOP except the T4 rising edge (and released at the next NOP)
        stim = stim_int_window; int_rel_from = 0; int_rel_to = 7; int_except = 6;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_M1, 0x0101 }, { C_M1, 0x0102 } };
        scenario("INT active on all NOP edges except T4 rising: not accepted", c, NUMC(c), false);
    }
    //--- bus request
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x3A, 0x34, 0x12, 0x00 };
        load(0x0100, prog, sizeof(prog));
        mem[0x1234] = 0x66;
        win_addr = 0x0100;
        // BUSRQ active from the T2 rising edge of MR(0101h) (half 10) for 3 clocks;
        // sampled at the T3 rising edge (half 12), bus released at the next rising
        // edge, BUSRQ released before half 16 => Tx at halves 14..17 (2 T-states)
        stim = stim_busrq; brq_from = 10; brq_to = 15;
        const cyc_t c[] = { { C_M1, 0x0100 }, { C_MR, 0x0101 }, { C_TX, 0, 0, -1, 0, 2 }, { C_MR, 0x0102 }, { C_MR, 0x1234 }, { C_M1, 0x0103 } };
        scenario("BUSRQ during a memory read: bus released after the machine cycle (BUSAK, floating bus)", c, NUMC(c), true);
        result("  ... A=66h", cpu.a == 0x66);
    }
    //--- reset: RES held for 3 clocks in the middle of an instruction
    {
        reset_board();
        load(0x0000, setup_code, sizeof(setup_code));
        const uint8_t prog[] = { 0x00, 0x00 };
        load(0x0100, prog, sizeof(prog));
        win_addr = 0x0100;
        // RES active on the edges of halves 2..7 (sampled at the rising edges 2, 4, 6)
        stim = stim_res; res_from = 2; res_to = 7;
        // T1 of the M1 at 0100h (cut short), 3 reset T-states, one dead T-state, M1 at 0000h
        cyc_t c3[] = { { C_M1, 0x0100 }, { C_RES, 0, 0, -1, 0, 3 }, { C_DEAD }, { C_M1, 0x0000 }, { C_MR, 0x0001 } };
        build_expect(c3, NUMC(c3));
        // remove halves 2..7 of the first M1
        expect_t tmp = ex;
        int h = 0;
        for (int i = 0; i < tmp.len; i++) {
            if ((i >= 2) && (i < 8)) continue;
            #define CP(f) ex.f[h] = tmp.f[i]
            CP(m1); CP(mreq); CP(iorq); CP(rd); CP(wr); CP(rfsh); CP(doe); CP(addr); CP(halt); CP(busak); CP(aoe);
            CP(paddr); CP(data); CP(sample); CP(cyc);
            memcpy(ex.tlabel[h], tmp.tlabel[i], 3);
            #undef CP
            h++;
        }
        ex.len = h;
        const int len = ex.len;
        expect_t saved = ex;
        run(len + 4);
        ex = saved;
        check("RESET: RES held 3 clocks, outputs inactive, bus floating, 1 dead T-state, M1 at 0000h", c3, NUMC(c3), true);
        // before the reset SP=8000h and R=5, after: SP=FFFFh, R=0, then 1 M1 refresh
        // and LD SP,nn has loaded the low byte (00h) so far
        result("  ... registers reset (SP high byte FFh, R restarted from 0)", ((cpu.sp >> 8) == 0xFF) && (cpu.r == 1) && !cpu.iff1 && (cpu.im == 0));
    }

    printf("\n%d scenarios/checks, %d half-cycles compared against the expected waveforms, %d FAILED\n",
        num_scenarios, num_checked_halves, num_failed);
    return num_failed ? 10 : 0;
}

