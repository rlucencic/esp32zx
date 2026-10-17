/*
 * Between the Spectrum core's modules.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "zx.h"
#include "z80.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define ZX_LOG(fmt, ...)    ESP_LOGI("zx", fmt, ##__VA_ARGS__)
/* The core's buffers in PSRAM: plain malloc() serves small blocks from
 * internal RAM, which WiFi, Bluetooth and the LCD need
 */
#define zx_malloc(n)        heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define zx_calloc(n, s)     heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#define zx_realloc(p, n)    heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define ZX_LOG(fmt, ...)    printf("zx: " fmt "\n", ##__VA_ARGS__)
#define zx_malloc(n)        malloc(n)
#define zx_calloc(n, s)     calloc((n), (s))
#define zx_realloc(p, n)    realloc((p), (n))
#endif

/* Code on the emulation's hot path */
#define ZX_HOT              Z80_HOT

/* ---- zx.c: the machine ---- */

typedef struct {
    zx_config_t cfg;
    bool plus3;                 /* +2A/+3 hardware (paging, contention, FDC) */
    uint8_t p7ffd, p1ffd;
    bool trdos_active;          /* the Beta 128's ROM is paged in */
    uint8_t ula_out;            /* last write to port $FE */
    uint64_t clock;             /* T-states since power-on at this frame's start */
    uint32_t frames;
    int int_len;                /* T-states the interrupt is held */
    bool reset_pending;
} zx_t;

extern zx_t zx;

/* T-states since power-on, now */
static inline uint64_t zx_now(void)
{
    return zx.clock + (uint64_t)z80.t;
}

/* The ROM in use at $0000 is the 48K BASIC one (tape traps, TR-DOS) */
bool zx_basic_rom(void);

void mem_update(void);

/* ---- ula.c: video, contention, floating bus ---- */

/* First contended T-state of the frame, and of the top-left paper pixel */
int ula_contention_start(void);

bool ula_init(void);                    /* tables for the model */
void ula_reset(void);
void ula_frame_start(void);
extern int ula_next_t;                  /* when ula_run() has something to do */
void ula_run(int t);                    /* draw what the beam passed by t */
void ula_frame_end(void);               /* the border, the rest of the frame */
void ula_border(uint8_t colour, int t);
uint8_t ula_floating_bus(int t);        /* the byte the ULA reads at t */
/* Port contention for an I/O cycle starting at t: the wait */
int ula_io_contention(uint16_t port, int t);

/* ---- audio.c: AY-3-8912, beeper, the sound output ---- */

void audio_init(void);
void ay_select(uint8_t reg);
void ay_write(uint8_t v);
uint8_t ay_read(void);
void ay_reset(void);
void beeper(uint8_t level, int t);      /* 0-3: EAR (bit 1) and MIC (bit 0) out, at frame time t */
void beeper_tape(bool level, uint64_t when);    /* the tape's sound, at T-state `when` */
void audio_frame_end(void);

/* ---- tape.c ---- */

void tape_reset(void);
bool tape_ear(void);                    /* the tape's level now */
bool tape_inserted(void);
bool tape_trap_load(void);              /* at the ROM's LD-BYTES: true if handled */
bool tape_trap_save(void);              /* at SA-BYTES */
void tape_rom_loader(void);             /* the ROM's loader started: play */
void tape_frame_end(void);

/* ---- beta.c: Beta 128, WD1793, TRD and SCL ---- */

void beta_reset(void);
bool beta_port(uint16_t port);          /* one of its ports (when TR-DOS is paged) */
uint8_t beta_in(uint16_t port);
void beta_out(uint16_t port, uint8_t v);
void beta_frame(void);
bool beta_has_disk(int drive);

/* ---- input.c ---- */

void input_reset(void);
void input_frame(void);                 /* drain the queue, once a frame */
uint8_t input_rows(uint8_t high);       /* port $FE bits 0-4 for the half-rows selected (active low) */
uint8_t input_kempston(void);
bool input_kempston_present(void);
