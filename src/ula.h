/* ula.h - the Ferranti ULA 6C001 of the ZX Spectrum 48K (IC1), modeled at
 * its pins. See ula.c. */
#ifndef ULA_H
#define ULA_H
#include "sim.h"
#include "parts.h"

#define ULA_TICKS_PER_LINE  448     /* pixel clock (7 MHz) ticks */
#define ULA_LINES           312
#define ULA_SCREEN_W        352     /* 48 pixels of border left and right */
#define ULA_SCREEN_H        296     /* 48 lines of border on top, 56 below */

typedef struct {
    /* Time: the pixel counter C (0..447), the line counter V (0..311) and
     * the half period of the 14 MHz oscillator inside a pixel tick (0..3). */
    uint16_t c, v;
    uint8_t half;
    uint32_t frames;
    /* Latches. */
    uint8_t data, attr;             /* bitmap and attribute bytes fetched */
    uint8_t line_bitmap[32], line_attr[32];
    uint8_t border;                 /* port FE bits 0-2 */
    uint8_t border_ao, border_shown;  /* the border through the output latch (see ula.c) */
    bool mic, speaker;              /* port FE bits 3 and 4 */
    bool mreqt2, ioreqt2;           /* /MREQ and /IOREQ as seen while the CPU clock was low */
    /* Outputs, as last computed. */
    bool cpuclk, ras, cas, ras_oe, ae, we, romcs, irq;
    uint8_t addr;                   /* A0-A6 */
    uint8_t dlow;                   /* data pins pulled low (open collector), port reads */
    bool cpu_cas, cpu_cas_req;      /* CAS for a CPU cycle, and its request (see ula.c) */
    bool running;                   /* there is an oscillator */
    /* The tape: what comes into the EAR socket (set by the machine). */
    bool ear_in;
    uint32_t *fb;                   /* ULA_SCREEN_W x ULA_SCREEN_H */
    /* For checking the model against the gates: the inputs it saw at its
     * last step (bit 0 /MREQ, 1 /IOREQ, 2 /RD, 3 /WR, 4 A14, 5 A15, 6 A0,
     * 7 the EAR level it reads, 8-15 D0-D7, 16-20 KB0-KB4). */
    uint32_t inputs;
    uint8_t pix;                    /* the colour of the last pixel (bit 3 bright) */
} ula_state_t;

extern const part_type_t PT_ULA;
/* One half period of the 14 MHz oscillator (8 per CPU T-state). The part
 * is marked dirty when its outputs change. */
void ula_step(sim_t *s, part_t *p);
/* One line about the last step, for tools/ulacheck: time, inputs, outputs. */
void ula_log(const ula_state_t *u, uint64_t step, char *buf, int len);
/* Video level of the border/paper boundary etc. for the monitor. */
uint32_t ula_color(int index, bool bright);

#endif
