/*
 * Keyboard and joystick, from other tasks (Bluetooth, console).
 *
 * Events go through a small queue that the emulation drains once a frame
 * into the keyboard matrix: 8 half-rows of 5 keys, active low, read
 * through port $FE with the half-rows selected by the address's high byte.
 * The joystick is a Kempston interface (port $1F), or presses keys
 * (Sinclair: 6-0, cursor: 5-8 and 0).
 */
#include <stdio.h>
#include <string.h>
#include "zx_internal.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()      portENTER_CRITICAL(&s_lock)
#define UNLOCK()    portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QUEUE_SIZE  64          /* power of two */

enum { EV_KEY, EV_JOY, EV_KEYS_UP };

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t value;
} input_event_t;

static input_event_t s_queue[QUEUE_SIZE];
static volatile uint32_t s_head, s_tail;

static struct {
    uint8_t matrix[8];          /* bits 0-4, active low */
    uint8_t joy;
    zx_joystick_t joy_type;
} in = { .joy_type = ZX_JOYSTICK_KEMPSTON };

static void push(const input_event_t *ev)
{
    LOCK();
    if (s_head - s_tail < QUEUE_SIZE) {
        s_queue[s_head % QUEUE_SIZE] = *ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    UNLOCK();
}

void zx_key(uint8_t key, bool down)
{
    input_event_t ev = { .type = EV_KEY, .code = key, .value = down };
    push(&ev);
}

void zx_joystick(uint8_t bits)
{
    input_event_t ev = { .type = EV_JOY, .value = bits };
    push(&ev);
}

void zx_keys_up(void)
{
    input_event_t ev = { .type = EV_KEYS_UP };
    push(&ev);
}

void zx_joystick_type(zx_joystick_t type)
{
    in.joy_type = type;
}

bool input_kempston_present(void)
{
    return in.joy_type == ZX_JOYSTICK_KEMPSTON;
}

/* ---- Typing ---- */

#define TYPE_MAX    256

static char s_type[TYPE_MAX];
static volatile int s_type_len;
static int s_type_pos, s_type_frame;

/* Key numbers by character: plain, with CAPS SHIFT, with SYMBOL SHIFT */
static const char s_plain[64] = {
    0, 'z', 'x', 'c', 'v', 0, 0, 0,     'a', 's', 'd', 'f', 'g', 0, 0, 0,
    'q', 'w', 'e', 'r', 't', 0, 0, 0,   '1', '2', '3', '4', '5', 0, 0, 0,
    '0', '9', '8', '7', '6', 0, 0, 0,   'p', 'o', 'i', 'u', 'y', 0, 0, 0,
    '\n', 'l', 'k', 'j', 'h', 0, 0, 0,  ' ', 0, 'm', 'n', 'b', 0, 0, 0,
};
static const char s_symbol[64] = {
    0, ':', 0, '?', '/', 0, 0, 0,       '~', '|', '\\', '{', '}', 0, 0, 0,
    0, 0, 0, '<', '>', 0, 0, 0,         '!', '@', '#', '$', '%', 0, 0, 0,
    '_', ')', '(', '\'', '&', 0, 0, 0,  '"', ';', 0, ']', '[', 0, 0, 0,
    0, '=', '+', '-', '^', 0, 0, 0,     0, 0, '.', ',', '*', 0, 0, 0,
};

int zx_ascii_key(char c, bool *caps, bool *symbol)
{
    *caps = *symbol = false;
    char lower = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    for (int k = 0; k < 64; k++) {
        if (s_plain[k] && s_plain[k] == lower) {
            *caps = lower != c;
            return k;
        }
    }
    for (int k = 0; k < 64; k++) {
        if (s_symbol[k] && s_symbol[k] == c) {
            *symbol = true;
            return k;
        }
    }
    return -1;
}

void zx_type(const char *text)
{
    LOCK();
    size_t n = strlen(text);
    if (n >= TYPE_MAX) {
        n = TYPE_MAX - 1;
    }
    memcpy(s_type, text, n);
    s_type[n] = 0;
    s_type_len = n;
    s_type_pos = 0;
    s_type_frame = 0;
    UNLOCK();
}

static inline void matrix_set(int key, bool down)
{
    uint8_t bit = 1u << (key & 7);
    in.matrix[key >> 3] = down ? in.matrix[key >> 3] & ~bit : in.matrix[key >> 3] | bit;
}

/* Each character: keys down for 3 frames, up for 3 (the ROM scans 50 times
 * a second and wants a key let go before it repeats)
 */
static void type_frame(void)
{
    if (s_type_pos >= s_type_len) {
        return;
    }
    bool caps, symbol;
    int key = zx_ascii_key(s_type[s_type_pos], &caps, &symbol);
    int phase = s_type_frame++;
    if (key < 0) {
        s_type_pos++;
        s_type_frame = 0;
        return;
    }
    if (phase == 0) {
        if (caps) {
            matrix_set(ZX_KEY_CAPS, true);
        }
        if (symbol) {
            matrix_set(ZX_KEY_SYMBOL, true);
        }
        matrix_set(key, true);
    } else if (phase == 3) {
        matrix_set(key, false);
        matrix_set(ZX_KEY_CAPS, false);
        matrix_set(ZX_KEY_SYMBOL, false);
    } else if (phase >= 6) {
        s_type_pos++;
        s_type_frame = 0;
    }
}

void input_frame(void)
{
    type_frame();
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    while (s_tail != head) {
        input_event_t ev = s_queue[s_tail % QUEUE_SIZE];
        __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
        switch (ev.type) {
        case EV_KEY:
            if (ev.code < 64 && (ev.code & 7) < 5) {
                matrix_set(ev.code, ev.value);
            }
            break;
        case EV_JOY:
            in.joy = ev.value;
            break;
        case EV_KEYS_UP:
            memset(in.matrix, 0xFF, sizeof(in.matrix));
            in.joy = 0;
            break;
        }
    }
}

/* The joystick as keys: Sinclair (6 left, 7 right, 8 down, 9 up, 0 fire),
 * cursor (5 left, 8 right, 6 down, 7 up, 0 fire)
 */
static void joystick_keys(uint8_t *rows)
{
    static const uint8_t sinclair[5] = { 35, 36, 34, 33, 32 };  /* right left down up fire */
    static const uint8_t cursor[5] = { 34, 28, 36, 35, 32 };
    const uint8_t *keys = in.joy_type == ZX_JOYSTICK_SINCLAIR ? sinclair : cursor;
    for (int i = 0; i < 5; i++) {
        if (in.joy & (1u << i)) {
            rows[keys[i] >> 3] &= ~(1u << (keys[i] & 7));
        }
    }
}

uint8_t input_rows(uint8_t high)
{
    uint8_t rows[8];
    memcpy(rows, in.matrix, sizeof(rows));
    if (in.joy && in.joy_type != ZX_JOYSTICK_KEMPSTON) {
        joystick_keys(rows);
    }
    uint8_t v = 0x1F;
    for (int r = 0; r < 8; r++) {
        if (!(high & (1u << r))) {
            v &= rows[r];
        }
    }
    return v & 0x1F;
}

uint8_t input_kempston(void)
{
    return in.joy & 0x1F;
}

void input_reset(void)
{
    memset(in.matrix, 0xFF, sizeof(in.matrix));
}
