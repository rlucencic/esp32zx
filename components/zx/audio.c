/*
 * AY-3-8912, the beeper and the sound output.
 *
 * Like the CPC's AY and the Atari's YM2149: writes become events stamped
 * with their time (T-states here); the render side (the speaker task)
 * replays them into its own copy of the chip. The beeper (EAR and MIC out)
 * and the tape's sound are events too, averaged over each output sample,
 * so short pulses keep their loudness.
 *
 * The AY runs from 1.7734 MHz, half the CPU clock; everything inside it
 * advances in ticks of 8 of its cycles (16 T-states), 7 per output sample
 * of 112 T-states (31669 Hz). The envelope's 16 steps take two ticks of
 * the period each; it runs as 32 half steps here. Output is mono, as from
 * the machines' own sound.
 */
#include <stdlib.h>
#include <string.h>
#include "zx_internal.h"

#define T_PER_SAMPLE        112
#define TICKS_PER_SAMPLE    7
#define EVENT_QUEUE         8192        /* power of two */

enum { EV_BEEPER = 16, EV_TAPE = 17 };

static const uint8_t reg_mask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

/* ---- Emulation side ---- */

typedef struct {
    uint64_t time;                      /* T-state */
    uint8_t reg, value;
} audio_event_t;

static struct {
    uint8_t reg[16];
    uint8_t sel;
    bool enabled;
    audio_event_t *queue;
    volatile uint32_t head, tail;
    volatile uint64_t now;              /* T-states the render side may produce */
    uint32_t dropped;
} ay;

static void push_event(uint64_t t, uint8_t reg, uint8_t value)
{
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
    if (!ay.enabled || ay.queue == NULL) {
        return;
    }
    uint32_t head = ay.head;
    if (head - __atomic_load_n(&ay.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        ay.dropped++;
        return;
    }
    audio_event_t *e = &ay.queue[head % EVENT_QUEUE];
    e->time = t;
    e->reg = reg;
    e->value = value;
    __atomic_store_n(&ay.head, head + 1, __ATOMIC_RELEASE);
}

void ay_select(uint8_t reg)
{
    ay.sel = reg & 15;
}

void ay_write(uint8_t v)
{
    ay.reg[ay.sel] = v & reg_mask[ay.sel];
    if (ay.sel < 14) {
        push_event(zx_now(), ay.sel, ay.reg[ay.sel]);
    }
}

uint8_t ay_read(void)
{
    return ay.reg[ay.sel];
}

void ay_reset(void)
{
    for (int r = 0; r < 16; r++) {
        ay.sel = r;
        ay_write(r == 7 ? 0x3F : 0);
    }
    ay.reg[14] = ay.reg[15] = 0xFF;
    ay.sel = 0;
}

void beeper(uint8_t level, int t)
{
    push_event(zx.clock + (uint64_t)(t < 0 ? 0 : t), EV_BEEPER, level);
}

void beeper_tape(bool level, uint64_t when)
{
    push_event(when, EV_TAPE, level);
}

void audio_frame_end(void)
{
    uint64_t t = zx.clock + ZX_FRAME_T;
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
}

/* ---- Render side ---- */

static struct {
    uint8_t reg[14];
    uint16_t tone_count[3];
    uint8_t tone_out[3];
    uint16_t noise_count;
    uint32_t lfsr;
    uint8_t noise_out;
    uint32_t env_count;
    uint8_t env_pos;            /* 0..31 half steps */
    uint8_t env_attack, env_holding, env_hold_value;
    uint8_t beeper, tape;       /* levels now */
    int32_t dc;
    uint64_t pos;               /* T-state of the next sample */
} ren = { .lfsr = 1 };

/* AY 4-bit DAC, full scale 8191 per channel */
static const uint16_t ay_level[16] = {
    0, 104, 151, 222, 327, 484, 675, 1103, 1299, 2088, 2917, 3661, 4621, 5802, 6899, 8191,
};

/* The beeper's loudness: EAR (bit 1) and MIC (bit 0) out */
static const int16_t beeper_level[4] = { 0, 1200, 9000, 10200 };
#define TAPE_LEVEL      1500

static void synth_write(uint8_t reg, uint8_t value)
{
    ren.reg[reg] = value;
    if (reg == 13) {
        ren.env_count = 0;
        ren.env_pos = 0;
        ren.env_attack = (value & 0x04) != 0;
        ren.env_holding = 0;
    }
}

static inline uint8_t env_level(void)
{
    uint8_t p = ren.env_holding ? ren.env_hold_value : ren.env_attack ? ren.env_pos : 31 - ren.env_pos;
    return p >> 1;
}

static inline void env_advance(uint32_t ticks)
{
    if (ren.env_holding) {
        return;
    }
    uint32_t ep = ren.reg[11] | (ren.reg[12] << 8);
    ep = ep ? ep : 1;
    ren.env_count += ticks;
    while (ren.env_count >= ep && !ren.env_holding) {
        ren.env_count -= ep;
        if (++ren.env_pos > 31) {
            uint8_t shape = ren.reg[13];
            bool cont = shape & 0x08, att = shape & 0x04, alt = shape & 0x02, hold = shape & 0x01;
            ren.env_pos = 0;
            if (!cont) {
                ren.env_holding = 1;
                ren.env_hold_value = 0;
            } else if (hold) {
                ren.env_holding = 1;
                ren.env_hold_value = (alt ^ att) ? 31 : 0;
            } else if (alt) {
                ren.env_attack ^= 1;
            }
        }
    }
}

static inline void noise_advance(uint32_t ticks)
{
    uint32_t np = ren.reg[6] ? ren.reg[6] * 2 : 2;
    ren.noise_count += ticks;
    while (ren.noise_count >= np) {
        ren.noise_count -= np;
        uint32_t bit = (ren.lfsr ^ (ren.lfsr >> 3)) & 1;
        ren.lfsr = (ren.lfsr >> 1) | (bit << 16);
    }
    ren.noise_out = ren.lfsr & 1;
}

/* Ticks the tone output is high during the next `ticks` */
static inline uint32_t tone_advance(int ch, uint32_t ticks)
{
    uint32_t period = ren.reg[ch * 2] | (ren.reg[ch * 2 + 1] << 8);
    period = period ? period : 1;
    uint32_t count = ren.tone_count[ch];
    if (count >= period) {
        count = period - 1;
    }
    uint8_t out = ren.tone_out[ch];
    uint32_t high = 0;
    while (ticks) {
        uint32_t to_toggle = period - count;
        if (to_toggle > ticks) {
            high += out ? ticks : 0;
            count += ticks;
            break;
        }
        high += out ? to_toggle : 0;
        ticks -= to_toggle;
        count = 0;
        out ^= 1;
    }
    ren.tone_count[ch] = count;
    ren.tone_out[ch] = out;
    return high;
}

static inline int16_t clip16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static inline int32_t ay_sample(void)
{
    uint8_t mixer = ren.reg[7];
    env_advance(TICKS_PER_SAMPLE);
    noise_advance(TICKS_PER_SAMPLE);
    uint8_t env = env_level();
    int32_t sum = 0;
    for (int c = 0; c < 3; c++) {
        bool tone_off = (mixer >> c) & 1;
        bool noise_off = (mixer >> (c + 3)) & 1;
        uint32_t high = tone_off ? TICKS_PER_SAMPLE : tone_advance(c, TICKS_PER_SAMPLE);
        if ((noise_off || ren.noise_out) && high) {
            uint8_t vol = ren.reg[8 + c];
            sum += ay_level[(vol & 0x10) ? env : vol & 15] * high / TICKS_PER_SAMPLE;
        }
    }
    return sum;
}

static inline int32_t beeper_now(void)
{
    return beeper_level[ren.beeper & 3] + (ren.tape ? TAPE_LEVEL : 0);
}

int ZX_HOT zx_audio_render(int16_t *dst, int frames)
{
    uint64_t limit = __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE);
    int done = 0;
    uint32_t tail = ay.tail;
    while (done < frames && ren.pos + T_PER_SAMPLE <= limit) {
        uint64_t end = ren.pos + T_PER_SAMPLE;
        uint32_t head = __atomic_load_n(&ay.head, __ATOMIC_ACQUIRE);
        /* The beeper averaged over the sample, the AY as it is after the
         * sample's writes
         */
        int64_t beep = 0;
        uint64_t t = ren.pos;
        while (tail != head && ay.queue[tail % EVENT_QUEUE].time < end) {
            const audio_event_t *e = &ay.queue[tail % EVENT_QUEUE];
            uint64_t et = e->time > t ? e->time : t;
            if (e->reg >= EV_BEEPER) {
                beep += (int64_t)beeper_now() * (int64_t)(et - t);
                t = et;
                if (e->reg == EV_BEEPER) {
                    ren.beeper = e->value;
                } else {
                    ren.tape = e->value;
                }
            } else {
                synth_write(e->reg, e->value);
            }
            tail++;
        }
        beep += (int64_t)beeper_now() * (int64_t)(end - t);
        int32_t v = ay_sample() * 3 / 4 + (int32_t)(beep / T_PER_SAMPLE);
        ren.dc += ((v << 8) - ren.dc) >> 10;
        int16_t out = clip16(v - (ren.dc >> 8));
        *dst++ = out;
        *dst++ = out;
        ren.pos = end;
        done++;
    }
    __atomic_store_n(&ay.tail, tail, __ATOMIC_RELEASE);
    return done;
}

int zx_audio_pending(void)
{
    uint64_t now = __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE);
    return now > ren.pos ? (int)((now - ren.pos) / T_PER_SAMPLE) : 0;
}

void zx_audio_enable(bool enable)
{
    ay.enabled = enable;
}

void audio_init(void)
{
    if (ay.enabled && ay.queue == NULL) {
        ay.queue = zx_malloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
    ay.head = ay.tail = 0;
    uint64_t now = zx.clock + (uint64_t)z80.t;
    ay.now = now;
    memset(&ren, 0, sizeof(ren));
    ren.lfsr = 1;
    ren.pos = now;
}
