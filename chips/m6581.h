#pragma once
/*#
    # m6581.h

    MOS Technology 6581 emulator (aka SID)

    based on tedplay (c) 2012 Attila Grosz, used under Unlicense http://unlicense.org/

    Modified for c64-llm; see docs/sid.md for changes and hardware references.

    ALTERED VERSION, modified again for c64-hw-sim: the POT X/Y registers
    return the values the board measured (pot[], set by the board).

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

    ***********************************
    *           +-----------+         *
    *    CS --->|           |<--- A0  *
    *    RW --->|           |...      *
    *           |           |<--- A4  *
    *           |   m6581   |         *
    *           |           |<--> D0  *
    *           |           |...      *
    *           |           |<--> D7  *
    *           |           |         *
    *           +-----------+         *
    ***********************************

    The emulation has an additional "virtual pin" which is set to active
    whenever a new sample is ready (M6581_SAMPLE).

    ## Links

    - http://blog.kevtris.org/?p=13

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

// address bus pins A0..A4
#define M6581_PIN_A0    (0)
#define M6581_PIN_A1    (1)
#define M6581_PIN_A2    (2)
#define M6581_PIN_A3    (3)
#define M6581_PIN_A4    (4)

// data bus pins D0..D7
#define M6581_PIN_D0    (16)
#define M6581_PIN_D1    (17)
#define M6581_PIN_D2    (18)
#define M6581_PIN_D3    (19)
#define M6581_PIN_D4    (20)
#define M6581_PIN_D5    (21)
#define M6581_PIN_D6    (22)
#define M6581_PIN_D7    (23)

// shared control pins
#define M6581_PIN_RW    (24)      /* same as M6502_RW */

// chip-specific pins
#define M6581_PIN_CS        (40)      /* chip-select */
#define M6581_PIN_SAMPLE    (41)      /* virtual "audio sample ready" pin */

// pin bit masks
#define M6581_A0    (1ULL<<M6581_PIN_A0)
#define M6581_A1    (1ULL<<M6581_PIN_A1)
#define M6581_A2    (1ULL<<M6581_PIN_A2)
#define M6581_A3    (1ULL<<M6581_PIN_A3)
#define M6581_A4    (1ULL<<M6581_PIN_A4)
#define M6581_ADDR_MASK (0x1F)
#define M6581_D0    (1ULL<<M6581_PIN_D0)
#define M6581_D1    (1ULL<<M6581_PIN_D1)
#define M6581_D2    (1ULL<<M6581_PIN_D2)
#define M6581_D3    (1ULL<<M6581_PIN_D3)
#define M6581_D4    (1ULL<<M6581_PIN_D4)
#define M6581_D5    (1ULL<<M6581_PIN_D5)
#define M6581_D6    (1ULL<<M6581_PIN_D6)
#define M6581_D7    (1ULL<<M6581_PIN_D7)
#define M6581_RW    (1ULL<<M6581_PIN_RW)
#define M6581_CS        (1ULL<<M6581_PIN_CS)
#define M6581_SAMPLE    (1ULL<<M6581_PIN_SAMPLE)

// registers
#define M6581_V1_FREQ_LO    (0)
#define M6581_V1_FREQ_HI    (1)
#define M6581_V1_PW_LO      (2)
#define M6581_V1_PW_HI      (3)
#define M6581_V1_CTRL       (4)
#define M6581_V1_ATKDEC     (5)
#define M6581_V1_SUSREL     (6)
#define M6581_V2_FREQ_LO    (7)
#define M6581_V2_FREQ_HI    (8)
#define M6581_V2_PW_LO      (9)
#define M6581_V2_PW_HI      (10)
#define M6581_V2_CTRL       (11)
#define M6581_V2_ATKDEC     (12)
#define M6581_V2_SUSREL     (13)
#define M6581_V3_FREQ_LO    (14)
#define M6581_V3_FREQ_HI    (15)
#define M6581_V3_PW_LO      (16)
#define M6581_V3_PW_HI      (17)
#define M6581_V3_CTRL       (18)
#define M6581_V3_ATKDEC     (19)
#define M6581_V3_SUSREL     (20)
#define M6581_FC_LO         (21)
#define M6581_FC_HI         (22)
#define M6581_RES_FILT      (23)
#define M6581_MODE_VOL      (24)
#define M6581_POT_X         (25)
#define M6581_POT_Y         (26)
#define M6581_OSC3RAND      (27)
#define M6581_ENV3          (28)
#define M6581_INV_0         (29)
#define M6581_INV_1         (30)
#define M6581_INV_2         (31)
#define M6581_NUM_REGS (32)

// voice control bits
#define M6581_CTRL_GATE     (1<<0)
#define M6581_CTRL_SYNC     (1<<1)
#define M6581_CTRL_RINGMOD  (1<<2)
#define M6581_CTRL_TEST     (1<<3)
#define M6581_CTRL_TRIANGLE (1<<4)
#define M6581_CTRL_SAWTOOTH (1<<5)
#define M6581_CTRL_PULSE    (1<<6)
#define M6581_CTRL_NOISE    (1<<7)

// filter routing bits
#define M6581_FILTER_FILT1  (1<<0)
#define M6581_FILTER_FILT2  (1<<1)
#define M6581_FILTER_FILT3  (1<<2)
#define M6581_FILTER_FILTEX (1<<3)

// filter mode bits
#define M6581_FILTER_LP     (1<<0)
#define M6581_FILTER_BP     (1<<1)
#define M6581_FILTER_HP     (1<<2)
#define M6581_FILTER_3OFF   (1<<3)

// setup parameters for m6581_init()
typedef struct {
    int tick_hz;        // frequency at which m6581_tick() will be called in Hz
    int sound_hz;       // sound sample frequency
    float magnitude;    // output sample magnitude (0=silence to 1=max volume)
} m6581_desc_t;

// envelope generator state
typedef enum {
    M6581_ENV_FROZEN,
    M6581_ENV_ATTACK,
    M6581_ENV_DECAY,
    M6581_ENV_RELEASE
} m6581_env_state_t;

// voice state
typedef struct {
    bool muted;
    // wave generator state
    uint16_t freq;
    uint16_t pulse_width;
    uint8_t ctrl;
    bool sync;
    uint32_t noise_shift;           // 23 bit
    uint32_t noise_latch;
    uint32_t noise_reset;
    uint8_t noise_delay;
    uint32_t wav_accum;             // 8.16 fixed
    uint32_t wav_output;            // 12 bit
    uint16_t pulse_output;          // previous cycle's comparator output

    // envelope generator state
    m6581_env_state_t env_state;
    uint32_t env_attack_add;
    uint32_t env_decay_sub;
    uint32_t env_sustain_level;
    uint32_t env_release_sub;
    uint32_t env_cur_level;
    uint32_t env_counter;
    uint32_t env_exp_counter;
    uint32_t env_counter_compare;
    uint8_t env_exp_period;
    bool env_hold_zero;
} m6581_voice_t;

// filter state
typedef struct {
    uint16_t cutoff;
    uint8_t resonance;
    uint8_t voices;
    uint8_t mode;
    uint8_t volume;
    int tick_hz;
    float resonance_coeff;
    float w0;
    double v_hp;
    double v_bp;
    double v_lp;
} m6581_filter_t;

#define M6581_SAMPLE_BUFFER_SIZE (1024)

// m6581 instance state
typedef struct {
    int sound_hz;
    int tick_hz;
    /* reading a write-only register returns the last value
       read or written for about 0x2000 ticks
    */
    uint8_t bus_value;
    uint16_t bus_decay;
    /* c64-hw-sim: POT X/Y, as measured on the POT pins by the board */
    uint8_t pot[2];
    // voice state
    m6581_voice_t voice[3];
    // filter state
    m6581_filter_t filter;
    // sample generation state
    uint32_t sample_phase;
    int sample_pos;
    float sample_buffer[M6581_SAMPLE_BUFFER_SIZE * 2];
    float sample_filter[M6581_SAMPLE_BUFFER_SIZE - 1];
    double ext_lp;
    double ext_hp;
    float ext_lp_coeff;
    float ext_hp_coeff;
    float sample_mag;
    float sample;
    // debug inspection
    uint64_t pins;
} m6581_t;

// initialize a new m6581_t instance
void m6581_init(m6581_t* sid, const m6581_desc_t* desc);
// reset a m6581_t instance
void m6581_reset(m6581_t* sid);
// tick a m6581_t instance
uint64_t m6581_tick(m6581_t* sid, uint64_t pins);

#ifdef __cplusplus
} // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>
#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif
#include <math.h>
#ifndef CHIPS_ASSERT
    #include <assert.h>
    #define CHIPS_ASSERT(c) assert(c)
#endif

/* extract 8-bit data bus from 64-bit pins */
#define M6581_GET_DATA(p) ((uint8_t)(((p)&0xFF0000ULL)>>16))
/* merge 8-bit data bus value into 64-bit pins */
#define M6581_SET_DATA(p,d) {p=(((p)&~0xFF0000ULL)|(((d)<<16)&0xFF0000ULL));}
/* move bit into first position */
#define M6581_BIT(val,bitnr) ((val>>bitnr)&1)
/* nominal analog offsets in volts */
#define M6581_DCWAVE (0.40f)
#define M6581_DCMIXER (-0.04f)
#define M6581_DCVOICE (0.26f)

static const uint32_t _m6581_rate_count_period[16] = {
    0x7F00, 0x0006, 0x003C, 0x0330, 0x20C0, 0x6755, 0x3800, 0x500E,
    0x1212, 0x0222, 0x1848, 0x59B8, 0x3840, 0x77E2, 0x7625, 0x0A93
};

static void _m6581_init_voice(m6581_voice_t* v) {
    memset(v, 0, sizeof(*v));
    v->noise_shift = 0x007FFFFC;
    v->env_state = M6581_ENV_RELEASE;
    v->env_exp_period = 1;
    v->env_hold_zero = true;
    v->env_counter = 0x7FFF;
}

static void _m6581_set_filter_cutoff(m6581_filter_t*);
static void _m6581_set_resonance(m6581_filter_t*);

static void _m6581_init_filter(m6581_filter_t* f, int tick_hz) {
    memset(f, 0, sizeof(*f));
    f->tick_hz = tick_hz;
    _m6581_set_filter_cutoff(f);
    _m6581_set_resonance(f);
}

static void _m6581_init_sampling(m6581_t* sid) {
    const int n = M6581_SAMPLE_BUFFER_SIZE - 1;
    const double pi = 3.14159265358979323846;
    double cutoff = 0.4 * sid->sound_hz / sid->tick_hz;
    double sum = 0.0;
    /* Blackman-windowed sinc. Leave a transition band below Nyquist. */
    for (int i = 0; i < n; i++) {
        double x = i - (n - 1) * 0.5;
        double sinc = x == 0.0 ? 2.0 * cutoff : sin(2.0 * pi * cutoff * x) / (pi * x);
        double phase = 2.0 * pi * i / (n - 1);
        double window = 0.42 - 0.5 * cos(phase) + 0.08 * cos(2.0 * phase);
        sid->sample_filter[i] = (float)(sinc * window);
        sum += sinc * window;
    }
    for (int i = 0; i < n; i++) {
        sid->sample_filter[i] /= (float)sum;
    }
    /* C64 output low-pass and AC coupling, independent of the SID filter. */
    sid->ext_lp_coeff = (float)(1.0 - exp(-2.0 * pi * 16000.0 / sid->tick_hz));
    sid->ext_hp_coeff = (float)(1.0 - exp(-2.0 * pi * 16.0 / sid->tick_hz));
}

void m6581_init(m6581_t* sid, const m6581_desc_t* desc) {
    CHIPS_ASSERT(sid && desc);
    CHIPS_ASSERT(desc->tick_hz > 0);
    CHIPS_ASSERT(desc->sound_hz > 0);
    CHIPS_ASSERT(desc->sound_hz <= desc->tick_hz);
    memset(sid, 0, sizeof(*sid));
    sid->sound_hz = desc->sound_hz;
    sid->tick_hz = desc->tick_hz;
    sid->sample_mag = desc->magnitude;
    for (int i = 0; i < 3; i++) {
        _m6581_init_voice(&sid->voice[i]);
    }
    _m6581_init_filter(&sid->filter, sid->tick_hz);
    _m6581_init_sampling(sid);
}

void m6581_reset(m6581_t* sid) {
    CHIPS_ASSERT(sid);
    sid->bus_value = 0;
    sid->bus_decay = 0x2000;
    for (int i = 0; i < 3; i++) {
        _m6581_init_voice(&sid->voice[i]);
    }
    _m6581_init_filter(&sid->filter, sid->tick_hz);
    sid->sample_phase = 0;
    sid->sample = 0.0f;
    sid->sample_pos = 0;
    memset(sid->sample_buffer, 0, sizeof(sid->sample_buffer));
    sid->ext_lp = sid->ext_hp = 0.0f;
    sid->pins = 0;
}

/*--- VOICE IMPLEMENTATION ---------------------------------------------------*/
static inline void _m6581_set_freq_lo(m6581_voice_t* v, uint8_t data) {
    v->freq = (v->freq & 0xFF00) | data;
}

static inline void _m6581_set_freq_hi(m6581_voice_t* v, uint8_t data) {
    v->freq = (data<<8) | (v->freq & 0x00FF);
}

static inline void _m6581_set_pw_lo(m6581_voice_t* v, uint8_t data) {
    v->pulse_width = (v->pulse_width & 0x0F00) | data;
}

static inline void _m6581_set_pw_hi(m6581_voice_t* v, uint8_t data) {
    v->pulse_width = ((data & 0x0F)<<8) | (v->pulse_width & 0x00FF);
}

static inline void _m6581_set_atkdec(m6581_voice_t* v, uint8_t data) {
    v->env_attack_add = data >> 4;
    v->env_decay_sub = data & 0x0F;
    if (v->env_state == M6581_ENV_ATTACK) {
        v->env_counter_compare = v->env_attack_add;
    }
    else if (v->env_state == M6581_ENV_DECAY) {
        v->env_counter_compare = v->env_decay_sub;
    }
}

static inline void _m6581_set_susrel(m6581_voice_t* v, uint8_t data) {
    v->env_sustain_level = (data >> 4) * 0x11;
    v->env_release_sub = data & 0x0F;
    if (v->env_state == M6581_ENV_RELEASE) {
        v->env_counter_compare = v->env_release_sub;
    }
}

static inline void _m6581_set_ctrl(m6581_voice_t* v, uint8_t data) {
    /* wave/noise generator */
    if ((data & M6581_CTRL_TEST) && !(v->ctrl & M6581_CTRL_TEST)) {
        v->wav_accum = 0;
        v->sync = false;
        v->noise_delay = 0;
        v->noise_reset = 0x8000;
    }
    else if (!(data & M6581_CTRL_TEST) && (v->ctrl & M6581_CTRL_TEST)) {
        /* TEST holds shift phase 1; clearing it completes phase 2. */
        uint32_t bit0 = 1 ^ ((v->noise_shift >> 17) & 1);
        v->noise_shift = ((v->noise_shift << 1) | bit0) & 0x007FFFFF;
        v->noise_reset = 0;
    }

    /* envelope generator */
    if ((data & M6581_CTRL_GATE) && !(v->ctrl & M6581_CTRL_GATE)) {
        /* gate bit on: start attack */
        v->env_state = M6581_ENV_ATTACK;
        v->env_hold_zero = false;
        v->env_counter_compare = v->env_attack_add;
    }
    else if (!(data & M6581_CTRL_GATE) && (v->ctrl & M6581_CTRL_GATE)) {
        /* gate bit off: start release */
        v->env_state = M6581_ENV_RELEASE;
        v->env_counter_compare = v->env_release_sub;
    }

    v->ctrl = data;
}

static inline uint32_t _m6581_wavnone(m6581_voice_t* v) {
    /* The disconnected waveform DAC retains its output, not oscillator phase. */
    return v->wav_output;
}

static inline uint32_t _m6581_triangle(m6581_voice_t* v, m6581_voice_t* v_sync) {
    const bool ring = 0 != (v->ctrl & M6581_CTRL_RINGMOD);
    uint32_t msb = (ring ? v->wav_accum ^ ~v_sync->wav_accum : v->wav_accum) & 0x00800000;
    /* Selecting sawtooth disables the shared triangle XOR gates. */
    if (v->ctrl & M6581_CTRL_SAWTOOTH) {
        msb = 0;
    }
    return ((msb ? ~v->wav_accum : v->wav_accum) >> 11) & 0x0FFE;
}

static inline uint32_t _m6581_sawtooth(m6581_voice_t* v) {
    return v->wav_accum >> 12;
}

static inline uint32_t _m6581_pulse(m6581_voice_t* v) {
    if (v->ctrl & M6581_CTRL_TEST) {
        return 0x0FFF;
    }
    else {
        return v->pulse_output;
    }
}

static inline uint32_t _m6581_trisaw(m6581_voice_t* v, m6581_voice_t* v_sync) {
    uint32_t sm = _m6581_triangle(v, v_sync) & _m6581_sawtooth(v);
    return (sm >> 1) & (sm << 1);
}

static inline uint32_t _m6581_tripulse(m6581_voice_t* v, m6581_voice_t* v_sync) {
    uint32_t sm = _m6581_triangle(v, v_sync) & _m6581_pulse(v);
    return (sm >> 1) & (sm << 1);
}

static inline uint32_t _m6581_sawpulse(m6581_voice_t* v) {
    uint32_t sm = _m6581_sawtooth(v) & _m6581_pulse(v);
    return (sm >> 1) & (sm << 1);
}

static inline uint32_t _m6581_trisawpulse(m6581_voice_t* v, m6581_voice_t* v_sync) {
    uint32_t sm = _m6581_triangle(v, v_sync) & _m6581_sawtooth(v) & _m6581_pulse(v);
    return (sm >> 1) & (sm << 1);
}

static inline uint16_t _m6581_noise(m6581_voice_t* v) {
    uint32_t s = v->noise_shift;
    return (M6581_BIT(s,22)<<11) |
           (M6581_BIT(s,20)<<10) |
           (M6581_BIT(s,16)<<9) |
           (M6581_BIT(s,13)<<8) |
           (M6581_BIT(s,11)<<7) |
           (M6581_BIT(s,7)<<6) |
           (M6581_BIT(s,4)<<5) |
           (M6581_BIT(s,2)<<4);
}

static inline void _m6581_voice_tick(m6581_t* sid, int voice_index) {
    m6581_voice_t* v = &sid->voice[voice_index];

    /* waveform generator */
    v->sync = false;
    if (v->ctrl & M6581_CTRL_TEST) {
        if (v->noise_reset && (--v->noise_reset == 0)) {
            v->noise_shift = 0x007FFFFF;
        }
    }
    else {
        if (v->noise_delay) {
            if (--v->noise_delay == 1) {
                uint32_t s = v->noise_shift;
                v->noise_latch = ((s<<1) | (((s>>22)^(s>>17)) & 1)) & 0x007FFFFF;
            }
            else {
                v->noise_shift = v->noise_latch;
            }
        }
        /* frequency accumulator */
        uint32_t prev_accum = v->wav_accum;
        v->wav_accum = (v->wav_accum + v->freq) & 0x00FFFFFF;
        /* noise */
        if ((v->wav_accum & 0x00080000) && !(prev_accum & 0x00080000)) {
            v->noise_delay = 2;
        }
        /* sync state */
        v->sync = (v->wav_accum & 0x00800000) && !(prev_accum & 0x00800000);
    }
    /* envelope generator */
    uint32_t lfsr = v->env_counter;
    if (lfsr != _m6581_rate_count_period[v->env_counter_compare & 0x0F]) {
        const uint32_t feedback = ((lfsr >> 14) ^ (lfsr >> 13)) & 1;
        lfsr = ((lfsr << 1) | feedback) & 0x7FFF;
        v->env_counter = lfsr;
    }
    else {
        v->env_counter = 0x7FFF;
        if ((v->env_state == M6581_ENV_ATTACK) ||
            (++v->env_exp_counter == v->env_exp_period))
        {
            v->env_exp_counter = 0;
            if (v->env_hold_zero) return;
            switch (v->env_state) {
                case M6581_ENV_ATTACK:
                    v->env_cur_level = (v->env_cur_level + 1) & 0xFF;
                    if (v->env_cur_level == 0xFF) {
                        v->env_state = M6581_ENV_DECAY;
                        v->env_counter_compare = v->env_decay_sub;
                    }
                    break;
                case M6581_ENV_DECAY:
                    if (v->env_cur_level != v->env_sustain_level) {
                        v->env_cur_level = (v->env_cur_level - 1) & 0xFF;
                    }
                    break;
                case M6581_ENV_RELEASE:
                    v->env_cur_level = (v->env_cur_level - 1) & 0xFF;
                    break;
                case M6581_ENV_FROZEN:
                    v->env_cur_level = 0;
                    break;
            }
            /* The exponential divider is latched by seven level detectors. */
            switch (v->env_cur_level) {
                case 0xFF: v->env_exp_period = 1; break;
                case 0x5D: v->env_exp_period = 2; break;
                case 0x36: v->env_exp_period = 4; break;
                case 0x1A: v->env_exp_period = 8; break;
                case 0x0E: v->env_exp_period = 16; break;
                case 0x06: v->env_exp_period = 30; break;
                case 0x00:
                    v->env_exp_period = 1;
                    v->env_hold_zero = true;
                    break;
            }
        }
    }
}

static inline void _m6581_voice_sync(m6581_t* sid, int voice_index) {
    m6581_voice_t* v = &sid->voice[voice_index];
    m6581_voice_t* v_sync = &sid->voice[(voice_index+2)%3];
    if (v_sync->sync && (v->ctrl & M6581_CTRL_SYNC)) {
        v->wav_accum = 0;
    }
}

static inline void _m6581_voice_output(m6581_t* sid, int voice_index) {
    m6581_voice_t* v = &sid->voice[voice_index];
    m6581_voice_t* v_sync = &sid->voice[(voice_index+2)%3];
    const uint8_t wave = v->ctrl >> 4;
    uint32_t sm;
    switch (wave & 7) {
        case 0: sm = wave ? 0x0FFF : _m6581_wavnone(v); break;
        case 1: sm = _m6581_triangle(v, v_sync); break;
        case 2: sm = _m6581_sawtooth(v); break;
        case 3: sm = _m6581_trisaw(v, v_sync); break;
        case 4: sm = _m6581_pulse(v); break;
        case 5: sm = _m6581_tripulse(v, v_sync); break;
        case 6: sm = _m6581_sawpulse(v); break;
        default: sm = _m6581_trisawpulse(v, v_sync); break;
    }
    if (wave & 8) {
        sm &= _m6581_noise(v);
        if (wave & 7) {
            /* Other waveforms can pull the noise output bits low. */
            const uint32_t outputs = (1<<22)|(1<<20)|(1<<16)|(1<<13)|
                                    (1<<11)|(1<<7)|(1<<4)|(1<<2);
            uint32_t mask = (0x007FFFFF ^ outputs) |
                (M6581_BIT(sm,11)<<22) | (M6581_BIT(sm,10)<<20) |
                (M6581_BIT(sm,9)<<16) | (M6581_BIT(sm,8)<<13) |
                (M6581_BIT(sm,7)<<11) | (M6581_BIT(sm,6)<<7) |
                (M6581_BIT(sm,5)<<4) | (M6581_BIT(sm,4)<<2);
            v->noise_shift &= mask;
            v->noise_latch &= mask;
        }
    }
    v->wav_output = sm;
    v->pulse_output = ((v->wav_accum >> 12) >= v->pulse_width) ? 0x0FFF : 0;
}

/*--- FILTER IMPLEMENTATION ---------------------------------------------------*/
static void _m6581_set_filter_cutoff(m6581_filter_t* f) {
    float x = f->cutoff / 8.0f;
    float cf = -0.0156f * x * x + 48.473f * x - 45.074f;
    f->w0 = 6.28318530718f * (cf < 30.0f ? 30.0f : cf) / f->tick_hz;
}

static void _m6581_set_resonance(m6581_filter_t* f) {
    f->resonance_coeff = 1.0f/(0.707f + 1.9f * ((float)f->resonance) / 15.0f);
}

static void _m6581_set_cutoff_lo(m6581_filter_t* f, uint8_t data) {
    if ((data ^ f->cutoff) & 7) {
        f->cutoff = (f->cutoff & 0x7F8) | (data & 7);
        _m6581_set_filter_cutoff(f);
    }
}

static void _m6581_set_cutoff_hi(m6581_filter_t* f, uint8_t data) {
    f->cutoff = (data<< 3) | (f->cutoff & 7);
    _m6581_set_filter_cutoff(f);
}

static void _m6581_set_resfilt(m6581_filter_t* f, uint8_t data) {
    f->voices = data & 7;
    f->resonance = data >> 4;
    _m6581_set_resonance(f);
}

static void _m6581_set_modevol(m6581_t* sid, uint8_t data) {
    sid->filter.volume = data & 0x0F;
    sid->filter.mode = data >> 4;
    sid->voice[2].muted = 0 != (sid->filter.mode & M6581_FILTER_3OFF);
}

static inline float _m6581_filter_output(m6581_filter_t* f, float vi) {
    f->v_lp -= f->w0 * f->v_bp;
    f->v_bp -= f->w0 * f->v_hp;
    f->v_hp = f->v_bp * f->resonance_coeff - f->v_lp - vi;

    float vf = 0.0f;
    if (f->mode & M6581_FILTER_LP) {
        vf += f->v_lp;
    }
    if (f->mode & M6581_FILTER_BP) {
        vf += f->v_bp;
    }
    if (f->mode & M6581_FILTER_HP) {
        vf += f->v_hp;
    }
    return vf;
}

static inline bool _m6581_sample(m6581_t* sid, float sample) {
    sid->ext_lp += sid->ext_lp_coeff * (sample - sid->ext_lp);
    sid->ext_hp += sid->ext_hp_coeff * (sid->ext_lp - sid->ext_hp);
    sample = sid->ext_lp - sid->ext_hp;
    /* Mirroring the ring keeps the convolution contiguous. */
    sid->sample_buffer[sid->sample_pos] = sample;
    sid->sample_buffer[sid->sample_pos + M6581_SAMPLE_BUFFER_SIZE] = sample;
    sid->sample_pos = (sid->sample_pos + 1) & (M6581_SAMPLE_BUFFER_SIZE - 1);
    sid->sample_phase += sid->sound_hz;
    if (sid->sample_phase < (uint32_t)sid->tick_hz) {
        return false;
    }
    sid->sample_phase -= sid->tick_hz;
    const float frac = (float)sid->sample_phase / sid->sound_hz;
    const float* src = &sid->sample_buffer[sid->sample_pos];
    float out = 0.0f;
    for (int i = 0; i < M6581_SAMPLE_BUFFER_SIZE - 1; i++) {
        /* Interpolate the fractional chip cycle at the exact sample boundary. */
        out += sid->sample_filter[i] * (src[i+1] + frac * (src[i] - src[i+1]));
    }
    sid->sample = sid->sample_mag * out;
    return true;
}

/* tick the sound generation, return true when new sample ready */
static uint64_t _m6581_tick(m6581_t* sid, uint64_t pins) {
    /* decay the last written register value */
    if (sid->bus_decay > 0) {
        if (--sid->bus_decay == 0) {
            sid->bus_value = 0;
        }
    }

    /* tick wave and envelope generators */
    for (int i = 0; i < 3; i++) {
        _m6581_voice_tick(sid, i);
    }
    /* handle voice synchronization */
    for (int i = 0; i < 3; i++) {
        _m6581_voice_sync(sid, i);
    }
    /* All oscillator phases must settle before computing ring modulation. */
    for (int i = 0; i < 3; i++) {
        _m6581_voice_output(sid, i);
    }
    /* filter */
    float sum_filtered_outp = 0.0f;
    float sum_outp = 0.0f;
    for (int i = 0; i < 3; i++) {
        m6581_voice_t* v = &sid->voice[i];
        float wav_out = v->wav_output * (1.05f / 4095.0f);
        float env_out = v->env_cur_level * (1.0f / 255.0f);
        if (sid->filter.voices & (1<<i)) {
            sum_filtered_outp += (wav_out - M6581_DCWAVE) * env_out + M6581_DCVOICE;
        }
        else if (!v->muted) {
            sum_outp += (wav_out - M6581_DCWAVE) * env_out + M6581_DCVOICE;
        }
    }
    float sample = (sum_outp + _m6581_filter_output(&sid->filter, sum_filtered_outp) + M6581_DCMIXER) *
                  sid->filter.volume * (1.0f / (15.0f * 4.5f));

    /* new sample? */
    if (_m6581_sample(sid, sample)) {
        pins |= M6581_SAMPLE;
    }
    else {
        pins &= ~M6581_SAMPLE;
    }
    return pins;
}

/* read a register */
static uint64_t _m6581_read(m6581_t* sid, uint64_t pins) {
    uint8_t reg = pins & M6581_ADDR_MASK;
    uint8_t data;
    switch (reg) {
        case M6581_POT_X:
        case M6581_POT_Y:
            /* c64-hw-sim: measured by the board (see board_pot_value) */
            data = sid->pot[reg - M6581_POT_X];
            break;
        case M6581_OSC3RAND:
            data = sid->voice[2].wav_output >> 4;
            break;
        case M6581_ENV3:
            data = sid->voice[2].env_cur_level;
            break;
        default:
            data = sid->bus_value;
            break;
    }
    if ((reg >= M6581_POT_X) && (reg <= M6581_ENV3)) {
        sid->bus_value = data;
        sid->bus_decay = 0x2000;
    }
    M6581_SET_DATA(pins, data);
    return pins;
}

/* write a register */
static void _m6581_write(m6581_t* sid, uint64_t pins) {
    uint8_t reg = pins & M6581_ADDR_MASK;
    uint8_t data = M6581_GET_DATA(pins);
    sid->bus_value = data;
    sid->bus_decay = 0x2000;
    switch (reg) {
        case M6581_V1_FREQ_LO:
            _m6581_set_freq_lo(&sid->voice[0], data);
            break;
        case M6581_V1_FREQ_HI:
            _m6581_set_freq_hi(&sid->voice[0], data);
            break;
        case M6581_V1_PW_LO:
            _m6581_set_pw_lo(&sid->voice[0], data);
            break;
        case M6581_V1_PW_HI:
            _m6581_set_pw_hi(&sid->voice[0], data);
            break;
        case M6581_V1_CTRL:
            _m6581_set_ctrl(&sid->voice[0], data);
            break;
        case M6581_V1_ATKDEC:
            _m6581_set_atkdec(&sid->voice[0], data);
            break;
        case M6581_V1_SUSREL:
            _m6581_set_susrel(&sid->voice[0], data);
            break;
        case M6581_V2_FREQ_LO:
            _m6581_set_freq_lo(&sid->voice[1], data);
            break;
        case M6581_V2_FREQ_HI:
            _m6581_set_freq_hi(&sid->voice[1], data);
            break;
        case M6581_V2_PW_LO:
            _m6581_set_pw_lo(&sid->voice[1], data);
            break;
        case M6581_V2_PW_HI:
            _m6581_set_pw_hi(&sid->voice[1], data);
            break;
        case M6581_V2_CTRL:
            _m6581_set_ctrl(&sid->voice[1], data);
            break;
        case M6581_V2_ATKDEC:
            _m6581_set_atkdec(&sid->voice[1], data);
            break;
        case M6581_V2_SUSREL:
            _m6581_set_susrel(&sid->voice[1], data);
            break;
        case M6581_V3_FREQ_LO:
            _m6581_set_freq_lo(&sid->voice[2], data);
            break;
        case M6581_V3_FREQ_HI:
            _m6581_set_freq_hi(&sid->voice[2], data);
            break;
        case M6581_V3_PW_LO:
            _m6581_set_pw_lo(&sid->voice[2], data);
            break;
        case M6581_V3_PW_HI:
            _m6581_set_pw_hi(&sid->voice[2], data);
            break;
        case M6581_V3_CTRL:
            _m6581_set_ctrl(&sid->voice[2], data);
            break;
        case M6581_V3_ATKDEC:
            _m6581_set_atkdec(&sid->voice[2], data);
            break;
        case M6581_V3_SUSREL:
            _m6581_set_susrel(&sid->voice[2], data);
            break;
        case M6581_FC_LO:
            _m6581_set_cutoff_lo(&sid->filter, data);
            break;
        case M6581_FC_HI:
            _m6581_set_cutoff_hi(&sid->filter, data);
            break;
        case M6581_RES_FILT:
            _m6581_set_resfilt(&sid->filter, data);
            break;
        case M6581_MODE_VOL:
            _m6581_set_modevol(sid, data);
            break;
    }
}

/* the all-in-one tick function */
uint64_t m6581_tick(m6581_t* sid, uint64_t pins) {
    CHIPS_ASSERT(sid);

    /* first perform the regular per-tick actions */
    pins = _m6581_tick(sid, pins);

    /* register read/write */
    if (pins & M6581_CS) {
        if (pins & M6581_RW) {
            pins = _m6581_read(sid, pins);
        }
        else {
            _m6581_write(sid, pins);
        }
    }
    sid->pins = pins;
    return pins;
}

#endif /* CHIPS_IMPL */
