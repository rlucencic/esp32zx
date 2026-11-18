/*
 * ZX Spectrum 128K / +2 / +3 emulator on the Makerfabs MaTouch ESP32-S3 7".
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Spectrum into it: memory, ROMs, settings, keyboard map, joystick, disks,
 * tape and snapshots, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "zx.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static const char *const model_names[] = { "128K", "+2", "+3", "" };
static const char *const model_full[] = {
    "ZX Spectrum 128", "ZX Spectrum +2", "ZX Spectrum +3",
};
static const char *const joystick_names[] = { "Kempston", "Sinclair", "cursor" };

static zx_config_t s_cfg;
static uint8_t *s_rom, *s_trdos;
static char s_rom_text[96];
static bool s_started;
static char s_snapshot[DISKS_NAME_LEN + 64];  /* path of the snapshot "in the drive" */

/* ---- Memory ----
 *
 * Everything at power-on, before the screen, WiFi and Bluetooth fragment
 * PSRAM: 128 KB RAM, the largest ROM set (+3: 64 KB), TR-DOS, the picture.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(ZX_RAM_SIZE, MALLOC_CAP_SPIRAM);
    s_rom = heap_caps_malloc(4 * ZX_ROM_SIZE, MALLOC_CAP_SPIRAM);
    s_trdos = heap_caps_malloc(ZX_ROM_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.frame = heap_caps_calloc(ZX_SCREEN_WIDTH * ZX_SCREEN_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disks, tapes and snapshots on the SD card" : "No SD card: no disks or tapes");
}

/* ---- Drives: A:, B:, the tape, the snapshot ---- */

enum { DRIVE_A, DRIVE_B, DRIVE_TAPE, DRIVE_SNAPSHOT, DRIVES };

#define BLANK_TAPE      1               /* "size" of a new empty tape */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_tape(const char *name)
{
    return has_ext(name, ".tap") || has_ext(name, ".tzx");
}

static bool is_snapshot(const char *name)
{
    return has_ext(name, ".sna") || has_ext(name, ".z80");
}

/* ---- Starting ---- */

static bool missing(const char *what)
{
    static char message[200];
    snprintf(message, sizeof(message),
             "No %s ROM: put %s into roms/ and flash with idf.py flash.", model_full[settings.model], what);
    emu_set_message(message);
    return false;
}

static bool start(void)
{
    if (s_cfg.ram == NULL || s_rom == NULL || s_trdos == NULL || s_cfg.frame == NULL) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    zx_model_t model = settings.model;
    static const rom_id_t roms[] = { ROM_128, ROM_PLUS2, ROM_PLUS3 };
    if (!rom_store_load(roms[model], s_rom)) {
        return missing(rom_store_name(roms[model]));
    }
    s_cfg.model = model;
    s_cfg.rom = s_rom;
    s_cfg.trdos = model != ZX_MODEL_PLUS3 && rom_store_load(ROM_TRDOS, s_trdos) ? s_trdos : NULL;
    snprintf(s_rom_text, sizeof(s_rom_text), "%s%s", rom_store_name(roms[model]),
             model == ZX_MODEL_PLUS3 ? "" : s_cfg.trdos ? ", trdos.rom (Beta 128)" : " (no trdos.rom: no disks)");

    /* Disks of the other kind (TR-DOS on a +3, +3 disks on a 128) come out */
    for (int d = DRIVE_A; d <= DRIVE_B; d++) {
        const char *name = disks_name(d);
        if (name && !zx_disk_fits(model, name)) {
            disks_eject(d);
        }
    }
    if (!zx_init(&s_cfg)) {
        emu_set_message("The Spectrum could not be set up: see the log.");
        return false;
    }
    zx_joystick_type(settings.joystick);
    zx_tape_fast(settings.tape_fast);
    s_started = true;
    if (s_snapshot[0] && !zx_snapshot_load(s_snapshot)) {
        ESP_LOGW(TAG, "can't load %s", s_snapshot);
    }
    ESP_LOGI(TAG, "%s: %s", model_full[model], s_rom_text);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = ZX_SCREEN_WIDTH;
    mode->height = ZX_SCREEN_HEIGHT;
    mode->hz = ZX_SCREEN_HZ;
}

/* ---- Keyboard and joystick ----
 *
 * Letters, digits, Enter and Space as on the Spectrum; Shift is CAPS
 * SHIFT, Ctrl and Alt are SYMBOL SHIFT. Keys the Spectrum has as shifted
 * ones: Backspace (DELETE), cursor keys, Esc (BREAK), Caps Lock, Tab
 * (EXTEND MODE). Punctuation keys type their character with SYMBOL SHIFT
 * (and without CAPS SHIFT). The numeric keypad is the joystick (8 4 6 2
 * and the diagonals, 0 or 5 fire); F12 makes the cursor keys the joystick
 * too (Alt: fire), F11 switches the joystick between Kempston, Sinclair
 * and cursor. F9 starts or stops the tape, F10 rewinds it.
 */
#define KEY(row, bit)   ((row) * 8 + (bit))
#define NONE            0xFF
enum {
    K_CAPS = KEY(0, 0), K_Z = KEY(0, 1), K_X = KEY(0, 2), K_C = KEY(0, 3), K_V = KEY(0, 4),
    K_A = KEY(1, 0), K_S = KEY(1, 1), K_D = KEY(1, 2), K_F = KEY(1, 3), K_G = KEY(1, 4),
    K_Q = KEY(2, 0), K_W = KEY(2, 1), K_E = KEY(2, 2), K_R = KEY(2, 3), K_T = KEY(2, 4),
    K_1 = KEY(3, 0), K_2 = KEY(3, 1), K_3 = KEY(3, 2), K_4 = KEY(3, 3), K_5 = KEY(3, 4),
    K_0 = KEY(4, 0), K_9 = KEY(4, 1), K_8 = KEY(4, 2), K_7 = KEY(4, 3), K_6 = KEY(4, 4),
    K_P = KEY(5, 0), K_O = KEY(5, 1), K_I = KEY(5, 2), K_U = KEY(5, 3), K_Y = KEY(5, 4),
    K_ENTER = KEY(6, 0), K_L = KEY(6, 1), K_K = KEY(6, 2), K_J = KEY(6, 3), K_H = KEY(6, 4),
    K_SPACE = KEY(7, 0), K_SYM = KEY(7, 1), K_M = KEY(7, 2), K_N = KEY(7, 3), K_B = KEY(7, 4),
};

/* HID usage -> Spectrum key, or a combination with CAPS (C) or SYMBOL (S) */
typedef struct {
    uint8_t key;
    uint8_t with;               /* NONE, K_CAPS or K_SYM */
    uint8_t shifted_key;        /* with the PC's Shift held (punctuation): */
    uint8_t shifted_with;
} keymap_t;

#define PLAIN(k)        { (k), NONE, NONE, NONE }
#define CAPS(k)         { (k), K_CAPS, NONE, NONE }
#define PUNCT(k, sk)    { (k), K_SYM, (sk), K_SYM }

static const keymap_t usage_map[0x39] = {
    [0x04] = PLAIN(K_A), [0x05] = PLAIN(K_B), [0x06] = PLAIN(K_C), [0x07] = PLAIN(K_D),
    [0x08] = PLAIN(K_E), [0x09] = PLAIN(K_F), [0x0A] = PLAIN(K_G), [0x0B] = PLAIN(K_H),
    [0x0C] = PLAIN(K_I), [0x0D] = PLAIN(K_J), [0x0E] = PLAIN(K_K), [0x0F] = PLAIN(K_L),
    [0x10] = PLAIN(K_M), [0x11] = PLAIN(K_N), [0x12] = PLAIN(K_O), [0x13] = PLAIN(K_P),
    [0x14] = PLAIN(K_Q), [0x15] = PLAIN(K_R), [0x16] = PLAIN(K_S), [0x17] = PLAIN(K_T),
    [0x18] = PLAIN(K_U), [0x19] = PLAIN(K_V), [0x1A] = PLAIN(K_W), [0x1B] = PLAIN(K_X),
    [0x1C] = PLAIN(K_Y), [0x1D] = PLAIN(K_Z),
    [0x1E] = PLAIN(K_1), [0x1F] = PLAIN(K_2), [0x20] = PLAIN(K_3), [0x21] = PLAIN(K_4),
    [0x22] = PLAIN(K_5), [0x23] = PLAIN(K_6), [0x24] = PLAIN(K_7), [0x25] = PLAIN(K_8),
    [0x26] = PLAIN(K_9), [0x27] = PLAIN(K_0),
    [0x28] = PLAIN(K_ENTER),
    [0x29] = CAPS(K_SPACE),             /* Esc: BREAK */
    [0x2A] = CAPS(K_0),                 /* Backspace: DELETE */
    [0x2B] = CAPS(K_SYM),               /* Tab: EXTEND MODE */
    [0x2C] = PLAIN(K_SPACE),
    [0x2D] = PUNCT(K_J, K_0),           /* - _ */
    [0x2E] = PUNCT(K_L, K_K),           /* = + */
    [0x2F] = PUNCT(K_8, K_F),           /* [ { -> ( { */
    [0x30] = PUNCT(K_9, K_G),           /* ] } -> ) } */
    [0x31] = PUNCT(K_D, K_S),           /* \ | */
    [0x32] = PUNCT(K_3, K_A),           /* non-US # ~ */
    [0x33] = PUNCT(K_O, K_Z),           /* ; : */
    [0x34] = PUNCT(K_7, K_P),           /* ' " */
    [0x35] = PUNCT(K_X, K_A),           /* ` ~ -> £ ~ */
    [0x36] = PUNCT(K_N, K_R),           /* , < */
    [0x37] = PUNCT(K_M, K_T),           /* . > */
    [0x38] = PUNCT(K_V, K_C),           /* / ? */
};

/* Cursor keys (right, left, down, up) and Caps Lock */
static const keymap_t arrow_map[4] = { CAPS(K_8), CAPS(K_5), CAPS(K_6), CAPS(K_7) };
static const keymap_t capslock_map = CAPS(K_2);

static uint8_t s_count[64];             /* presses holding each Spectrum key */
static uint8_t s_no_caps;               /* keys that want CAPS SHIFT up */
static uint8_t s_held[256][2];          /* per HID usage: the Spectrum keys it pressed */
static uint8_t s_modifiers;
static uint8_t s_joy_pad, s_joy_arrows;
static bool s_arrows_joystick;

static void key_ref(uint8_t key, bool down)
{
    if (key == NONE) {
        return;
    }
    bool was = s_count[key] > 0;
    s_count[key] = down ? s_count[key] + 1 : s_count[key] ? s_count[key] - 1 : 0;
    bool now = s_count[key] > 0;
    if (key == K_CAPS) {
        was = was && !s_no_caps;
        now = now && !s_no_caps;
    }
    if (was != now) {
        zx_key(key, now);
    }
}

/* Keys like ";" are SYMBOL SHIFT + O: with the PC's Shift held, CAPS
 * SHIFT has to let go meanwhile
 */
static void caps_hold_off(bool on)
{
    bool was = s_count[K_CAPS] > 0 && !s_no_caps;
    s_no_caps = on ? s_no_caps + 1 : s_no_caps ? s_no_caps - 1 : 0;
    bool now = s_count[K_CAPS] > 0 && !s_no_caps;
    if (was != now) {
        zx_key(K_CAPS, now);
    }
}

static void press(uint8_t usage, const keymap_t *m, bool pressed)
{
    if (pressed) {
        bool shifted = (s_modifiers & 0x22) && m->shifted_key != NONE;
        uint8_t key = shifted ? m->shifted_key : m->key;
        uint8_t with = shifted ? m->shifted_with : m->with;
        if (with == K_SYM) {
            caps_hold_off(true);
        }
        s_held[usage][0] = key;
        s_held[usage][1] = with;
        key_ref(with, true);
        key_ref(key, true);
    } else {
        uint8_t key = s_held[usage][0], with = s_held[usage][1];
        if (key == 0 && with == 0) {
            return;
        }
        key_ref(key, false);
        key_ref(with, false);
        if (with == K_SYM) {
            caps_hold_off(false);
        }
        s_held[usage][0] = s_held[usage][1] = 0;
    }
}

static void joystick_update(void)
{
    zx_joystick(s_joy_pad | s_joy_arrows);
}

/* Numeric keypad: directions and fire */
static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        [0x59 - 0x59] = ZX_JOY_DOWN | ZX_JOY_LEFT, ZX_JOY_DOWN, ZX_JOY_DOWN | ZX_JOY_RIGHT,
        ZX_JOY_LEFT, ZX_JOY_FIRE, ZX_JOY_RIGHT,
        ZX_JOY_UP | ZX_JOY_LEFT, ZX_JOY_UP, ZX_JOY_UP | ZX_JOY_RIGHT,
        ZX_JOY_FIRE,                    /* 0 */
        ZX_JOY_FIRE,                    /* . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_joy_pad = pressed ? s_joy_pad | b : s_joy_pad & ~b;
    joystick_update();
    return true;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        if (s_arrows_joystick && (bit == 2 || bit == 6)) {
            /* Alt is fire while the cursor keys are the joystick */
            s_joy_arrows = pressed ? s_joy_arrows | ZX_JOY_FIRE : s_joy_arrows & ~ZX_JOY_FIRE;
            joystick_update();
            return;
        }
        /* LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
        static const uint8_t keys[8] = { K_SYM, K_CAPS, K_SYM, NONE, K_SYM, K_CAPS, K_SYM, NONE };
        bool was = s_modifiers & (1u << bit);
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        if (was != pressed) {
            key_ref(keys[bit], pressed);
        }
        return;
    }
    if (!pressed && usage >= 0x42 && usage <= 0x45) {
        return;
    }
    switch (usage) {
    case 0x42:                          /* F9: tape play / stop */
    {
        zx_tape_status_t ts;
        zx_tape_status(&ts);
        zx_tape_play(!ts.playing);
        ESP_LOGI(TAG, "tape %s", ts.playing ? "stopped" : "playing");
        return;
    }
    case 0x43:                          /* F10: rewind */
        zx_tape_rewind();
        ESP_LOGI(TAG, "tape rewound");
        return;
    case 0x44:                          /* F11: joystick type */
        settings.joystick = (settings.joystick + 1) % ZX_JOYSTICK_COUNT;
        zx_joystick_type(settings.joystick);
        settings_save();
        ESP_LOGI(TAG, "joystick: %s", joystick_names[settings.joystick]);
        return;
    case 0x45:                          /* F12: cursor keys <-> joystick */
        s_arrows_joystick = !s_arrows_joystick;
        s_joy_arrows = 0;
        joystick_update();
        ESP_LOGI(TAG, "cursor keys: %s", s_arrows_joystick ? "joystick" : "cursor keys");
        return;
    default:
        break;
    }
    if (usage >= 0x4F && usage <= 0x52) {
        if (s_arrows_joystick) {
            static const uint8_t dir[4] = { ZX_JOY_RIGHT, ZX_JOY_LEFT, ZX_JOY_DOWN, ZX_JOY_UP };
            uint8_t b = dir[usage - 0x4F];
            s_joy_arrows = pressed ? s_joy_arrows | b : s_joy_arrows & ~b;
            joystick_update();
        } else {
            press(usage, &arrow_map[usage - 0x4F], pressed);
        }
        return;
    }
    if (keypad_joystick(usage, pressed)) {
        return;
    }
    if (usage == 0x39) {
        press(usage, &capslock_map, pressed);
        return;
    }
    if (usage == 0x58) {                /* keypad Enter */
        press(usage, &usage_map[0x28], pressed);
        return;
    }
    if (usage == 0x64) {                /* non-US \ (ISO key left of Z) */
        press(usage, &usage_map[0x31], pressed);
        return;
    }
    if (usage < sizeof(usage_map) / sizeof(usage_map[0]) && (usage_map[usage].key || usage_map[usage].with)) {
        press(usage, &usage_map[usage], pressed);
    }
}

/* ---- Setup screen choices ---- */

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < ZX_MODEL_COUNT) {
        settings.model = i;
        settings_save();
    }
}

static const char *const tape_names[] = { "Fast", "Real time", "" };

static int tape_get(void)
{
    return settings.tape_fast ? 0 : 1;
}

static void tape_set(int i)
{
    settings.tape_fast = i == 0;
    zx_tape_fast(settings.tape_fast);
    settings_save();
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set, false },
    { "Tape", tape_names, tape_get, tape_set, false },
};

/* ---- Drives ---- */

static const char *const extensions[] = { ".dsk", ".trd", ".scl", ".tap", ".tzx", ".sna", ".z80", NULL };
static const char *const drive_names[] = { "A:", "B:", "Tape", "Snapshot" };
static const disks_size_t sizes[] = {
    { ZX_TRD_BYTES, "640 KB TR-DOS disk (128K, +2)" },
    { ZX_PLUS3_BYTES, "173 KB +3 disk" },
    { BLANK_TAPE, "Empty tape (.TAP)" },
};

static bool accepts(int drive, const char *name)
{
    switch (drive) {
    case DRIVE_TAPE: return is_tape(name);
    case DRIVE_SNAPSHOT: return is_snapshot(name);
    default: return zx_disk_fits(settings.model, name);
    }
}

static bool takes_blank(int drive)
{
    return drive != DRIVE_SNAPSHOT;
}

static uint32_t default_size(int drive)
{
    if (drive == DRIVE_TAPE) {
        return BLANK_TAPE;
    }
    return settings.model == ZX_MODEL_PLUS3 ? ZX_PLUS3_BYTES : ZX_TRD_BYTES;
}

static const char *extension_for(uint32_t bytes)
{
    return bytes == BLANK_TAPE ? ".tap" : bytes == ZX_PLUS3_BYTES ? ".dsk" : ".trd";
}

static bool insert(int drive, const char *path)
{
    if (!accepts(drive, path)) {
        return false;
    }
    switch (drive) {
    case DRIVE_TAPE:
        return zx_tape_insert(path);
    case DRIVE_SNAPSHOT:
        /* loads now, and again whenever the machine starts */
        if (s_started && !zx_snapshot_load(path)) {
            return false;
        }
        snprintf(s_snapshot, sizeof(s_snapshot), "%s", path);
        return true;
    default:
        return zx_disk_insert(drive, path);
    }
}

static void eject(int drive)
{
    switch (drive) {
    case DRIVE_TAPE: zx_tape_eject(); break;
    case DRIVE_SNAPSHOT: s_snapshot[0] = 0; break;
    default: zx_disk_eject(drive); break;
    }
}

static const char *path(int drive)
{
    switch (drive) {
    case DRIVE_TAPE: return zx_tape_path();
    case DRIVE_SNAPSHOT: return s_snapshot[0] ? s_snapshot : NULL;
    default: return zx_disk_path(drive);
    }
}

static bool write_protected(int drive)
{
    switch (drive) {
    case DRIVE_TAPE: return zx_tape_path() && !has_ext(zx_tape_path(), ".tap");
    case DRIVE_SNAPSHOT: return true;
    default: return zx_disk_write_protected(drive);
    }
}

static bool create(const char *path, uint32_t bytes)
{
    return bytes == BLANK_TAPE ? zx_tape_create(path) : zx_disk_create(path, bytes);
}

static const disks_config_t disks = {
    .folder = "zx",
    .extensions = extensions,
    .description = "Disks (.DSK .TRD .SCL), tapes (.TAP .TZX), snapshots (.SNA .Z80)",
    .title = "Disks, tape and snapshot",
    .button = "Disks",
    .nvs_namespace = "zxdisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".trd",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 4 * 1024 * 1024,
    .insert = insert,
    .eject = eject,
    .path = path,
    .write_protected = write_protected,
    .create = create,
    .accepts = accepts,
    .takes_blank = takes_blank,
    .default_size = default_size,
    .extension_for = extension_for,
};

/* Disk writes go to the SD card from here (core 0), between frames, once
 * nothing has been written for a second
 */
static void flush(void *arg)
{
    zx_disk_flush();
}

static void every_second(void)
{
    static int dirty_for;
    if (zx_disk_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ---- */

static void tape_text(char *text, size_t len)
{
    zx_tape_status_t ts;
    zx_tape_status(&ts);
    snprintf(text, len, "block %d of %d%s%s", ts.block < ts.blocks ? ts.block + 1 : ts.blocks, ts.blocks,
             ts.playing ? ", playing" : ", stopped", ts.fast ? "" : ", real time");
}

/* The web page's machine card: the state now, a short line a row. The
 * whole /api/status reply has 4 KB (esp32emu's STATUS_SIZE), these rows
 * at most ~1.8 KB of it: keep them short (a " goes out as \").
 */

/* The colours as the ULA draws them (ula.c): 0-7, 8-15 BRIGHT */
static uint32_t colour_rgb(unsigned c)
{
    unsigned level = c & 8 ? 0xFF : 0xD7;
    return ((c & 2) ? level << 16 : 0) | ((c & 4) ? level << 8 : 0) | ((c & 1) ? level : 0);
}

static size_t swatch(char *text, size_t size, uint32_t rgb)
{
    int n = snprintf(text, size, "<b style=\"background:#%06lx\">&nbsp;&nbsp;&nbsp;</b> ",
                     (unsigned long)(rgb & 0xFFFFFF));
    return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

/* The AY's channels as they sound now (its clock 1.7734 MHz), the beeper */
static void sound_text(char *text, size_t size, const zx_status_t *s)
{
    zx_sound_t snd;
    zx_get_sound(&snd);
    const uint8_t *r = snd.reg;
    const uint32_t clock = ZX_CLOCK_HZ / 2;
    size_t n = 0;
    bool envelope = false;
    for (int c = 0; c < 3 && n < size; c++) {
        bool tone = !(r[7] & (1 << c)), noise = !(r[7] & (8 << c));
        unsigned volume = r[8 + c] & 31;
        if (volume == 0) {
            continue;
        }
        unsigned period = r[c * 2] | ((r[c * 2 + 1] & 15) << 8);
        char what[40];
        if (tone) {
            snprintf(what, sizeof(what), "%lu Hz%s", (unsigned long)(clock / (16 * (period ? period : 1))),
                     noise ? " + noise" : "");
        } else {
            snprintf(what, sizeof(what), noise ? "noise" : "a fixed level");
        }
        char level[16];
        snprintf(level, sizeof(level), "volume %u", volume);
        n += snprintf(text + n, size - n, "%s%c %s, %s", n ? "; " : "AY: ", 'A' + c, what,
                      (volume & 16) ? "envelope" : level);
        envelope |= volume & 16;
    }
    if (n == 0) {
        n = snprintf(text, size, "AY silent");
    }
    if (envelope && n < size) {
        /* one sweep of the envelope: 256 x its period in AY cycles */
        unsigned period = r[11] | (r[12] << 8);
        uint64_t ms = (256ull * (period ? period : 1) * 1000 + clock / 2) / clock;
        n += snprintf(text + n, size - n, "; envelope shape %u, %lu ms", r[13] & 15, (unsigned long)ms);
    }
    if (n < size) {
        /* two changes a cycle, 50 frames a second */
        if (s->beeper) {
            snprintf(text + n, size - n, "; the beeper ~%u Hz", s->beeper * 25u);
        } else {
            snprintf(text + n, size - n, "; the beeper quiet");
        }
    }
}

static void status(emu_json_t *j, int section)
{
    char text[200];
    if (section == EMU_STATUS_ROWS) {
        zx_status_t s;
        zx_get_status(&s);
        bool plus3 = zx_model() == ZX_MODEL_PLUS3;
        char im[24];
        snprintf(im, sizeof(im), s.im == 2 ? "IM 2, I = $%02X" : "IM %u", s.im == 2 ? s.i : s.im);
        snprintf(text, sizeof(text), "%s: a Z80A at 3.55 MHz (%s, interrupts %s), 128 KB RAM, %s, an AY-3-8912",
                 model_full[zx_model()], im, s.ei ? "on" : "off", plus3 ? "the gate array" : "the ULA");
        emu_json_row(j, "System", text);

        /* the ROM at $0000 now: $7FFD bit 4 (+3: and $1FFD bit 2) picks it */
        static const char *const roms_128[2] = { "the 128 editor", "48 BASIC" };
        static const char *const roms_plus3[4] = { "the editor", "the syntax checker", "+3DOS", "48 BASIC" };
        unsigned rom = ((s.port_7ffd >> 4) & 1) | (plus3 ? (s.port_1ffd >> 1) & 2 : 0);
        char now[40];
        if (s.trdos) {
            snprintf(now, sizeof(now), "TR-DOS");
        } else if (plus3 && (s.port_1ffd & 1)) {
            snprintf(now, sizeof(now), "RAM");
        } else {
            snprintf(now, sizeof(now), "ROM %u (%s)", rom, plus3 ? roms_plus3[rom] : roms_128[rom]);
        }
        snprintf(text, sizeof(text), "%s; at $0000 now: %s", s_rom_text, now);
        emu_json_row(j, "ROM", text);

        /* the 16 KB banks at $0000, $4000, $8000, $C000 */
        char ports[32];
        if (plus3) {
            snprintf(ports, sizeof(ports), "$7FFD = $%02X, $1FFD = $%02X", s.port_7ffd, s.port_1ffd);
        } else {
            snprintf(ports, sizeof(ports), "$7FFD = $%02X", s.port_7ffd);
        }
        if (plus3 && (s.port_1ffd & 1)) {
            static const uint8_t special[4][4] = {
                { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 4, 5, 6, 3 }, { 4, 7, 6, 3 },
            };
            const uint8_t *b = special[(s.port_1ffd >> 1) & 3];
            snprintf(text, sizeof(text), "%s: all RAM, banks %u, %u, %u, %u at $0000-$FFFF", ports, b[0], b[1], b[2],
                     b[3]);
        } else {
            snprintf(text, sizeof(text), "%s: ROM, banks 5, 2, %u at $0000-$FFFF", ports, s.port_7ffd & 7);
        }
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, "%s%s", (s.port_7ffd & 0x20) ? "; paging locked (48K)" : "",
                 plus3 && (s.port_1ffd & 0x08) ? "; disk motor on" : "");
        emu_json_row(j, "Memory", text);

        static const char *const names[8] = { "black", "blue", "red", "magenta", "green", "cyan", "yellow",
                                              "white" };
        snprintf(text, sizeof(text), "256 × 192, 2 colours a cell of 8 × 8 (attributes); the %s screen (bank %u); "
                 "border %s", (s.port_7ffd & 0x08) ? "shadow" : "normal", (s.port_7ffd & 0x08) ? 7 : 5,
                 names[s.border & 7]);
        emu_json_row(j, "Screen", text);

        /* the colours the attributes use now, of the 15 */
        uint16_t used;
        unsigned flashing;
        zx_screen_colours(&used, &flashing);
        static char swatches[1000];
        n = (size_t)snprintf(swatches, sizeof(swatches), "%d of 15: ", __builtin_popcount(used));
        for (unsigned c = 0; c < 16; c++) {
            if (used & (1u << c)) {
                n += swatch(swatches + n, sizeof(swatches) - n, colour_rgb(c));
            }
        }
        if (flashing && n < sizeof(swatches)) {
            snprintf(swatches + n, sizeof(swatches) - n, "%u cells flashing", flashing);
        }
        emu_json_row(j, "Colours", swatches);

        sound_text(text, sizeof(text), &s);
        emu_json_row(j, "Sound", text);
        emu_json_row(j, "Keyboard", "By position; Shift CAPS SHIFT, Ctrl or Alt SYMBOL SHIFT, Esc BREAK, Backspace "
                     "DELETE, Tab EXTEND MODE, punctuation as on the PC; F9 tape play/stop, F10 rewind");
        snprintf(text, sizeof(text), "%s on the keypad%s; F11: Kempston, Sinclair, cursor; F12: %s",
                 joystick_names[settings.joystick], s_arrows_joystick ? " and the cursor keys (0, 5, Alt fire)" :
                 " (0, 5 fire)", s_arrows_joystick ? "off" : "the cursor keys too");
        emu_json_row(j, "Joystick", text);
        snprintf(text, sizeof(text), "%s; tapes .TAP, .TZX (%s); snapshots .SNA, .Z80",
                 plus3 ? "The +3's drive (a uPD765): .DSK" :
                 s_cfg.trdos ? "Beta 128 (a WD1793, TR-DOS): .TRD, .SCL (read-only)" : "No disks (no trdos.rom)",
                 settings.tape_fast ? "standard blocks at once" : "in real time");
        emu_json_row(j, "Storage", text);
        return;
    }
    zx_disk_status_t ds;
    zx_disk_status(&ds);
    for (int d = DRIVE_A; d <= DRIVE_B; d++) {
        const char *name = disks_name(d);
        snprintf(text, sizeof(text), "track %u", ds.track[d]);
        uint64_t free_bytes = 0, size = 0;
        zx_disk_space(d, &free_bytes, &size);
        emu_json_drive_space(j, drive_names[d], name, zx_disk_write_protected(d), ds.light == d, ds.reads[d],
                             ds.writes[d], name ? text : NULL, free_bytes, size);
    }
    const char *tape = disks_name(DRIVE_TAPE);
    tape_text(text, sizeof(text));
    zx_tape_status_t ts;
    zx_tape_status(&ts);
    emu_json_drive(j, drive_names[DRIVE_TAPE], tape, write_protected(DRIVE_TAPE), ts.playing, ts.loaded,
                   ts.saved, tape ? text : NULL);
    const char *snap = disks_name(DRIVE_SNAPSHOT);
    emu_json_drive(j, drive_names[DRIVE_SNAPSHOT], snap, true, false, 0, 0, NULL);
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    zx_status_t s;
    zx_get_status(&s);
    char tape[64];
    tape_text(tape, sizeof(tape));
    snprintf(buf, len, "frames %lu PC $%04X 7FFD %02X 1FFD %02X%s tape %s", (unsigned long)s.frames, s.pc,
             s.port_7ffd, s.port_1ffd, s.trdos ? " TR-DOS" : "", tape);
}

/*   k <n>              tap key number n (half-row * 8 + bit, e.g. 48 ENTER)
 *   K <n> <0|1>        key n down (1) or up (0)
 *   T <text>           type text (\n is ENTER)
 *   j <bits>           joystick (1 right 2 left 4 down 8 up 16 fire)
 *   P                  tape: play / stop
 *   W                  tape: rewind
 *   R <model>          restart as model 0 128K, 1 +2, 2 +3
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned key = strtoul(line + 1, NULL, 10);
        zx_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        zx_key(key, false);
        return true;
    }
    case 'K': {
        unsigned key = 0, down = 0;
        if (sscanf(line + 1, "%u %u", &key, &down) == 2) {
            zx_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[200];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        zx_type(text);
        return true;
    }
    case 'j':
        zx_joystick(strtoul(line + 1, NULL, 0));
        return true;
    case 'P': {
        zx_tape_status_t ts;
        zx_tape_status(&ts);
        zx_tape_play(!ts.playing);
        return true;
    }
    case 'W':
        zx_tape_rewind();
        return true;
    case 'R': {
        int model = 0;
        if (sscanf(line + 1, "%d", &model) == 1 && model >= 0 && model < ZX_MODEL_COUNT) {
            settings.model = model;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "ZX Spectrum",
    .short_name = "Spectrum",
    .view_width = ZX_SCREEN_WIDTH * 2,  /* 352 x 300, doubled: 704 x 600 */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = zx_reset,
    .run_frame = zx_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = zx_screen_signature,
    .render_line = zx_render_line_rgb565,
    .audio_hz = ZX_AUDIO_HZ,
    .audio_enable = zx_audio_enable,
    .audio_render = zx_audio_render,
    .audio_pending = zx_audio_pending,
    .hid_key = hid_key,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
