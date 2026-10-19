//------------------------------------------------------------------------------
//  zex.c
//
//  Runs Frank Cringle's ZEXDOC / ZEXALL through the edge-stepped z80.h, on a
//  'board' which only talks to the CPU through the pins, on every clock edge:
//
//  - RES is held for 4 clock cycles at power-on, the CPU starts at 0000h
//  - 0000h: JP 0100h (boot), a second opcode fetch at 0000h (the program's
//    'JP 0' warm boot) ends the run
//  - 0005h: JP F000h, a small Z80 BDOS stub at F000h prints through OUT (0),A
//    (the stack is set by the program from the word at 0006h, i.e. F000h)
//  - the board writes memory on MREQ+WR and collects console output from
//    the data bus on IO writes
//
//  Board modes:
//    normal: memory answers combinationally (data is put on the bus as soon
//            as MREQ+RD / IORQ+RD is seen)
//    strict: the board drives VALID data only on the exact clock edge where
//            the Zilog timing says the CPU samples it (M1: rising edge of T3,
//            memory/IO read: falling edge of T3, interrupt ack: rising edge
//            of T3) and random garbage on every other edge; it also inserts
//            a random number (0..2) of wait states into every bus cycle by
//            asserting WAIT exactly on the edges where the CPU samples WAIT,
//            and puts random noise on WAIT on all other edges. A CPU that
//            samples data or WAIT on any other edge fails the test.
//
//  usage: zex [zexdoc|zexall] [normal|strict]
//------------------------------------------------------------------------------
#define CHIPS_IMPL
#include "../../chips/z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

static uint8_t mem[1<<16];
static char output[1<<16];
static int out_pos;
static int strict;
static uint32_t rnd_state = 0x12345678;

static uint32_t rnd(void) {
    uint32_t x = rnd_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rnd_state = x;
}

static const uint8_t bdos_stub[] = {
    0x79,               // F000: LD A,C
    0xFE, 0x02,         // F001: CP 2
    0x28, 0x0C,         // F003: JR Z,F011
    0xFE, 0x09,         // F005: CP 9
    0xC0,               // F007: RET NZ
    0x1A,               // F008: LD A,(DE)
    0xFE, 0x24,         // F009: CP '$'
    0xC8,               // F00B: RET Z
    0xD3, 0x00,         // F00C: OUT (0),A
    0x13,               // F00E: INC DE
    0x18, 0xF7,         // F00F: JR F008
    0x7B,               // F011: LD A,E
    0xD3, 0x00,         // F012: OUT (0),A
    0xC9,               // F014: RET
};

// bus cycle tracking for the strict board
enum { CYC_NONE, CYC_M1, CYC_MR, CYC_MW, CYC_IOR, CYC_IOW, CYC_INTA };
static struct {
    int type;
    uint64_t e0;            // edge index where the cycle's strobe was first seen
    int waits;              // number of wait states the board inserts
    uint64_t sample_edge;   // edge on which the data must be valid
} cyc;

static uint64_t edge;       // index of the edge being processed
static int m1_at_zero;      // number of opcode fetches seen at address 0000h

static int classify(uint64_t p) {
    const uint64_t c = p & (Z80_M1|Z80_MREQ|Z80_IORQ|Z80_RD|Z80_WR|Z80_RFSH);
    if (c == (Z80_M1|Z80_MREQ|Z80_RD)) return CYC_M1;
    if (c == (Z80_MREQ|Z80_RD)) return CYC_MR;
    if (c == Z80_MREQ) return CYC_MW;        // write cycle T1 (WR follows at T2)
    if (c == (Z80_MREQ|Z80_WR)) return CYC_MW;
    if (c == (Z80_IORQ|Z80_RD)) return CYC_IOR;
    if (c == (Z80_IORQ|Z80_WR)) return CYC_IOW;
    if (c == (Z80_M1|Z80_IORQ)) return CYC_INTA;
    return CYC_NONE;
}

// called with the CPU outputs of edge 'edge'
static void observe(uint64_t p) {
    const int t = classify(p);
    if (t != CYC_NONE && t != cyc.type) {
        // a new bus cycle strobe appeared
        cyc.type = t;
        cyc.e0 = edge;
        cyc.waits = strict ? (int)(rnd() % 3) : 0;
        if ((t == CYC_M1) && (Z80_GET_ADDR(p) == 0)) {
            m1_at_zero++;
        }
        switch (t) {
            case CYC_M1:   cyc.sample_edge = edge + 3 + 2*cyc.waits; break;
            case CYC_MR:   cyc.sample_edge = edge + 4 + 2*cyc.waits; break;
            case CYC_IOR:  cyc.sample_edge = edge + 5 + 2*cyc.waits; break;
            case CYC_INTA: cyc.sample_edge = edge + 3 + 2*cyc.waits; break;
            default:       cyc.sample_edge = 0; break;
        }
        if (t == CYC_IOW) {
            if (!(p & Z80_DOE)) { printf("\nIO write without DOE!\n"); exit(10); }
            const char c = (char)Z80_GET_DATA(p);
            if (out_pos < (int)sizeof(output)-1) { output[out_pos++] = c; }
            putchar(c); fflush(stdout);
        }
    }
    else if (t == CYC_NONE) {
        cyc.type = CYC_NONE;
    }
    if ((p & (Z80_MREQ|Z80_WR)) == (Z80_MREQ|Z80_WR)) {
        if (!(p & Z80_DOE)) { printf("\nmemory write without DOE!\n"); exit(10); }
        mem[Z80_GET_ADDR(p)] = Z80_GET_DATA(p);
    }
}

// compute the board's inputs for the next edge (edge+1), given the last CPU outputs
static uint64_t inputs(uint64_t p, uint64_t clk, uint64_t next_edge) {
    uint64_t in = clk ? Z80_CLK : 0;
    // RES for the first 4 clock cycles
    if (next_edge < 8) {
        in |= Z80_RES;
    }
    // the value the addressed device would put on the data bus
    uint8_t data = 0xFF;
    bool valid = false;
    switch (cyc.type) {
        case CYC_M1: case CYC_MR: data = mem[Z80_GET_ADDR(p)]; valid = true; break;
        case CYC_IOR: data = 0xFF; valid = true; break;
        case CYC_INTA: data = 0xFF; valid = true; break;
        default: break;
    }
    if (strict) {
        if (!(valid && (next_edge == cyc.sample_edge))) {
            data = (uint8_t)rnd();
        }
        // WAIT sampling edges of the current cycle
        int wait_offset = -1;
        switch (cyc.type) {
            case CYC_M1: case CYC_MR: case CYC_MW: case CYC_INTA: wait_offset = 2; break;
            case CYC_IOR: case CYC_IOW: wait_offset = 3; break;
            default: break;
        }
        bool wait = (rnd() & 1);   // noise
        if (wait_offset >= 0) {
            const int64_t rel = (int64_t)(next_edge - cyc.e0) - wait_offset;
            if ((rel >= 0) && ((rel & 1) == 0) && (rel/2 <= cyc.waits)) {
                // a WAIT sampling edge: active for the first 'waits' samples
                wait = (rel/2) < cyc.waits;
            }
        }
        if (wait) {
            in |= Z80_WAIT;
        }
    }
    in |= ((uint64_t)data) << 16;
    return in;
}

static bool run(const char* name, const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("can't open %s\n", path); return false; }
    memset(mem, 0, sizeof(mem));
    size_t n = fread(&mem[0x0100], 1, sizeof(mem)-0x100, fp);
    fclose(fp);
    printf("%s: loaded %zu bytes, board mode: %s\n", name, n, strict ? "strict (data valid only at sampling edge, random WAIT states + noise)" : "normal");
    mem[0x0000] = 0xC3; mem[0x0001] = 0x00; mem[0x0002] = 0x01;     // JP 0100h
    mem[0x0005] = 0xC3; mem[0x0006] = 0x00; mem[0x0007] = 0xF0;     // JP F000h
    memcpy(&mem[0xF000], bdos_stub, sizeof(bdos_stub));
    out_pos = 0;
    memset(output, 0, sizeof(output));
    memset(&cyc, 0, sizeof(cyc));

    z80_t cpu;
    uint64_t pins = z80_init(&cpu);
    uint64_t clk = 0;
    m1_at_zero = 0;
    uint64_t tstates = 0;
    edge = 0;
    clock_t t0 = clock();
    for (;;) {
        clk ^= 1;
        const uint64_t in = inputs(pins, clk, edge + 1);
        edge++;
        pins = z80_tick(&cpu, in);
        observe(pins);
        if (clk) {
            tstates++;
        }
        if (m1_at_zero == 2) {
            // 'JP 0' warm boot
            break;
        }
    }
    double dur = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n%s: %" PRIu64 " T-states (%" PRIu64 " clock edges) in %.2f s (%.2f MHz emulated)\n",
        name, tstates, edge, dur, tstates / dur / 1e6);
    if (strstr(output, "ERROR") || !strstr(output, "Tests complete")) {
        printf("%s: FAILED\n", name);
        return false;
    }
    printf("%s: ALL TESTS PASSED\n", name);
    return true;
}

int main(int argc, char* argv[]) {
    const char* which = (argc > 1) ? argv[1] : "zexdoc";
    strict = (argc > 2) && (0 == strcmp(argv[2], "strict"));
    char path[256];
    snprintf(path, sizeof(path), "roms/%s.com", which);
    return run(which, path) ? 0 : 10;
}
