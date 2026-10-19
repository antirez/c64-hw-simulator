#pragma once
/*#
    # m6526.h

    MOS Technology 6526 Complex Interface Adapter (CIA)

    Do this:
    ~~~C
    #define CHIPS_IMPL
    ~~~
    before you include this file in *one* C or C++ file to create the
    implementation.

    Optionally provide the following macros with your own implementation
    ~~~C
    CHIPS_ASSERT(c)
    ~~~

    ## Emulated Pins

    ************************************
    *           +-----------+          *
    *    CS --->|           |<--- FLAG *
    *    RW --->|           |---> PC   *
    *   RES --->|           |---> SP   *
    *   IRQ <---|           |<--- TOD  *
    *           |           |<--- CNT  *
    *           |           |          *
    *   RS0 --->|   M6526   |<--> PA0  *
    *   RS1 --->|           |...       *
    *   RS2 --->|           |<--> PA7  *
    *   RS3 --->|           |          *
    *           |           |<--> PB0  *
    *   DB0 --->|           |...       *
    *        ...|           |<--> PB7  *
    *   DB7 --->|           |          *
    *           +-----------+          *
    ************************************

    ## NOT IMPLEMENTED:

    - no external counter trigger via CNT pin

    ALTERED VERSION, modified for c64-hw-sim: implemented the time of day
    clock (counted from the TOD pin, with the read latch, the write stop and
    the alarm interrupt), the serial port (SDR shifted on the SP and CNT
    pins) and the /PC handshake pin. This is not the original file by Andre
    Weissflog.

    ## LINKS:
    - https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html
    - https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html

    TODO: Documentation

    ## zlib/libpng license

    Copyright (c) 2018 Andre Weissflog
    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.
    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:
        1. The origin of this software must not be misrepresented; you must not
        claim that you wrote the original software. If you use this software in a
        product, an acknowledgment in the product documentation would be
        appreciated but is not required.
        2. Altered source versions must be plainly marked as such, and must not
        be misrepresented as being the original software.
        3. This notice may not be removed or altered from any source
        distribution.
#*/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// register select same as lower 4 shared address bus bits
#define M6526_PIN_RS0   (0)
#define M6526_PIN_RS1   (1)
#define M6526_PIN_RS2   (2)
#define M6526_PIN_RS3   (3)

// data bus pins shared with CPU
#define M6526_PIN_D0    (16)
#define M6526_PIN_D1    (17)
#define M6526_PIN_D2    (18)
#define M6526_PIN_D3    (19)
#define M6526_PIN_D4    (20)
#define M6526_PIN_D5    (21)
#define M6526_PIN_D6    (22)
#define M6526_PIN_D7    (23)

// control pins shared with CPU
#define M6526_PIN_RW    (24)      // same as M6502_RW
#define M6526_PIN_IRQ   (26)      // same as M6502_IRQ

// chip-specific control pins
#define M6526_PIN_CS    (40)
#define M6526_PIN_FLAG  (41)
#define M6526_PIN_PC    (42)
#define M6526_PIN_SP    (43)
#define M6526_PIN_TOD   (44)
#define M6526_PIN_CNT   (45)

// port I/O pins
#define M6526_PIN_PA0   (48)
#define M6526_PIN_PA1   (49)
#define M6526_PIN_PA2   (50)
#define M6526_PIN_PA3   (51)
#define M6526_PIN_PA4   (52)
#define M6526_PIN_PA5   (53)
#define M6526_PIN_PA6   (54)
#define M6526_PIN_PA7   (55)

#define M6526_PIN_PB0   (56)
#define M6526_PIN_PB1   (57)
#define M6526_PIN_PB2   (58)
#define M6526_PIN_PB3   (59)
#define M6526_PIN_PB4   (60)
#define M6526_PIN_PB5   (61)
#define M6526_PIN_PB6   (62)
#define M6526_PIN_PB7   (63)

// pin bit masks
#define M6526_RS0   (1ULL<<M6526_PIN_RS0)
#define M6526_RS1   (1ULL<<M6526_PIN_RS1)
#define M6526_RS2   (1ULL<<M6526_PIN_RS2)
#define M6526_RS3   (1ULL<<M6526_PIN_RS3)
#define M6526_RS    (M6526_RS3|M6526_RS2|M6526_RS1|M6526_RS0)
#define M6526_D0    (1ULL<<M6526_PIN_D0)
#define M6526_D1    (1ULL<<M6526_PIN_D1)
#define M6526_D2    (1ULL<<M6526_PIN_D2)
#define M6526_D3    (1ULL<<M6526_PIN_D3)
#define M6526_D4    (1ULL<<M6526_PIN_D4)
#define M6526_D5    (1ULL<<M6526_PIN_D5)
#define M6526_D6    (1ULL<<M6526_PIN_D6)
#define M6526_D7    (1ULL<<M6526_PIN_D7)
#define M6526_DB_PINS (0xFF0000ULL)
#define M6526_RW    (1ULL<<M6526_PIN_RW)
#define M6526_IRQ   (1ULL<<M6526_PIN_IRQ)
#define M6526_CS    (1ULL<<M6526_PIN_CS)
#define M6526_FLAG  (1ULL<<M6526_PIN_FLAG)
#define M6526_PC    (1ULL<<M6526_PIN_PC)
#define M6526_SP    (1ULL<<M6526_PIN_SP)
#define M6526_TOD   (1ULL<<M6526_PIN_TOD)
#define M6526_CNT   (1ULL<<M6526_PIN_CNT)
#define M6526_PA0   (1ULL<<M6526_PIN_PA0)
#define M6526_PA1   (1ULL<<M6526_PIN_PA1)
#define M6526_PA2   (1ULL<<M6526_PIN_PA2)
#define M6526_PA3   (1ULL<<M6526_PIN_PA3)
#define M6526_PA4   (1ULL<<M6526_PIN_PA4)
#define M6526_PA5   (1ULL<<M6526_PIN_PA5)
#define M6526_PA6   (1ULL<<M6526_PIN_PA6)
#define M6526_PA7   (1ULL<<M6526_PIN_PA7)
#define M6526_PA_PINS   (M6526_PA0|M6526_PA1|M6526_PA2|M6526_PA3|M6526_PA4|M6526_PA5|M6526_PA6|M6526_PA7)
#define M6526_PB0   (1ULL<<M6526_PIN_PB0)
#define M6526_PB1   (1ULL<<M6526_PIN_PB1)
#define M6526_PB2   (1ULL<<M6526_PIN_PB2)
#define M6526_PB3   (1ULL<<M6526_PIN_PB3)
#define M6526_PB4   (1ULL<<M6526_PIN_PB4)
#define M6526_PB5   (1ULL<<M6526_PIN_PB5)
#define M6526_PB6   (1ULL<<M6526_PIN_PB6)
#define M6526_PB7   (1ULL<<M6526_PIN_PB7)
#define M6526_PB_PINS   (M6526_PB0|M6526_PB1|M6526_PB2|M6526_PB3|M6526_PB4|M6526_PB5|M6526_PB6|M6526_PB7)

// register indices
#define M6526_REG_PRA       (0)     // peripheral data reg A
#define M6526_REG_PRB       (1)     // peripheral data reg B
#define M6526_REG_DDRA      (2)     // data direction reg A
#define M6526_REG_DDRB      (3)     // data direction reg B
#define M6526_REG_TALO      (4)     // timer A low register
#define M6526_REG_TAHI      (5)     // timer A high register
#define M6526_REG_TBLO      (6)     // timer B low register
#define M6526_REG_TBHI      (7)     // timer B high register
#define M6526_REG_TOD10TH   (8)     // 10ths of seconds register
#define M6526_REG_TODSEC    (9)     // seconds register
#define M6526_REG_TODMIN    (10)    // minutes register
#define M6526_REG_TODHR     (11)    // hours am/pm register
#define M6526_REG_SDR       (12)    // serial data register
#define M6526_REG_ICR       (13)    // interrupt control register
#define M6526_REG_CRA       (14)    // control register A
#define M6526_REG_CRB       (15)    // control register B

//--- control-register flag testing macros
#define M6526_TIMER_STARTED(cr)     (0!=((cr)&(1<<0)))
#define M6526_PBON(cr)              (0!=((cr)&(1<<1)))
#define M6526_OUTMODE_TOGGLE(cr)    (0!=((cr)&(1<<2)))
#define M6526_RUNMODE_ONESHOT(cr)   (0!=((cr)&(1<<3)))
#define M6526_FORCE_LOAD(cr)        (0!=((cr)&(1<<4)))
#define M6526_TA_INMODE_PHI2(cra)   (0==((cra)&(1<<5)))
#define M6526_TA_SPMODE_OUTPUT(cra) (0!=((cra)&(1<<6)))
#define M6526_TA_TODIN_50HZ(cra)    (0!=((cra)&(1<<7)))
#define M6526_TB_INMODE_PHI2(crb)   (0==((crb)&((1<<6)|(1<<5))))
#define M6526_TB_INMODE_CNT(crb)    ((1<<5)==((crb)&((1<<6)|(1<<5))))
#define M6526_TB_INMODE_TA(crb)     ((1<<6)==((crb)&((1<<6)|(1<<5))))
#define M6526_TB_INMODE_TACNT(crb)  (((1<<6)|(1<<5))==((crb)&((1<<6)|(1<<5))))
#define M6526_TB_ALARM_ALARM(crb)   (0!=((crb)&(1<<7)))

// delay-pipeline bit offsets
#define M6526_PIP_TIMER_COUNT     (0)
#define M6526_PIP_TIMER_ONESHOT   (8)
#define M6526_PIP_TIMER_LOAD      (16)

#define M6526_PIP_IRQ       (0)
#define M6526_PIP_READ_ICR  (8)

// I/O port state
typedef struct {
    uint8_t reg;    // port register
    uint8_t ddr;    // data direction register
    uint8_t inp;    // input latch
    uint8_t pins;   // current port pin state
} m6526_port_t;

// timer state
typedef struct {
    uint16_t latch;     // 16-bit initial value latch
    uint16_t counter;   // 16-bit counter
    uint8_t cr;         // control register
    bool t_bit;         // toggles between true and false when counter underflows
    bool t_out;         // true for 1 cycle when counter underflow
    /* merged delay-pipelines:
        2-cycle 'counter active':   bits 0..7
        1-cycle 'oneshot active':   bits 8..15
        1-cycle 'force load':       bits 16..24
    */
    uint32_t pip;
} m6526_timer_t;

// interrupt state
typedef struct {
    uint8_t imr;            // interrupt mask
    uint8_t imr1;           // one cycle delay for imr updates
    uint8_t icr;            // interrupt control register
    /* merged delay pipelines:
        1-cycle delay pipeline to request irq:  bits 0..7
        timer B bug: remember reads from ICR:   bits 8..15
    */
    uint32_t pip;
    bool flag;              // last state of flag bit, to detect edge
} m6526_int_t;

// time of day clock state (c64-hw-sim)
typedef struct {
    uint8_t time[4];        // 10ths, seconds, minutes, hours (BCD, hours bit 7 = PM)
    uint8_t alarm[4];
    uint8_t latch[4];       // what the reads return while latched
    bool latched;           // reading hours froze the read values, until 10ths is read
    bool stopped;           // writing hours stopped the clock, until 10ths is written
    bool match;             // the clock equals the alarm
    bool irq;               // alarm interrupt to raise
    uint8_t prescaler;      // TOD pin edges toward the next 10th (5 at 50 Hz, 6 at 60 Hz)
    bool pin;               // last TOD pin level, to see its rising edges
} m6526_tod_t;

// serial port state (c64-hw-sim)
typedef struct {
    uint8_t sdr;            // serial data register
    uint8_t shift;          // shift register
    uint8_t bits;           // bits shifted so far
    bool busy;              // output mode: a byte is being shifted out
    bool loaded;            // output mode: SDR written, waiting to be shifted out
    bool cnt_out, sp_out;   // output mode: the levels driven on CNT and SP
    bool cnt_in;            // last CNT pin level, to see its rising edges
    bool irq;               // serial interrupt to raise
} m6526_sp_t;

// m6526 state
typedef struct {
    m6526_port_t pa;
    m6526_port_t pb;
    m6526_timer_t ta;
    m6526_timer_t tb;
    m6526_int_t intr;
    m6526_tod_t tod;
    m6526_sp_t sp;
    bool pc_low;            // /PC is low in this cycle (c64-hw-sim)
    bool pc_next;           // port B was accessed: /PC goes low in the next cycle
    uint64_t pins;
} m6526_t;

// extract 8-bit data bus from 64-bit pins
#define M6526_GET_DATA(p) ((uint8_t)((p)>>16))
// merge 8-bit data bus value into 64-bit pins
#define M6526_SET_DATA(p,d) {p=(((p)&~0xFF0000ULL)|(((d)<<16)&0xFF0000ULL));}
// extract port A pins
#define M6526_GET_PA(p) ((uint8_t)((p)>>48))
// extract port B pins
#define M6526_GET_PB(p) ((uint8_t)((p)>>56))
// merge port A pins into pin mask
#define M6526_SET_PA(p,a) {p=((p)&0xFF00FFFFFFFFFFFFULL)|(((a)&0xFFULL)<<48);}
// merge port B pins into pin mask
#define M6526_SET_PB(p,b) {p=((p)&0x00FFFFFFFFFFFFFFULL)|(((b)&0xFFULL)<<56);}
// merge port A and B pins into pin mask
#define M6526_SET_PAB(p,a,b) {p=((p)&0x0000FFFFFFFFFFFFULL)|(((a)&0xFFULL)<<48)|(((b)&0xFFULL)<<56);}

// initialize a new m6526_t instance
void m6526_init(m6526_t* c);
// reset an existing m6526_t instance
void m6526_reset(m6526_t* c);
// tick the m6526_t instance
uint64_t m6526_tick(m6526_t* c, uint64_t pins);

#ifdef __cplusplus
} // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
    #include <assert.h>
    #define CHIPS_ASSERT(c) assert(c)
#endif

static void _m6526_init_port(m6526_port_t* p) {
    p->reg = 0;
    p->ddr = 0;
    p->inp = 0;
    p->pins = 0;
}

static void _m6526_init_timer(m6526_timer_t* t) {
    t->latch = 0xFFFF;
    t->counter = 0;
    t->cr = 0;
    t->t_bit = 0;
    t->t_out = 0;
    t->pip = 0;
}

static void _m6526_init_interrupt(m6526_int_t* intr) {
    intr->imr = 0;
    intr->imr1 = 0;
    intr->icr = 0;
    intr->pip = 0;
    intr->flag = false;
}

void m6526_init(m6526_t* c) {
    CHIPS_ASSERT(c);
    memset(c, 0, sizeof(*c));
    _m6526_init_port(&c->pa);
    _m6526_init_port(&c->pb);
    _m6526_init_timer(&c->ta);
    _m6526_init_timer(&c->tb);
    _m6526_init_interrupt(&c->intr);
    c->ta.latch = 0xFFFF;
    c->tb.latch = 0xFFFF;
    c->sp.cnt_out = c->sp.sp_out = true;
    c->sp.cnt_in = true;
}

void m6526_reset(m6526_t* c) {
    CHIPS_ASSERT(c);
    _m6526_init_port(&c->pa);
    _m6526_init_port(&c->pb);
    _m6526_init_timer(&c->ta);
    _m6526_init_timer(&c->tb);
    _m6526_init_interrupt(&c->intr);
    /* c64-hw-sim: "all other registers are reset to zero" (data sheet),
       the time of day clock and its alarm too; the serial port is idle,
       CNT high */
    memset(&c->tod, 0, sizeof(c->tod));
    memset(&c->sp, 0, sizeof(c->sp));
    c->sp.cnt_out = c->sp.sp_out = true;
    c->sp.cnt_in = true;    /* CNT idles high (pull-up): no edge at power on */
    c->pc_low = c->pc_next = false;
    c->pins = 0;
}

/*--- delay-pipeline macros ---*/
/* set or clear a new state at pipeline pos */
#define _M6526_PIP_SET(pip,offset,pos) {pip|=(1<<(offset+pos));}
#define _M6526_PIP_CLR(pip,offset,pos) {pip&=~(1<<(offset+pos));}
/* reset an entire pipeline */
#define _M6526_PIP_RESET(pip,offset) {pip&=~(0xFF<<offset);}
/* test pipeline state, pos 0 is the 'output bit' */
#define _M6526_PIP_TEST(pip,offset,pos) (0!=(pip&(1<<(offset+pos))))

/*--- port implementation ---*/
static inline void _m6526_read_port_pins(m6526_t* c, uint64_t pins) {
    c->pa.inp = M6526_GET_PA(pins);
    c->pb.inp = M6526_GET_PB(pins);
}

static inline uint8_t _m6526_merge_pb67(m6526_t* c, uint8_t data) {
    /* merge timer state bits into data byte */
    if (M6526_PBON(c->ta.cr)) {
        data &= ~(1<<6);
        if (M6526_OUTMODE_TOGGLE(c->ta.cr)) {
            /* timer A toggle state into bit 6 */
            if (c->ta.t_bit) {
                data |= (1<<6);
            }
        }
        else {
            /* timer A output state into bit 6 */
            if (c->ta.t_out) {
                data |= (1<<6);
            }
        }
    }
    if (M6526_PBON(c->tb.cr)) {
        data &= ~(1<<7);
        if (M6526_OUTMODE_TOGGLE(c->tb.cr)) {
            /* timer B toggle state into bit 7 */
            if (c->tb.t_bit) {
                data |= (1<<7);
            }
        }
        else {
            /* timer B output state into bit 7 */
            if (c->tb.t_out) {
                data |= (1<<7);
            }
        }
    }
    return data;
}

static inline uint64_t _m6526_write_port_pins(m6526_t* c, uint64_t pins) {
    c->pa.pins = c->pa.reg | (c->pa.inp & ~c->pa.ddr);
    c->pb.pins = _m6526_merge_pb67(c, c->pb.reg | (c->pb.inp & ~c->pb.ddr));
    M6526_SET_PAB(pins, c->pa.pins, c->pb.pins);
    return pins;
}

/*--- interrupt implementation ---*/
static void _m6526_write_icr(m6526_t* c, uint8_t data) {
    /* from datasheet: When writing to the MASK register, if bit 7 (SET/CLEAR)
       of data written is a ZERO, any mask bit written with a on will be cleared,
       while those mask bits written with a zero will be unaffected. If bit 7
       of the data written is a ONE, any mask bit written with a one will be set,
       while those mask bits written with a zero will be unaffected.
    */
    if (data & (1<<7)) {
        c->intr.imr1 |= (data & 0x1F);
    }
    else {
        c->intr.imr1 &= ~(data & 0x1F);
    }
    /* from WLorenz Test Suite readme:
        When a condition in the ICR is true, setting the corresponding bit in
        the IMR must also set the interrupt. Clearing the bit in the IMR may
        not clear the interrupt. Only reading the ICR may clear the interrupt.
    */
    if (c->intr.icr & c->intr.imr1) {
        _M6526_PIP_SET(c->intr.pip, M6526_PIP_IRQ, 1);
    }
}

static uint8_t _m6526_read_icr(m6526_t* c) {
    /* the icr register is cleared after reading, this will also cause the
       IRQ line to go inactive, also the irq 1-cycle-delay pipeline is
       set to cleared state.
       see Figure 5 https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html
    */
    uint8_t data = c->intr.icr;
    c->intr.icr = 0;
    /* cancel an interrupt pending in the pipeline */
    _M6526_PIP_RESET(c->intr.pip, M6526_PIP_IRQ)
    /* remember reads from ICR to implement "Timer B Bug" */
    _M6526_PIP_SET(c->intr.pip, M6526_PIP_READ_ICR, 0);
    return data;
}

/*--- time of day clock (c64-hw-sim) ---*/

/* One more in BCD: the low digit carries into the high one after 9. */
static inline uint8_t _m6526_bcd_inc(uint8_t v) {
    return ((v & 0x0F) == 9) ? (uint8_t)((v & 0xF0) + 0x10) : (uint8_t)(v + 1);
}

/* The alarm interrupt fires when the clock becomes equal to the alarm. */
static void _m6526_tod_check_alarm(m6526_tod_t* t) {
    bool match = 0 == memcmp(t->time, t->alarm, 4);
    if (match && !t->match) {
        t->irq = true;
    }
    t->match = match;
}

/* The TOD pin gets 50 or 60 Hz from the mains: every 5 or 6 rising edges
   (CRA bit 7 says which) the clock counts a tenth of a second. */
static void _m6526_tick_tod(m6526_t* c, uint64_t pins) {
    m6526_tod_t* t = &c->tod;
    bool pin = 0 != (pins & M6526_TOD);
    bool rise = pin && !t->pin;
    t->pin = pin;
    if (!rise || t->stopped) {
        return;
    }
    if (++t->prescaler < (M6526_TA_TODIN_50HZ(c->ta.cr) ? 5 : 6)) {
        return;
    }
    t->prescaler = 0;
    if (t->time[0] != 9) {
        t->time[0] = (t->time[0] + 1) & 0x0F;
    }
    else {
        t->time[0] = 0;
        if (t->time[1] != 0x59) {
            t->time[1] = _m6526_bcd_inc(t->time[1]) & 0x7F;
        }
        else {
            t->time[1] = 0;
            if (t->time[2] != 0x59) {
                t->time[2] = _m6526_bcd_inc(t->time[2]) & 0x7F;
            }
            else {
                /* hours 1..12, AM/PM flips going from 11 to 12 */
                t->time[2] = 0;
                uint8_t h = t->time[3] & 0x1F, pm = t->time[3] & 0x80;
                if (h == 0x11) {
                    h = 0x12;
                    pm ^= 0x80;
                }
                else if (h == 0x12) {
                    h = 0x01;
                }
                else {
                    h = _m6526_bcd_inc(h) & 0x1F;
                }
                t->time[3] = pm | h;
            }
        }
    }
    _m6526_tod_check_alarm(t);
}

/* Reading the hours latches all four registers until the 10ths are read,
   so a time read hours first is consistent. */
static uint8_t _m6526_read_tod(m6526_t* c, uint8_t addr) {
    m6526_tod_t* t = &c->tod;
    int i = addr - M6526_REG_TOD10TH;
    if ((i == 3) && !t->latched) {
        memcpy(t->latch, t->time, 4);
        t->latched = true;
    }
    uint8_t data = t->latched ? t->latch[i] : t->time[i];
    if (i == 0) {
        t->latched = false;
    }
    return data;
}

/* With CRB bit 7 set the writes go to the alarm. Writing the hours stops
   the clock until the 10ths are written, so a time set hours first starts
   all at once. Writing 12 to the hours flips AM/PM: a quirk of the real
   chip (as documented by VICE). */
static void _m6526_write_tod(m6526_t* c, uint8_t addr, uint8_t data) {
    static const uint8_t mask[4] = { 0x0F, 0x7F, 0x7F, 0x9F };
    m6526_tod_t* t = &c->tod;
    int i = addr - M6526_REG_TOD10TH;
    data &= mask[i];
    if (c->tb.cr & (1<<7)) {
        t->alarm[i] = data;
    }
    else {
        if (i == 3) {
            if ((data & 0x1F) == 0x12) {
                data ^= 0x80;
            }
            t->stopped = true;
        }
        else if (i == 0) {
            t->stopped = false;
            t->prescaler = 0;
        }
        t->time[i] = data;
    }
    _m6526_tod_check_alarm(t);
}

/*--- serial port (c64-hw-sim) ---*/

/* CRA bit 6 selects the direction. Output: after a write to the SDR the
   byte is shifted out MSB first at half the timer A underflow rate, CNT
   is the clock (the data changes when it falls, the receiver takes it when
   it rises); after 8 bits the serial interrupt fires, and a byte written
   meanwhile follows. Input: SP is shifted in on every rising edge of CNT,
   after 8 bits the byte goes to the SDR and the interrupt fires. */
static void _m6526_tick_sp(m6526_t* c, uint64_t pins) {
    m6526_sp_t* s = &c->sp;
    bool cnt = 0 != (pins & M6526_CNT);
    if (c->ta.cr & (1<<6)) {
        if (!s->busy && s->loaded) {
            s->shift = s->sdr;
            s->loaded = false;
            s->busy = true;
            s->bits = 0;
        }
        if (s->busy && c->ta.t_out) {
            if (s->cnt_out) {
                s->cnt_out = false;
                s->sp_out = 0 != (s->shift & 0x80);
                s->shift <<= 1;
            }
            else {
                s->cnt_out = true;
                if (++s->bits == 8) {
                    s->irq = true;
                    s->busy = false;
                }
            }
        }
    }
    else if (cnt && !s->cnt_in) {
        s->shift = (uint8_t)((s->shift << 1) | ((pins & M6526_SP) ? 1 : 0));
        if (++s->bits == 8) {
            s->sdr = s->shift;
            s->bits = 0;
            s->irq = true;
        }
    }
    s->cnt_in = cnt;
}

static uint64_t _m6526_update_irq(m6526_t* c, uint64_t pins) {
    /* see Figure 5 https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html */

    /* timer A underflow interrupt? */
    if (c->ta.t_out) {
        c->intr.icr |= (1<<0);
    }
    /* timer B underflow interrupt flag? */
    if (c->tb.t_out) {
        /* "Timer B Bug": reads from ICR block timer B interrupt generation */
        if (!_M6526_PIP_TEST(c->intr.pip, M6526_PIP_READ_ICR, 0)) {
            c->intr.icr |= (1<<1);
        }
    }
    /* check for FLAG pin trigger */
    if ((pins & M6526_FLAG) && (!c->intr.flag)) {
        c->intr.icr |= (1<<4);
    }
    c->intr.flag = 0 != (pins & M6526_FLAG);

    /* time of day alarm (c64-hw-sim) */
    if (c->tod.irq) {
        c->intr.icr |= (1<<2);
        c->tod.irq = false;
    }

    /* serial port (c64-hw-sim) */
    if (c->sp.irq) {
        c->intr.icr |= (1<<3);
        c->sp.irq = false;
    }

    /* handle main interrupt bit */
    if (_M6526_PIP_TEST(c->intr.pip, M6526_PIP_IRQ, 0)) {
        c->intr.icr |= (1<<7);
    }

    /* update IRQ pin */
    if (0 != (c->intr.icr & (1<<7))) {
        pins |= M6526_IRQ;
    }
    else {
        pins &= ~M6526_IRQ;
    }
    return pins;
}

/*--- timer implementation ---*/

/* generic timer tick, doesn't handle counter pipeline input
   (since this is different for timer A and B)
   check here for the details: https://ist.uwaterloo.ca/~schepers/MJK/cia6526.html
*/
static void _m6526_tick_timer(m6526_timer_t* t) {
    /* decrement counter? */
    if (_M6526_PIP_TEST(t->pip, M6526_PIP_TIMER_COUNT, 0)) {
        t->counter--;
    }

    /* timer undeflow? */
    t->t_out = (0 == t->counter) && _M6526_PIP_TEST(t->pip, M6526_PIP_TIMER_COUNT, 1);
    if (t->t_out) {
        t->t_bit = !t->t_bit;
        /* reset started flag if in one-shot mode */
        if (M6526_RUNMODE_ONESHOT(t->cr) || _M6526_PIP_TEST(t->pip, M6526_PIP_TIMER_ONESHOT, 0)) {
            t->cr &= ~(1<<0);
        }
        _M6526_PIP_SET(t->pip, M6526_PIP_TIMER_LOAD, 0);
    }

    /* reload counter from latch? */
    if (_M6526_PIP_TEST(t->pip, M6526_PIP_TIMER_LOAD, 0)) {
        t->counter = t->latch;
        _M6526_PIP_CLR(t->pip, M6526_PIP_TIMER_COUNT, 1);
    }
}

static void _m6526_tick_pipeline(m6526_t* c) {
    /* timer A counter pipeline (FIXME: CNT) */
    if (M6526_TA_INMODE_PHI2(c->ta.cr)) {
        _M6526_PIP_SET(c->ta.pip, M6526_PIP_TIMER_COUNT, 2);
    }
    if (!M6526_TIMER_STARTED(c->ta.cr)) {
        _M6526_PIP_CLR(c->ta.pip, M6526_PIP_TIMER_COUNT, 2);
    }
    /* timer A load-from-latch pipeline */
    if (M6526_FORCE_LOAD(c->ta.cr)) {
        _M6526_PIP_SET(c->ta.pip, M6526_PIP_TIMER_LOAD, 1);
        c->ta.cr &= ~(1<<4);
    }
    /* timer A oneshot pipeline */
    if (M6526_RUNMODE_ONESHOT(c->ta.cr)) {
        _M6526_PIP_SET(c->ta.pip, M6526_PIP_TIMER_ONESHOT, 1);
    }

    /* timer B counter pipeline (FIMXE: CNT) */
    bool tb_active = false;
    if (M6526_TB_INMODE_PHI2(c->tb.cr)) {
        tb_active = true;
    }
    else if (M6526_TB_INMODE_TA(c->tb.cr)) {
        tb_active = c->ta.t_out;
    }
    else if (M6526_TB_INMODE_CNT(c->tb.cr)) {
        tb_active = false;   // FIXME: CNT always high for now
    }
    else if (M6526_TB_INMODE_TACNT(c->tb.cr)) {
        tb_active = c->ta.t_out; // FIXME: CNT always high for now
    }
    if (tb_active) {
        _M6526_PIP_SET(c->tb.pip, M6526_PIP_TIMER_COUNT, 2);
    }
    else {
        _M6526_PIP_CLR(c->tb.pip, M6526_PIP_TIMER_COUNT, 2);
    }
    if (!M6526_TIMER_STARTED(c->tb.cr)) {
        _M6526_PIP_CLR(c->tb.pip, M6526_PIP_TIMER_COUNT, 2);
    }

    /* timer B load-from-latch pipeline */
    if (M6526_FORCE_LOAD(c->tb.cr)) {
        _M6526_PIP_SET(c->tb.pip, M6526_PIP_TIMER_LOAD, 1);
        c->tb.cr &= ~(1<<4);
    }

    /* timer B oneshot pipeline */
    if (M6526_RUNMODE_ONESHOT(c->tb.cr)) {
        _M6526_PIP_SET(c->tb.pip, M6526_PIP_TIMER_ONESHOT, 1);
    }

    /* interrupt pipeline */
    if (c->intr.icr & c->intr.imr) {
        _M6526_PIP_SET(c->intr.pip, M6526_PIP_IRQ, 1);
    }
    c->intr.imr = c->intr.imr1;

    /* tick pipelines forward */
    c->ta.pip = (c->ta.pip >> 1) & 0x7F7F7F7F;
    c->tb.pip = (c->tb.pip >> 1) & 0x7F7F7F7F;
    c->intr.pip = (c->intr.pip >> 1) & 0x7F7F7F7F;
}

static uint64_t _m6526_tick(m6526_t* c, uint64_t pins) {
    _m6526_read_port_pins(c, pins);
    _m6526_tick_timer(&c->ta);
    _m6526_tick_timer(&c->tb);
    _m6526_tick_tod(c, pins);
    _m6526_tick_sp(c, pins);
    pins = _m6526_update_irq(c, pins);
    pins = _m6526_write_port_pins(c, pins);
    _m6526_tick_pipeline(c);
    return pins;
}

static inline void _m6526_write_cr(m6526_timer_t* t, uint8_t data) {
    /* if the start bit goes from 0 to 1, set the current toggle-bit-state to 1 */
    if (!M6526_TIMER_STARTED(t->cr) && M6526_TIMER_STARTED(data)) {
        t->t_bit = true;
    }
    t->cr = data;
}

/* read a register */
static uint8_t _m6526_read(m6526_t* c, uint8_t addr) {
    uint8_t data = 0xFF;
    switch (addr) {
        case M6526_REG_PRA:
            data = c->pa.inp;
            break;
        case M6526_REG_PRB:
            data = _m6526_merge_pb67(c, c->pb.inp);
            break;
        case M6526_REG_DDRA:
            data = c->pa.ddr;
            break;
        case M6526_REG_DDRB:
            data = c->pb.ddr;
            break;
        case M6526_REG_TALO:
            data = c->ta.counter & 0xFF;
            break;
        case M6526_REG_TAHI:
            data = c->ta.counter >> 8;
            break;
        case M6526_REG_TBLO:
            data = c->tb.counter & 0xFF;
            break;
        case M6526_REG_TBHI:
            data = c->tb.counter >> 8;
            break;
        case M6526_REG_ICR:
            data = _m6526_read_icr(c);
            break;
        case M6526_REG_CRA:
            /* force-load bit always returns zero */
            data = c->ta.cr & ~(1<<4);
            break;
        case M6526_REG_CRB:
            /* force-load bit always returns zero */
            data = c->tb.cr & ~(1<<4);
            break;
        case M6526_REG_TOD10TH:
        case M6526_REG_TODSEC:
        case M6526_REG_TODMIN:
        case M6526_REG_TODHR:
            data = _m6526_read_tod(c, addr);
            break;
        case M6526_REG_SDR:
            data = c->sp.sdr;
            break;
    }
    return data;
}

static void _m6526_write(m6526_t* c, uint8_t addr, uint8_t data) {
    switch (addr) {
        case M6526_REG_PRA:
            c->pa.reg = data;
            break;
        case M6526_REG_PRB:
            c->pb.reg = data;
            break;
        case M6526_REG_DDRA:
            c->pa.ddr = data;
            break;
        case M6526_REG_DDRB:
            c->pb.ddr = data;
            break;
        case M6526_REG_TALO:
            c->ta.latch = (c->ta.latch & 0xFF00) | data;
            break;
        case M6526_REG_TAHI:
            c->ta.latch = (data<<8) | (c->ta.latch & 0x00FF);
            /* if timer is not running, writing hi-byte load counter form latch */
            if (!M6526_TIMER_STARTED(c->ta.cr)) {
                _M6526_PIP_SET(c->ta.pip, M6526_PIP_TIMER_LOAD, 1);
            }
            break;
        case M6526_REG_TBLO:
            c->tb.latch = (c->tb.latch & 0xFF00) | data;
            break;
        case M6526_REG_TBHI:
            c->tb.latch = (data<<8) | (c->tb.latch & 0x00FF);
            /* if timer is not running, writing hi-byte writes latch */
            if (!M6526_TIMER_STARTED(c->tb.cr)) {
                _M6526_PIP_SET(c->tb.pip, M6526_PIP_TIMER_LOAD, 1);
            }
            break;
        case M6526_REG_ICR:
            _m6526_write_icr(c, data);
            break;
        case M6526_REG_CRA:
            /* c64-hw-sim: changing the serial direction stops the shifter */
            if ((c->ta.cr ^ data) & (1<<6)) {
                c->sp.busy = c->sp.loaded = false;
                c->sp.bits = 0;
                c->sp.cnt_out = true;
            }
            _m6526_write_cr(&c->ta, data);
            break;
        case M6526_REG_CRB:
            _m6526_write_cr(&c->tb, data);
            break;
        case M6526_REG_TOD10TH:
        case M6526_REG_TODSEC:
        case M6526_REG_TODMIN:
        case M6526_REG_TODHR:
            _m6526_write_tod(c, addr, data);
            break;
        case M6526_REG_SDR:
            c->sp.sdr = data;
            if (c->ta.cr & (1<<6)) {
                c->sp.loaded = true;
            }
            break;
    }
}

uint64_t m6526_tick(m6526_t* c, uint64_t pins) {
    /* c64-hw-sim: /PC is low for one cycle following an access to port B */
    c->pc_low = c->pc_next;
    c->pc_next = false;
    pins = _m6526_tick(c, pins);
    if (pins & M6526_CS) {
        uint8_t addr = pins & M6526_RS;
        if (addr == M6526_REG_PRB) {
            c->pc_next = true;
        }
        if (pins & M6526_RW) {
            uint8_t data = _m6526_read(c, addr);
            M6526_SET_DATA(pins, data);
        }
        else {
            uint8_t data = M6526_GET_DATA(pins);
            _m6526_write(c, addr, data);
        }
    }
    c->pins = pins;
    return pins;
}

#endif /* CHIPS_IMPL */
