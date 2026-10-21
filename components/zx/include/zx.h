/*
 * ZX Spectrum 128K, +2 and +3.
 *
 * Z80 at 3.5469 MHz with the ULA's memory contention (esp32emu's Z80 core
 * in T-states), the ULA (256 x 192 with attributes, border, 50 Hz
 * interrupt, keyboard, beeper, tape input), 128 KB in 16 KB banks with the
 * 128K and +3 paging, AY-3-8912. Floppy disks: the +3's own uPD765 with
 * DSK images (esp32emu's), or on the 128K and +2 a Beta 128 interface
 * (WD1793, TR-DOS ROM) with TRD and SCL images. Tapes (TAP, TZX) play in
 * real time, standard blocks load and save at once (ROM traps). Snapshots
 * (SNA, Z80) load into the machine.
 *
 * Platform independent: the caller provides the ROMs and the big buffers
 * and drives the emulation one video frame (70908 T-states) at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ZX_MODEL_128,               /* Sinclair ZX Spectrum 128 */
    ZX_MODEL_PLUS2,             /* Amstrad (grey) +2: the 128 with a tape deck */
    ZX_MODEL_PLUS3,             /* +3: +2A hardware and a disk drive */
    ZX_MODEL_COUNT
} zx_model_t;

/* The picture: 256 x 192 with 48 pixels of border left and right and 54
 * lines above and below; the board doubles it to 704 x 600
 */
#define ZX_SCREEN_WIDTH         352
#define ZX_SCREEN_HEIGHT        300
#define ZX_SCREEN_HZ            50
#define ZX_FRAME_T              70908   /* 311 lines of 228 T-states */
#define ZX_CLOCK_HZ             3546900

#define ZX_ROM_SIZE             16384
#define ZX_RAM_SIZE             (128 * 1024)
#define ZX_DRIVES               2       /* A: and B: */

typedef struct {
    zx_model_t model;
    uint8_t *ram;               /* ZX_RAM_SIZE bytes */
    /* The ROMs as dumped: 32 KB (128K, +2: editor, 48 BASIC) or 64 KB (+3) */
    const uint8_t *rom;
    /* TR-DOS for the Beta 128 interface (16 KB, 128K and +2); NULL: none */
    const uint8_t *trdos;
    /* ZX_SCREEN_WIDTH x ZX_SCREEN_HEIGHT RGB565 pixels */
    uint16_t *frame;
} zx_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    uint8_t port_7ffd, port_1ffd;
    uint8_t border;
    bool trdos;                 /* the TR-DOS ROM is paged in */
} zx_status_t;

/* Set up the machine and cold-reset it. False if something is missing. */
bool zx_init(const zx_config_t *config);

/* Cold reset, as the power switch */
void zx_reset(void);

/* Emulate one video frame */
void zx_run_frame(void);

void zx_get_status(zx_status_t *status);
zx_model_t zx_model(void);

/* Read memory as the CPU sees it now (no side effects) */
uint8_t zx_peek(uint16_t address);

/* ---- Screen ---- */

/* Line y (0 .. ZX_SCREEN_HEIGHT - 1) as RGB565, ZX_SCREEN_WIDTH pixels */
void zx_render_line_rgb565(int y, uint16_t *dst);

/* Changes when the picture does */
uint32_t zx_screen_signature(void);

/* ---- Sound: 16-bit stereo (AY: A left, B both, C right; beeper both) ---- */

#define ZX_AUDIO_HZ             31669   /* 112 T-states per sample */

void zx_audio_enable(bool enable);
int zx_audio_render(int16_t *dst, int frames);
int zx_audio_pending(void);

/* ---- Floppy disks: drives 0 (A:) and 1 (B:) ----
 *
 * By the image: .DSK goes to the +3's uPD765, .TRD and .SCL to the Beta
 * 128 (an SCL is read-only). Only the model's own kind is used. Writes go
 * back to the file with zx_disk_flush(). Not while zx_run_frame() runs.
 */
bool zx_disk_insert(int drive, const char *path);
void zx_disk_eject(int drive);
const char *zx_disk_path(int drive);        /* NULL when empty */
bool zx_disk_write_protected(int drive);
bool zx_disk_dirty(void);
void zx_disk_flush(void);

/* A formatted blank disk: ZX_TRD_BYTES (TR-DOS, 80 tracks, two sides) or
 * ZX_PLUS3_BYTES (+3, 40 tracks of 9 sectors, one side)
 */
#define ZX_TRD_BYTES            655360
#define ZX_PLUS3_BYTES          184320
bool zx_disk_create(const char *path, uint32_t bytes);

typedef struct {
    int light;                  /* drive accessed in the last ~0.3 s, -1 none */
    uint32_t reads[ZX_DRIVES];  /* sectors read and written */
    uint32_t writes[ZX_DRIVES];
    uint8_t track[ZX_DRIVES];   /* head position */
} zx_disk_status_t;

void zx_disk_status(zx_disk_status_t *status);

/* True for file names the model's drives take */
bool zx_disk_fits(zx_model_t model, const char *name);

/* A disk's free space and size in bytes as its DOS counts them (+3DOS,
 * TR-DOS), worked out at insert, eject and flush; false when not known.
 * Any task.
 */
bool zx_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* ---- Tape ----
 *
 * .TAP and .TZX. The tape starts when the ROM's loader does (or with
 * zx_tape_play()) and stops at a "stop the tape" block or its end.
 * Standard-speed blocks load at once through the ROM loader unless fast
 * loading is off; SAVE appends to an inserted .TAP. Not while
 * zx_run_frame() runs (except the play/stop/fast calls).
 */
bool zx_tape_insert(const char *path);
void zx_tape_eject(void);
const char *zx_tape_path(void);             /* NULL when empty */
bool zx_tape_create(const char *path);      /* an empty .TAP */
void zx_tape_rewind(void);
void zx_tape_play(bool play);
void zx_tape_fast(bool fast);

typedef struct {
    bool inserted, playing, fast;
    int block, blocks;          /* current block (0 ..), how many */
    uint32_t loaded, saved;     /* blocks loaded and saved at once */
} zx_tape_status_t;

void zx_tape_status(zx_tape_status_t *status);

/* ---- Snapshots: .SNA (48K and 128K) and .Z80 (v1-v3) ----
 *
 * Loads into the running machine (a 48K snapshot runs in 48K mode, paging
 * locked); false if it isn't one, or needs another model. Not while
 * zx_run_frame() runs.
 */
bool zx_snapshot_load(const char *path);

/* ---- Keyboard and joystick: safe to call from another task ---- */

/* Key number: half-row * 8 + bit, rows as the ULA scans them:
 *   0 CAPS SHIFT Z X C V    1 A S D F G     2 Q W E R T    3 1 2 3 4 5
 *   4 0 9 8 7 6             5 P O I U Y     6 ENTER L K J H
 *   7 SPACE SYMBOL-SHIFT M N B
 */
enum {
    ZX_KEY_CAPS = 0, ZX_KEY_ENTER = 48, ZX_KEY_SPACE = 56, ZX_KEY_SYMBOL = 57,
};
void zx_key(uint8_t key, bool down);
void zx_keys_up(void);

/* The key for a letter, digit, space or '\n' (*symbol: with SYMBOL SHIFT
 * for punctuation, *caps: with CAPS SHIFT); -1 if none
 */
int zx_ascii_key(char c, bool *caps, bool *symbol);

/* Type text, one key every few frames. Replaces what's still queued. */
void zx_type(const char *text);

enum {
    ZX_JOY_RIGHT = 0x01, ZX_JOY_LEFT = 0x02, ZX_JOY_DOWN = 0x04, ZX_JOY_UP = 0x08,
    ZX_JOY_FIRE = 0x10,
};
typedef enum {
    ZX_JOYSTICK_KEMPSTON,       /* port $1F */
    ZX_JOYSTICK_SINCLAIR,       /* keys 6-0 (the +2's and +3's left port) */
    ZX_JOYSTICK_CURSOR,         /* keys 5-8 and 0 (Protek, AGF) */
    ZX_JOYSTICK_COUNT
} zx_joystick_t;

void zx_joystick_type(zx_joystick_t type);
void zx_joystick(uint8_t bits);
