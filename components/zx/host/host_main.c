/*
 * Desktop build of the Spectrum core, for testing without the board:
 *
 *   zx_host <128|plus2|plus3> [frames] [out.ppm]
 *
 * ROMs from ZX_ROMS (default ../../../roms): 128.rom, plus2.rom, plus3.rom,
 * trdos.rom.
 *
 * Environment:
 *   ZX_DISKA, ZX_DISKB      disk images in A: and B: (.dsk, .trd, .scl)
 *   ZX_NEWDISK=<path>       make a blank disk first (.dsk: +3, else TR-DOS)
 *   ZX_TAPE=<file>          tape (.tap, .tzx); ZX_NEWTAPE=<path> makes an empty .tap
 *   ZX_SLOW                 no fast tape loading
 *   ZX_SNAP=<file>          snapshot (.sna, .z80), loaded after 100 frames
 *   ZX_JOY=<0|1|2>          joystick: Kempston, Sinclair, cursor
 *   ZX_SCRIPT=<file>        lines "<frame> <command>":
 *                             type <text>          type it (\n for ENTER)
 *                             key <n> 0|1          key number down / up
 *                             joy <bits>           joystick
 *                             shot <file.ppm>      screenshot
 *                             insert <d> <path>    disk into drive d (0 A:, 1 B:)
 *                             eject <d>
 *                             tape <path> | play | stop | rewind
 *                             snap <path>
 *                             peek <hex addr>      print the byte there
 *                             dump <file>          the 64 KB the CPU sees
 *                             reset
 *   ZX_STATUS=<n>           print PC, paging and the tape every n frames
 *   ZX_TRACE=<pc>[:<n>]     disassemble n instructions from when PC is reached
 *   ZX_TRACE_FRAME=<f>      ... from frame f on
 *   ZX_RING=<n>             ... after the n instructions before it
 *   ZX_WAV=<file>           record the sound (stereo WAV)
 *   ZX_QUIET                no summary
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zx.h"
#include "z80.h"

static uint16_t s_frame[ZX_SCREEN_WIDTH * ZX_SCREEN_HEIGHT];

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", ZX_SCREEN_WIDTH * 2, ZX_SCREEN_HEIGHT * 2);
    uint16_t line[ZX_SCREEN_WIDTH];
    uint8_t rgb[ZX_SCREEN_WIDTH * 2 * 3];
    for (int y = 0; y < ZX_SCREEN_HEIGHT; y++) {
        zx_render_line_rgb565(y, line);
        for (int x = 0; x < ZX_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            for (int k = 0; k < 2; k++) {
                rgb[(x * 2 + k) * 3] = ((c >> 11) & 31) * 255 / 31;
                rgb[(x * 2 + k) * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
                rgb[(x * 2 + k) * 3 + 2] = (c & 31) * 255 / 31;
            }
        }
        fwrite(rgb, 1, sizeof(rgb), f);
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

static uint8_t *read_file(const char *dir, const char *name, uint32_t *size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (size) {
        *size = n;
    }
    return buf;
}

/* ---- Tracing ---- */

static uint16_t s_trace_pc;
static int s_trace_left;
static uint32_t s_trace_frame, s_frame_now;
static bool s_tracing, s_trace_on;

static z80_t *s_ring;
static int s_ring_n, s_ring_pos;

static void print_state(const z80_t *c)
{
    char buf[64];
    z80_disassemble(c->pc.w, buf, sizeof(buf));
    printf("  %04X  %-20s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X SP=%04X t=%d\n", c->pc.w, buf,
           c->af.w, c->bc.w, c->de.w, c->hl.w, c->ix.w, c->sp.w, c->t);
}

void z80_host_hook(void)
{
    if (!s_trace_on) {
        return;
    }
    if (!s_tracing && z80.pc.w == s_trace_pc && s_trace_left > 0 && s_frame_now >= s_trace_frame) {
        s_tracing = true;
        for (int i = 0; s_ring && i < s_ring_n; i++) {
            const z80_t *c = &s_ring[(s_ring_pos + i) % s_ring_n];
            if (c->sp.w || c->pc.w) {
                print_state(c);
            }
        }
        printf("  ----\n");
    }
    if (s_ring && !s_tracing) {
        s_ring[s_ring_pos] = z80;
        s_ring_pos = (s_ring_pos + 1) % s_ring_n;
    }
    if (s_tracing) {
        char buf[64];
        z80_disassemble(z80.pc.w, buf, sizeof(buf));
        printf("  %04X  %-20s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X SP=%04X t=%d\n", z80.pc.w, buf,
               z80.af.w, z80.bc.w, z80.de.w, z80.hl.w, z80.ix.w, z80.sp.w, z80.t);
        if (--s_trace_left <= 0) {
            s_tracing = false;
        }
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[512];
} line_t;

static line_t *s_script;
static int s_lines, s_next;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "no script %s\n", path);
        exit(1);
    }
    s_script = calloc(4096, sizeof(line_t));
    char buf[600];
    while (s_lines < 4096 && fgets(buf, sizeof(buf), f)) {
        line_t *l = &s_script[s_lines];
        int at = 0;
        if (buf[0] == '#' || sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &at) < 2) {
            continue;
        }
        snprintf(l->arg, sizeof(l->arg), "%s", buf + at);
        l->arg[strcspn(l->arg, "\r\n")] = 0;
        s_lines++;
    }
    fclose(f);
}

/* "\n" in the text is ENTER */
static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s++;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void run_script(uint32_t frame)
{
    while (s_next < s_lines && s_script[s_next].frame <= frame) {
        line_t *l = &s_script[s_next++];
        if (strcmp(l->cmd, "type") == 0) {
            unescape(l->arg);
            zx_type(l->arg);
        } else if (strcmp(l->cmd, "key") == 0) {
            unsigned key = 0, down = 0;
            sscanf(l->arg, "%u %u", &key, &down);
            zx_key(key, down);
        } else if (strcmp(l->cmd, "joy") == 0) {
            zx_joystick(strtoul(l->arg, NULL, 0));
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg);
        } else if (strcmp(l->cmd, "insert") == 0) {
            int d = 0, at = 0;
            sscanf(l->arg, "%d %n", &d, &at);
            if (!zx_disk_insert(d, l->arg + at)) {
                printf("host: can't insert %s\n", l->arg + at);
            }
        } else if (strcmp(l->cmd, "eject") == 0) {
            zx_disk_eject(atoi(l->arg));
        } else if (strcmp(l->cmd, "tape") == 0) {
            if (strcmp(l->arg, "play") == 0) {
                zx_tape_play(true);
            } else if (strcmp(l->arg, "stop") == 0) {
                zx_tape_play(false);
            } else if (strcmp(l->arg, "rewind") == 0) {
                zx_tape_rewind();
            } else if (!zx_tape_insert(l->arg)) {
                printf("host: can't insert tape %s\n", l->arg);
            }
        } else if (strcmp(l->cmd, "snap") == 0) {
            if (!zx_snapshot_load(l->arg)) {
                printf("host: can't load %s\n", l->arg);
            }
        } else if (strcmp(l->cmd, "peek") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: $%04X = $%02X\n", frame, a, zx_peek(a));
        } else if (strcmp(l->cmd, "dump") == 0) {
            FILE *df = fopen(l->arg, "wb");
            for (int a = 0; df && a < 65536; a++) {
                fputc(zx_peek(a), df);
            }
            if (df) {
                fclose(df);
            }
        } else if (strcmp(l->cmd, "reset") == 0) {
            zx_reset();
        } else {
            printf("host: unknown script command %s\n", l->cmd);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <128|plus2|plus3> [frames] [out.ppm]\n", argv[0]);
        return 1;
    }
    static const char *const names[] = { "128", "plus2", "plus3" };
    static const char *const roms[] = { "128.rom", "plus2.rom", "plus3.rom" };
    int model = -1;
    for (int i = 0; i < ZX_MODEL_COUNT; i++) {
        if (strcmp(argv[1], names[i]) == 0) {
            model = i;
        }
    }
    if (model < 0) {
        fprintf(stderr, "unknown model %s\n", argv[1]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 250;
    const char *out = argc > 3 ? argv[3] : NULL;
    const char *dir = getenv("ZX_ROMS") ? getenv("ZX_ROMS") : "../../../roms";

    uint32_t rom_size = 0;
    zx_config_t cfg = {
        .model = model,
        .ram = malloc(ZX_RAM_SIZE),
        .rom = read_file(dir, roms[model], &rom_size),
        .trdos = model == ZX_MODEL_PLUS3 ? NULL : read_file(dir, "trdos.rom", NULL),
        .frame = s_frame,
    };
    if (cfg.rom == NULL || rom_size != (model == ZX_MODEL_PLUS3 ? 65536u : 32768u)) {
        fprintf(stderr, "no %s in %s\n", roms[model], dir);
        return 1;
    }
    FILE *wav = getenv("ZX_WAV") ? fopen(getenv("ZX_WAV"), "wb") : NULL;
    zx_audio_enable(wav != NULL);
    zx_tape_fast(!getenv("ZX_SLOW"));
    zx_joystick_type(getenv("ZX_JOY") ? atoi(getenv("ZX_JOY")) : ZX_JOYSTICK_KEMPSTON);
    /* Disks first: a TR-DOS disk in A: boots */
    if (getenv("ZX_NEWDISK")) {
        const char *p = getenv("ZX_NEWDISK");
        size_t n = strlen(p);
        bool dsk = n > 4 && strcmp(p + n - 4, ".dsk") == 0;
        if (!zx_disk_create(p, dsk ? ZX_PLUS3_BYTES : ZX_TRD_BYTES)) {
            fprintf(stderr, "can't make %s\n", p);
        }
    }
    for (int d = 0; d < ZX_DRIVES; d++) {
        const char *path = getenv(d ? "ZX_DISKB" : "ZX_DISKA");
        if (path && !zx_disk_insert(d, path)) {
            fprintf(stderr, "can't insert %s\n", path);
        }
    }
    if (!zx_init(&cfg)) {
        fprintf(stderr, "zx_init failed\n");
        return 1;
    }
    if (getenv("ZX_NEWTAPE") && !zx_tape_create(getenv("ZX_NEWTAPE"))) {
        fprintf(stderr, "can't make %s\n", getenv("ZX_NEWTAPE"));
    }
    const char *tape = getenv("ZX_TAPE") ? getenv("ZX_TAPE") : getenv("ZX_NEWTAPE");
    if (tape && !zx_tape_insert(tape)) {
        fprintf(stderr, "can't insert tape %s\n", tape);
    }
    if (getenv("ZX_SCRIPT")) {
        load_script(getenv("ZX_SCRIPT"));
    }
    const char *tr = getenv("ZX_TRACE");
    if (tr) {
        unsigned pc = 0;
        s_trace_left = 200;
        sscanf(tr, "%x:%d", &pc, &s_trace_left);
        s_trace_pc = pc;
        s_trace_on = true;
        s_trace_frame = getenv("ZX_TRACE_FRAME") ? atoi(getenv("ZX_TRACE_FRAME")) : 0;
        s_ring_n = getenv("ZX_RING") ? atoi(getenv("ZX_RING")) : 0;
        if (s_ring_n > 0) {
            s_ring = calloc(s_ring_n, sizeof(z80_t));
        }
    }
    int status_every = getenv("ZX_STATUS") ? atoi(getenv("ZX_STATUS")) : 0;
    uint32_t wav_frames = 0;
    if (wav) {
        fseek(wav, 44, SEEK_SET);
    }

    clock_t start = clock();
    for (int f = 0; f < frames; f++) {
        run_script(f);
        s_frame_now = f;
        if (f == 100 && getenv("ZX_SNAP") && !zx_snapshot_load(getenv("ZX_SNAP"))) {
            fprintf(stderr, "can't load %s\n", getenv("ZX_SNAP"));
        }
        zx_run_frame();
        if (wav) {
            int16_t buf[1024 * 2];
            int n;
            while ((n = zx_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, n, wav);
                wav_frames += n;
            }
        }
        if (status_every && (f + 1) % status_every == 0) {
            zx_status_t s;
            zx_get_status(&s);
            zx_tape_status_t ts;
            zx_tape_status(&ts);
            printf("frame %d: PC $%04X SP $%04X IFF %d IM %d 7FFD %02X 1FFD %02X%s tape %d/%d%s\n", f + 1, s.pc,
                   z80.sp.w, z80.iff1, z80.im, s.port_7ffd, s.port_1ffd, s.trdos ? " TR-DOS" : "", ts.block,
                   ts.blocks, ts.playing ? " playing" : "");
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (zx_disk_dirty()) {
        zx_disk_flush();
    }
    if (!getenv("ZX_QUIET")) {
        zx_status_t s;
        zx_get_status(&s);
        printf("%d frames in %.2f s (%.0f%% of real time), PC $%04X\n", frames, secs,
               frames / 50.02 / secs * 100, s.pc);
        zx_disk_status_t ds;
        zx_disk_status(&ds);
        for (int d = 0; d < ZX_DRIVES; d++) {
            if (zx_disk_path(d)) {
                printf("drive %c: %u sectors read, %u written\n", 'A' + d, ds.reads[d], ds.writes[d]);
            }
        }
        zx_tape_status_t ts;
        zx_tape_status(&ts);
        if (ts.inserted) {
            printf("tape: block %d of %d, %u loaded, %u saved at once\n", ts.block, ts.blocks, ts.loaded,
                   ts.saved);
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (wav) {
        uint32_t data = wav_frames * 4, rate = ZX_AUDIO_HZ, bytes = rate * 4;
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data...";
        uint32_t riff = 36 + data;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &bytes, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
