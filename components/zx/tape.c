/*
 * The tape: .TAP and .TZX, played as pulses on the EAR input, and the ROM
 * traps that load and save standard blocks at once.
 *
 * The player turns blocks into segments of constant level: pilot, sync and
 * data pulses (each pulse flips the level), direct recordings, pauses
 * (level low). The EAR level is worked out when the CPU reads port $FE.
 *
 * Fast loading (the default): at LD-BYTES ($056B, past its setup) with the
 * tape at the start of a standard-speed block, the block goes straight into
 * memory and the routine returns; a turbo or custom block after it starts
 * the tape playing in real time. Without fast loading the tape plays when
 * the ROM's loader starts. SA-BYTES ($04C2) appends a block to an inserted
 * .TAP file.
 *
 * TZX blocks: $10 standard, $11 turbo, $12 pure tone, $13 pulses, $14 pure
 * data, $15 direct recording, $19 generalized data (up to 2 symbol pulses
 * and RLE pilots), $20 pause / stop, $21 $22 groups, $23 jump, $24 $25
 * loops, $2A stop in 48K mode (ignored), $2B signal level, and the
 * information blocks, skipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zx_internal.h"

#define MAX_TAPE        (4 * 1024 * 1024)
#define MAX_BLOCKS      4096
#define MS_T            3547            /* T-states per millisecond */

typedef struct {
    char path[256];
    bool tzx;
    uint8_t *data;
    uint32_t size;
    uint32_t *block;                    /* offsets: TAP a block's length word, TZX its ID */
    int blocks;
} tape_file_t;

static tape_file_t s_tape;

enum { SEG_TOGGLE, SEG_LOW, SEG_HIGH, SEG_KEEP };

static struct {
    bool playing, fast;
    bool level;
    uint64_t next_t;                    /* when the current segment ends */
    int blk;                            /* block being played */
    bool started;                       /* its setup is done */
    /* Pulse blocks */
    int phase;
    uint32_t pilot_len, pilot_count, sync1, sync2, zero, one;
    uint8_t last_bits;
    uint32_t pause_ms;
    const uint8_t *data;
    uint32_t len, pos;
    int bit, half;
    uint32_t count;
    /* Direct recording */
    uint32_t sample_t;
    /* Generalized data ($19) */
    const uint8_t *gp, *gd;
    uint32_t totp, totd;
    uint8_t npp, asp, npd, asd;
    uint32_t sym_i, sym_rep, sym_pulse;
    int sym_cur;
    const uint8_t *symdefs;             /* current table (pilot or data) */
    uint8_t sym_np;
    int dbits;
    /* Loops */
    int loop_start, loop_count;
    uint32_t loaded, saved;
    /* Loader detection: EAR reads this frame, frames of polling or not */
    uint32_t reads;
    int busy_frames, idle_frames;
    bool auto_play;                     /* playing because a loader polls */
} t;

enum { PH_START, PH_PILOT, PH_SYNC1, PH_SYNC2, PH_DATA, PH_PAUSE, PH_TONE, PH_PULSES,
       PH_DIRECT, PH_GEN_PILOT, PH_GEN_DATA, PH_PAUSE_LOW, PH_END };

static inline uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static inline uint32_t le24(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t le32(const uint8_t *p)
{
    return le24(p) | ((uint32_t)p[3] << 24);
}

/* ---- Files ---- */

/* A TZX block's total length (ID included), 0 if unknown or cut short */
static uint32_t tzx_block_len(const uint8_t *p, uint32_t avail)
{
    if (avail < 1) {
        return 0;
    }
    uint32_t n;
#define NEED(k) do { if (avail < (k)) return 0; } while (0)
    switch (p[0]) {
    case 0x10: NEED(5); n = 5 + le16(p + 3); break;
    case 0x11: NEED(19); n = 19 + le24(p + 16); break;
    case 0x12: n = 5; break;
    case 0x13: NEED(2); n = 2 + p[1] * 2; break;
    case 0x14: NEED(11); n = 11 + le24(p + 8); break;
    case 0x15: NEED(9); n = 9 + le24(p + 6); break;
    case 0x18: case 0x19: NEED(5); n = 5 + le32(p + 1); break;
    case 0x20: case 0x23: case 0x24: n = 3; break;
    case 0x21: case 0x30: NEED(2); n = 2 + p[1]; break;
    case 0x22: case 0x25: case 0x27: n = 1; break;
    case 0x26: NEED(3); n = 3 + le16(p + 1) * 2; break;
    case 0x28: case 0x32: NEED(3); n = 3 + le16(p + 1); break;
    case 0x2A: n = 5; break;
    case 0x2B: n = 6; break;
    case 0x31: NEED(3); n = 3 + p[2]; break;
    case 0x33: NEED(2); n = 2 + p[1] * 3; break;
    case 0x35: NEED(21); n = 21 + le32(p + 17); break;
    case 0x5A: n = 10; break;
    default:
        /* the extension rule: a 32-bit length after the ID */
        NEED(5); n = 5 + le32(p + 1); break;
    }
#undef NEED
    return n <= avail ? n : 0;
}

static bool index_blocks(tape_file_t *f)
{
    f->block = zx_malloc(MAX_BLOCKS * sizeof(uint32_t));
    if (f->block == NULL) {
        return false;
    }
    f->blocks = 0;
    uint32_t pos = 0;
    if (f->tzx) {
        if (f->size < 10 || memcmp(f->data, "ZXTape!\x1A", 8) != 0) {
            return false;
        }
        pos = 10;
        while (pos < f->size && f->blocks < MAX_BLOCKS) {
            uint32_t n = tzx_block_len(f->data + pos, f->size - pos);
            if (n == 0) {
                break;
            }
            f->block[f->blocks++] = pos;
            pos += n;
        }
    } else {
        while (pos + 2 <= f->size && f->blocks < MAX_BLOCKS) {
            uint32_t n = le16(f->data + pos);
            if (pos + 2 + n > f->size) {
                break;
            }
            f->block[f->blocks++] = pos;
            pos += 2 + n;
        }
    }
    return true;
}

static void free_tape(void)
{
    free(s_tape.data);
    free(s_tape.block);
    memset(&s_tape, 0, sizeof(s_tape));
}

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

static void rewind_state(void)
{
    t.blk = 0;
    t.started = false;
    t.phase = PH_START;
    t.level = false;
    t.loop_count = 0;
}

bool zx_tape_insert(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = size >= 0 && size <= MAX_TAPE ? zx_malloc(size ? size : 1) : NULL;
    if (data == NULL || fread(data, 1, size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return false;
    }
    fclose(fp);
    free_tape();
    s_tape.data = data;
    s_tape.size = size;
    s_tape.tzx = has_ext(path, ".tzx");
    if (!index_blocks(&s_tape)) {
        free_tape();
        return false;
    }
    snprintf(s_tape.path, sizeof(s_tape.path), "%s", path);
    rewind_state();
    t.playing = false;
    ZX_LOG("tape %s: %d blocks", path, s_tape.blocks);
    return true;
}

void zx_tape_eject(void)
{
    free_tape();
    rewind_state();
    t.playing = false;
}

const char *zx_tape_path(void)
{
    return s_tape.data ? s_tape.path : NULL;
}

bool zx_tape_create(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }
    fclose(fp);
    return true;
}

void zx_tape_rewind(void)
{
    rewind_state();
}

void zx_tape_play(bool play)
{
    t.auto_play = false;
    if (play && s_tape.data && !t.playing) {
        t.next_t = zx_now();
        if (t.blk >= s_tape.blocks) {
            rewind_state();
        }
    }
    t.playing = play && s_tape.data != NULL;
}

void zx_tape_fast(bool fast)
{
    t.fast = fast;
}

void zx_tape_status(zx_tape_status_t *s)
{
    s->inserted = s_tape.data != NULL;
    s->playing = t.playing;
    s->fast = t.fast;
    s->block = t.blk;
    s->blocks = s_tape.blocks;
    s->loaded = t.loaded;
    s->saved = t.saved;
}

/* ---- The player ---- */

static void next_block(void)
{
    t.blk++;
    t.started = false;
    t.phase = PH_START;
}

static void set_pulse_block(uint32_t pilot, uint32_t count, uint32_t s1, uint32_t s2, uint32_t zero,
                            uint32_t one, uint8_t last_bits, uint32_t pause, const uint8_t *data, uint32_t len)
{
    t.pilot_len = pilot;
    t.pilot_count = count;
    t.sync1 = s1;
    t.sync2 = s2;
    t.zero = zero;
    t.one = one;
    t.last_bits = last_bits ? last_bits : 8;
    t.pause_ms = pause;
    t.data = data;
    t.len = len;
    t.pos = 0;
    t.bit = 7;
    t.half = 0;
    t.count = 0;
    t.phase = count ? PH_PILOT : s1 ? PH_SYNC1 : len ? PH_DATA : PH_PAUSE;
}

/* Generalized data: symbol table entries are flags + npp/npd pulse lengths */
static uint32_t gen_symbol_pulse(const uint8_t *defs, uint8_t np, int sym, uint32_t i)
{
    return le16(defs + sym * (1 + 2 * np) + 1 + 2 * i);
}

static uint8_t gen_symbol_flags(const uint8_t *defs, uint8_t np, int sym)
{
    return defs[sym * (1 + 2 * np)];
}

/* Set up the block t.blk; false at the end of the tape */
static bool start_block(void)
{
    for (;;) {
        if (t.blk >= s_tape.blocks) {
            t.phase = PH_END;
            return false;
        }
        const uint8_t *p = s_tape.data + s_tape.block[t.blk];
        t.started = true;
        if (!s_tape.tzx) {
            uint32_t n = le16(p);
            set_pulse_block(2168, n && p[2] < 0x80 ? 8063 : 3223, 667, 735, 855, 1710, 8, 1000, p + 2, n);
            return true;
        }
        switch (p[0]) {
        case 0x10: {
            uint32_t n = le16(p + 3);
            set_pulse_block(2168, n && p[5] < 0x80 ? 8063 : 3223, 667, 735, 855, 1710, 8, le16(p + 1),
                            p + 5, n);
            return true;
        }
        case 0x11:
            set_pulse_block(le16(p + 1), le16(p + 11), le16(p + 3), le16(p + 5), le16(p + 7), le16(p + 9),
                            p[13], le16(p + 14), p + 19, le24(p + 16));
            return true;
        case 0x12:
            t.pilot_len = le16(p + 1);
            t.count = le16(p + 3);
            t.phase = PH_TONE;
            return true;
        case 0x13:
            t.data = p + 2;
            t.len = p[1];
            t.pos = 0;
            t.phase = PH_PULSES;
            return true;
        case 0x14:
            set_pulse_block(0, 0, 0, 0, le16(p + 1), le16(p + 3), p[5], le16(p + 6), p + 11, le24(p + 8));
            return true;
        case 0x15:
            t.sample_t = le16(p + 1);
            t.pause_ms = le16(p + 3);
            t.last_bits = p[5] ? p[5] : 8;
            t.data = p + 9;
            t.len = le24(p + 6);
            t.pos = 0;
            t.bit = 7;
            t.phase = t.len ? PH_DIRECT : PH_PAUSE;
            return true;
        case 0x19: {
            const uint8_t *q = p + 5;
            t.pause_ms = le16(q);
            t.totp = le32(q + 2);
            t.npp = q[6];
            t.asp = q[7];
            t.totd = le32(q + 8);
            t.npd = q[12];
            t.asd = q[13];
            const uint8_t *r = q + 14;
            t.gp = r;                                   /* pilot symbols, then the PRLE table */
            r += t.totp ? (1 + 2 * t.npp) * (t.asp ? t.asp : 256) + 3 * t.totp : 0;
            t.gd = r;                                   /* data symbols, then the stream */
            t.dbits = 0;
            for (int v = (t.asd ? t.asd : 256) - 1; v > 0; v >>= 1) {
                t.dbits++;
            }
            t.sym_i = 0;
            t.sym_rep = 0;
            t.sym_pulse = 0;
            t.sym_cur = -1;
            t.phase = t.totp ? PH_GEN_PILOT : t.totd ? PH_GEN_DATA : PH_PAUSE;
            return true;
        }
        case 0x20:
            if (le16(p + 1) == 0) {
                t.playing = false;      /* stop the tape */
                next_block();
                return false;
            }
            t.pause_ms = le16(p + 1);
            t.phase = PH_PAUSE;
            return true;
        case 0x23: {
            int16_t rel = (int16_t)le16(p + 1);
            t.blk += rel ? rel : 1;
            continue;
        }
        case 0x24:
            t.loop_count = le16(p + 1);
            t.loop_start = t.blk + 1;
            t.blk++;
            continue;
        case 0x25:
            if (t.loop_count > 1) {
                t.loop_count--;
                t.blk = t.loop_start;
            } else {
                t.blk++;
            }
            continue;
        case 0x2B:
            t.level = p[5] & 1;
            t.blk++;
            continue;
        default:
            t.blk++;                    /* information, groups, ... */
            continue;
        }
    }
}

/* The next segment: its length and what happens to the level at its start */
static bool next_segment(uint32_t *len, int *kind)
{
    for (;;) {
        if (!t.started || t.phase == PH_START) {
            if (!start_block()) {
                return false;
            }
        }
        *kind = SEG_TOGGLE;
        switch (t.phase) {
        case PH_PILOT:
            if (t.count < t.pilot_count) {
                t.count++;
                *len = t.pilot_len;
                return true;
            }
            t.phase = t.sync1 ? PH_SYNC1 : PH_DATA;
            continue;
        case PH_SYNC1:
            t.phase = PH_SYNC2;
            *len = t.sync1;
            return true;
        case PH_SYNC2:
            t.phase = PH_DATA;
            if (t.sync2) {
                *len = t.sync2;
                return true;
            }
            continue;
        case PH_DATA: {
            int last = t.pos + 1 == t.len ? 8 - t.last_bits : 0;
            if (t.pos >= t.len || (t.pos + 1 == t.len && t.bit < last)) {
                t.phase = PH_PAUSE;
                continue;
            }
            bool one = (t.data[t.pos] >> t.bit) & 1;
            *len = one ? t.one : t.zero;
            if (++t.half == 2) {
                t.half = 0;
                if (--t.bit < 0) {
                    t.bit = 7;
                    t.pos++;
                }
            }
            return true;
        }
        case PH_TONE:
            if (t.count) {
                t.count--;
                *len = t.pilot_len;
                return true;
            }
            next_block();
            continue;
        case PH_PULSES:
            if (t.pos < t.len) {
                *len = le16(t.data + t.pos * 2);
                t.pos++;
                return true;
            }
            next_block();
            continue;
        case PH_DIRECT: {
            int last = t.pos + 1 == t.len ? 8 - t.last_bits : 0;
            if (t.pos >= t.len || (t.pos + 1 == t.len && t.bit < last)) {
                t.phase = PH_PAUSE;
                continue;
            }
            bool high = (t.data[t.pos] >> t.bit) & 1;
            if (--t.bit < 0) {
                t.bit = 7;
                t.pos++;
            }
            *kind = high ? SEG_HIGH : SEG_LOW;
            *len = t.sample_t;
            return true;
        }
        case PH_GEN_PILOT: case PH_GEN_DATA: {
            bool pilot = t.phase == PH_GEN_PILOT;
            uint8_t np = pilot ? t.npp : t.npd;
            const uint8_t *defs = pilot ? t.gp : t.gd;
            uint32_t nsym = pilot ? (t.asp ? t.asp : 256) : (t.asd ? t.asd : 256);
            /* the current symbol's next pulse */
            while (t.sym_cur >= 0 && t.sym_pulse < np) {
                uint32_t pl = gen_symbol_pulse(defs, np, t.sym_cur, t.sym_pulse);
                uint8_t flags = gen_symbol_flags(defs, np, t.sym_cur) & 3;
                bool first = t.sym_pulse == 0;
                t.sym_pulse++;
                if (pl == 0) {
                    t.sym_pulse = np;
                    break;
                }
                *len = pl;
                *kind = !first ? SEG_TOGGLE : flags == 0 ? SEG_TOGGLE : flags == 1 ? SEG_KEEP
                        : flags == 2 ? SEG_LOW : SEG_HIGH;
                return true;
            }
            /* the next symbol */
            if (pilot) {
                const uint8_t *prle = t.gp + (1 + 2 * np) * nsym;
                if (t.sym_rep == 0) {
                    if (t.sym_i >= t.totp) {
                        t.sym_cur = -1;
                        t.sym_i = 0;
                        t.phase = t.totd ? PH_GEN_DATA : PH_PAUSE;
                        continue;
                    }
                    t.sym_cur = prle[t.sym_i * 3];
                    t.sym_rep = le16(prle + t.sym_i * 3 + 1);
                    t.sym_i++;
                }
                t.sym_rep--;
            } else {
                if (t.sym_i >= t.totd) {
                    t.sym_cur = -1;
                    t.phase = PH_PAUSE;
                    continue;
                }
                const uint8_t *stream = t.gd + (1 + 2 * np) * nsym;
                uint32_t bitpos = t.sym_i * t.dbits;
                int sym = 0;
                for (int b = 0; b < t.dbits; b++, bitpos++) {
                    sym = (sym << 1) | ((stream[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
                }
                t.sym_cur = sym < (int)nsym ? sym : 0;
                t.sym_i++;
            }
            t.sym_pulse = 0;
            continue;
        }
        case PH_PAUSE:
            /* as libspectrum plays it: an edge that ends the last pulse,
             * 1 ms at that level, then low for the rest
             */
            if (t.pause_ms == 0) {
                next_block();
                continue;
            }
            t.phase = PH_PAUSE_LOW;
            *kind = SEG_TOGGLE;
            *len = MS_T;
            return true;
        case PH_PAUSE_LOW: {
            uint32_t rest = (t.pause_ms - 1) * MS_T;
            next_block();
            if (rest == 0) {
                continue;
            }
            *kind = SEG_LOW;
            *len = rest;
            return true;
        }
        default:
            return false;
        }
    }
}

/* Play up to now */
static void advance(uint64_t now)
{
    while (t.playing && t.next_t <= now) {
        uint32_t len;
        int kind;
        if (!next_segment(&len, &kind)) {
            /* the tape stops (or ends): an edge ends the last pulse, as
             * where each pulse ends with one (libspectrum)
             */
            t.level = !t.level;
            beeper_tape(t.level, t.next_t);
            if (t.phase == PH_END) {
                t.playing = false;
            }
            t.next_t = now;
            break;
        }
        bool old = t.level;
        switch (kind) {
        case SEG_TOGGLE: t.level = !t.level; break;
        case SEG_LOW: t.level = false; break;
        case SEG_HIGH: t.level = true; break;
        default: break;
        }
        if (t.level != old) {
            beeper_tape(t.level, t.next_t);
        }
        t.next_t += len ? len : 1;
    }
}

/* Loaders built on the ROM's edge loop, LD-SAMPLE:
 *
 *   INC B; RET Z; LD A,n; IN A,($FE); RRA; RET NC (or another RET cc);
 *   XOR C; AND $20; JR Z,LD-SAMPLE          (59 T-states a pass)
 *
 * With fast loading, an IN in such a loop brings the tape's next edge to
 * now, and B counts the passes that would have gone by: the loader sees
 * the same pulse lengths, in a fraction of the time.
 */
#define SAMPLE_PASS_T   59

static void accelerate(void)
{
#ifndef ESP_PLATFORM
    static int off = -1;
    if (off < 0) {
        off = getenv("ZX_NOACCEL") != NULL;
    }
    if (off) {
        return;
    }
#endif
    uint16_t s = z80.pc.w - 6;          /* PC is past the IN */
    if (z80_contended[s >> 14]) {
        return;
    }
    static const uint8_t loop[13] = { 0x04, 0xC8, 0x3E, 0, 0xDB, 0xFE, 0x1F, 0, 0xA9, 0xE6, 0x20, 0x28, 0xF3 };
    for (int i = 0; i < 13; i++) {
        uint8_t b = z80_peek(s + i);
        if (i == 3 || (i == 7 && (b & 0xC7) == 0xC0)) {
            continue;                   /* any constant, any RET cc */
        }
        if (b != loop[i]) {
            return;
        }
    }
    /* only while the level is still the one the loop waits to change */
    if (t.level != ((z80.bc.b.l >> 5) & 1)) {
        return;
    }
    uint64_t now = zx_now();
    if (t.next_t <= now) {
        return;
    }
    uint64_t n = (t.next_t - now) / SAMPLE_PASS_T;
    uint32_t room = 0xFF - z80.bc.b.h;  /* B reaching 0 is the loader's timeout */
    if (n > room) {
        n = room;
    }
    if (n == 0) {
        return;
    }
    z80.bc.b.h += n;
    z80.r = (z80.r & 0x80) | ((z80.r + n * 9) & 0x7F);
    t.next_t -= n * SAMPLE_PASS_T;
}

bool tape_inserted(void)
{
    return s_tape.data != NULL;
}

/* A stopped tape leaves the input at its last level */
bool tape_ear(void)
{
    t.reads++;
    if (!t.playing) {
        return t.level;
    }
    advance(zx_now());
    if (t.fast) {
        accelerate();
        advance(zx_now());
    }
    return t.level;
}

/* A loader polls the EAR input thousands of times a frame (a game reading
 * its keys a few dozen): play while one does, pause when it stops for 2 s
 */
#define LOADER_READS    1000
#define QUIET_READS     100

static void detect_loader(void)
{
    uint32_t reads = t.reads;
    t.reads = 0;
    if (s_tape.data == NULL) {
        return;
    }
    if (!t.playing) {
        t.busy_frames = reads >= LOADER_READS && t.blk < s_tape.blocks ? t.busy_frames + 1 : 0;
        if (t.busy_frames >= 3) {
            zx_tape_play(true);
            t.auto_play = true;
            t.idle_frames = 0;
        }
    } else if (t.auto_play) {
        t.idle_frames = reads < QUIET_READS ? t.idle_frames + 1 : 0;
        if (t.idle_frames >= 100) {
            t.playing = false;
            t.auto_play = false;
        }
    }
}

void tape_frame_end(void)
{
    if (t.playing) {
        advance(zx.clock + ZX_FRAME_T);
    }
    detect_loader();
}

void tape_reset(void)
{
    t.playing = false;
}

void tape_rom_loader(void)
{
    if (!t.fast && s_tape.data && !t.playing) {
        zx_tape_play(true);
    }
}

/* ---- Traps ---- */

/* A standard-speed data block at the tape's position, not begun: its bytes */
static bool standard_block(const uint8_t **data, uint32_t *len)
{
    if (t.blk >= s_tape.blocks) {
        return false;
    }
    bool fresh = !t.started || t.phase == PH_START || t.phase == PH_PILOT;
    if (!fresh) {
        return false;
    }
    const uint8_t *p = s_tape.data + s_tape.block[t.blk];
    if (!s_tape.tzx) {
        *len = le16(p);
        *data = p + 2;
        return true;
    }
    if (p[0] == 0x10) {
        *len = le16(p + 3);
        *data = p + 5;
        return true;
    }
    return false;
}

/* Skip blocks that make no sound (information, groups) */
static void skip_silent(void)
{
    while (s_tape.tzx && t.blk < s_tape.blocks) {
        uint8_t id = s_tape.data[s_tape.block[t.blk]];
        if (id < 0x21 || id == 0x23 || id == 0x24 || id == 0x25 || id == 0x2B) {
            break;
        }
        t.blk++;
    }
}

static inline void poke(uint16_t a, uint8_t v)
{
    uint8_t *p = z80_wr[a >> 14];
    if (p) {
        p[a & 0x3FFF] = v;
    }
}

/* What SA/LD-RET ($053F) does, without its BREAK test (the key that
 * started a save is still down when it ends at once): border back,
 * interrupts on, return with carry = success
 */
static void ld_ret(bool ok)
{
    uint8_t border = (z80_peek(0x5C48) >> 3) & 7;
    if (zx.ula_out & 0x18) {
        beeper(0, z80.t);
    }
    zx.ula_out = border;
    ula_border(border, z80.t);
    z80.iff1 = z80.iff2 = 1;
    z80.af.b.l = ok ? (z80.af.b.l | 0x01) : (z80.af.b.l & ~0x01);
    z80.pc.w = z80_peek(z80.sp.w) | (z80_peek(z80.sp.w + 1) << 8);
    z80.sp.w += 2;
}

bool tape_trap_load(void)
{
    if (!t.fast || s_tape.data == NULL) {
        return false;
    }
    skip_silent();
    if (t.blk >= s_tape.blocks && !t.playing) {
        rewind_state();                 /* LOAD at the end of the tape: from the start */
        skip_silent();
    }
    const uint8_t *b;
    uint32_t n;
    if (!standard_block(&b, &n)) {
        if (!t.playing && t.blk < s_tape.blocks) {
            zx_tape_play(true);         /* a turbo block: real time from here */
        }
        return false;
    }
    uint8_t flag = z80.af2 >> 8;
    bool load = z80.af2 & 0x01;
    uint16_t want = z80.de.w, ix = z80.ix.w;
    bool ok = false;
    if (n > 0 && b[0] == flag) {
        uint8_t parity = b[0];
        uint32_t i = 0;
        bool same = true;
        while (i < want && 1 + i < n) {
            uint8_t v = b[1 + i];
            parity ^= v;
            if (load) {
                poke(ix + i, v);
            } else {
                same &= z80_peek(ix + i) == v;
            }
            i++;
        }
        if (i == want && 1 + i < n) {
            parity ^= b[1 + i];
            ok = parity == 0 && same;
        }
        z80.ix.w = ix + i;
        z80.de.w = want - i;
    }
    next_block();
    t.loaded++;
    /* LD-BYTES returns to what it pushed, SA/LD-RET ($053F): do its work.
     * A loader that had the block overwrite that address (the stack in
     * the loaded area) goes wherever the block says instead.
     */
    uint16_t ret = z80_peek(z80.sp.w) | (z80_peek(z80.sp.w + 1) << 8);
    z80.sp.w += 2;
    if (ret == 0x053F) {
        ld_ret(ok);
    } else {
        z80.af.b.l = ok ? (z80.af.b.l | 0x01) : (z80.af.b.l & ~0x01);
        z80.pc.w = ret;
    }
    /* A turbo block next: play it */
    skip_silent();
    const uint8_t *nb;
    uint32_t nn;
    if (t.blk < s_tape.blocks && !standard_block(&nb, &nn)) {
        zx_tape_play(true);
    } else {
        t.playing = false;
    }
    return true;
}

bool tape_trap_save(void)
{
    if (!t.fast || s_tape.data == NULL || s_tape.tzx) {
        return false;
    }
    uint8_t flag = z80.af.b.h;
    uint16_t len = z80.de.w, ix = z80.ix.w;
    uint32_t n = (uint32_t)len + 2;
    if (s_tape.size + 2 + n > MAX_TAPE || s_tape.blocks >= MAX_BLOCKS) {
        return false;
    }
    uint8_t *data = zx_realloc(s_tape.data, s_tape.size + 2 + n);
    if (data == NULL) {
        return false;
    }
    s_tape.data = data;
    uint8_t *p = data + s_tape.size;
    p[0] = n;
    p[1] = n >> 8;
    p[2] = flag;
    uint8_t parity = flag;
    for (uint32_t i = 0; i < len; i++) {
        p[3 + i] = z80_peek(ix + i);
        parity ^= p[3 + i];
    }
    p[3 + len] = parity;
    FILE *fp = fopen(s_tape.path, "ab");
    bool ok = fp && fwrite(p, 1, 2 + n, fp) == 2 + n;
    if (fp) {
        fclose(fp);
    }
    if (!ok) {
        ZX_LOG("can't write %s", s_tape.path);
        return false;
    }
    s_tape.block[s_tape.blocks++] = s_tape.size;
    s_tape.size += 2 + n;
    t.blk = s_tape.blocks;              /* after the new block, as on a real tape */
    t.started = false;
    t.phase = PH_START;
    t.saved++;
    z80.ix.w = ix + len;
    z80.de.w = 0;
    ld_ret(true);
    return true;
}
