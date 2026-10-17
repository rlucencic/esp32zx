/*
 * The ULA's picture, its memory and I/O contention, and the floating bus.
 *
 * A frame is 311 lines of 228 T-states. The paper's first line starts at
 * the first contended T-state (14361 on the 128K and +2, 14365 on the +3),
 * each T-state is two pixels; the picture here has 48 pixels (24 T) of
 * border left and right and 54 lines above and below.
 *
 * Each paper line is drawn when the beam reaches it, from the memory as it
 * is then (raster effects that change the screen between lines work;
 * changes within a line show from the next one). Border changes are kept
 * with their times and drawn at the end of the frame, to the T-state.
 * Lines whose bytes didn't change aren't drawn again.
 */
#include <limits.h>
#include <string.h>
#include "zx_internal.h"

#define BORDER_X        48
#define BORDER_Y        54
#define LINE_T          228
#define PAPER_LINES     192
#define MAX_EVENTS      8192
#define TABLE_SIZE      (ZX_FRAME_T + 512)

int ula_next_t;

static int s_start;                     /* first contended T-state = paper line 0 */
static uint8_t *s_table;                /* contention by T-state */
static int s_next_line;                 /* next paper line to draw */
static uint32_t s_signature;

static uint8_t s_border;                /* now */
static uint8_t s_border_at_start;       /* at this frame's start */
static struct border_event {
    int32_t t;
    uint8_t colour;
} *s_ev;                                /* this frame's changes (PSRAM) */
static int s_ev_n;
static bool s_border_redraw;            /* last frame's border wasn't uniform, or it's new */
static uint8_t s_border_drawn;          /* colour of the last uniform border drawn */

/* Per paper line: its bitmap and attributes as drawn, and the flash phase */
static uint8_t (*s_cache)[65];

static const uint16_t s_palette[16] = {
#define RGB(r, g, b)    (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
    RGB(0, 0, 0), RGB(0, 0, 0xD7), RGB(0xD7, 0, 0), RGB(0xD7, 0, 0xD7),
    RGB(0, 0xD7, 0), RGB(0, 0xD7, 0xD7), RGB(0xD7, 0xD7, 0), RGB(0xD7, 0xD7, 0xD7),
    RGB(0, 0, 0), RGB(0, 0, 0xFF), RGB(0xFF, 0, 0), RGB(0xFF, 0, 0xFF),
    RGB(0, 0xFF, 0), RGB(0, 0xFF, 0xFF), RGB(0xFF, 0xFF, 0), RGB(0xFF, 0xFF, 0xFF),
#undef RGB
};

int ula_contention_start(void)
{
    return s_start;
}

bool ula_init(void)
{
    if (s_table == NULL) {
        s_table = zx_malloc(TABLE_SIZE);
        s_cache = zx_malloc(PAPER_LINES * sizeof(*s_cache));
        s_ev = zx_malloc(MAX_EVENTS * sizeof(*s_ev));
        if (s_table == NULL || s_cache == NULL || s_ev == NULL) {
            return false;
        }
    }
    /* 128K: 6,5,4,3,2,1,0,0 from the first T-state of each 8; +2A/+3:
     * 1,0,7,6,5,4,3,2
     */
    static const uint8_t pattern[2][8] = { { 6, 5, 4, 3, 2, 1, 0, 0 }, { 1, 0, 7, 6, 5, 4, 3, 2 } };
    s_start = zx.plus3 ? 14365 : 14361;
    memset(s_table, 0, TABLE_SIZE);
    for (int line = 0; line < PAPER_LINES; line++) {
        for (int c = 0; c < 128; c++) {
            s_table[s_start + line * LINE_T + c] = pattern[zx.plus3][c & 7];
        }
    }
    z80_contention = s_table;
    return true;
}

void ula_reset(void)
{
    s_border = 0;
    s_border_redraw = true;
    memset(s_cache, 0xFF, PAPER_LINES * sizeof(*s_cache));
    for (int i = 0; i < PAPER_LINES; i++) {
        s_cache[i][64] = 2;             /* no flash phase: draw it */
    }
}

/* ---- Paper ---- */

static void draw_paper_line(int line)
{
    const uint8_t *screen = zx.cfg.ram + ((zx.p7ffd & 0x08) ? 7 : 5) * 0x4000;
    const uint8_t *bitmap = screen + (((line & 0xC0) << 5) | ((line & 7) << 8) | ((line & 0x38) << 2));
    const uint8_t *attr = screen + 0x1800 + (line >> 3) * 32;
    uint8_t *cache = s_cache[line];
    uint8_t flash = (zx.frames >> 4) & 1;
    bool any_flash = false;
    for (int i = 0; i < 32; i++) {
        any_flash |= attr[i] & 0x80;
    }
    uint8_t phase = any_flash ? flash : 0;
    if (cache[64] == phase && memcmp(cache, bitmap, 32) == 0 && memcmp(cache + 32, attr, 32) == 0) {
        return;
    }
    memcpy(cache, bitmap, 32);
    memcpy(cache + 32, attr, 32);
    cache[64] = phase;
    uint16_t *dst = zx.cfg.frame + (BORDER_Y + line) * ZX_SCREEN_WIDTH + BORDER_X;
    for (int i = 0; i < 32; i++) {
        uint8_t a = attr[i], b = bitmap[i];
        if ((a & 0x80) && flash) {
            b = ~b;
        }
        uint8_t bright = (a >> 3) & 8;
        uint16_t ink = s_palette[(a & 7) | bright];
        uint16_t paper = s_palette[((a >> 3) & 7) | bright];
        for (int bit = 7; bit >= 0; bit--) {
            *dst++ = (b >> bit) & 1 ? ink : paper;
        }
    }
    s_signature++;
}

void ula_run(int t)
{
    while (s_next_line < PAPER_LINES && s_start + s_next_line * LINE_T <= t) {
        draw_paper_line(s_next_line++);
    }
    ula_next_t = s_next_line < PAPER_LINES ? s_start + s_next_line * LINE_T : INT_MAX;
}

/* ---- Border ---- */

void ula_border(uint8_t colour, int t)
{
    if (colour == s_border) {
        return;
    }
    s_border = colour;
    if (s_ev_n < MAX_EVENTS) {
        s_ev[s_ev_n].t = t;
        s_ev[s_ev_n].colour = colour;
        s_ev_n++;
    }
}

static void fill(uint16_t *dst, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        dst[i] = c;
    }
}

/* Every border pixel of the picture, from the frame's border changes */
static void draw_border(void)
{
    int e = 0;
    uint8_t colour = s_border_at_start;
    for (int y = 0; y < ZX_SCREEN_HEIGHT; y++) {
        uint16_t *row = zx.cfg.frame + y * ZX_SCREEN_WIDTH;
        int t0 = s_start - BORDER_X / 2 + (y - BORDER_Y) * LINE_T;
        bool paper = y >= BORDER_Y && y < BORDER_Y + PAPER_LINES;
        for (int part = 0; part < (paper ? 2 : 1); part++) {
            int x0 = paper && part ? BORDER_X + 256 : 0;
            int x1 = paper && !part ? BORDER_X : ZX_SCREEN_WIDTH;
            for (int x = x0; x < x1; ) {
                int t = t0 + x / 2;
                while (e < s_ev_n && s_ev[e].t <= t) {
                    colour = s_ev[e++].colour;
                }
                /* up to the next change, or the segment's end */
                int end = x1;
                if (e < s_ev_n && (s_ev[e].t - t0) * 2 < end) {
                    end = (s_ev[e].t - t0) * 2;
                }
                if (end <= x) {
                    end = x + 2;
                }
                fill(row + x, end - x, s_palette[colour]);
                x = end;
            }
        }
    }
}

void ula_frame_start(void)
{
    s_next_line = 0;
    ula_next_t = s_start;
    s_ev_n = 0;
    s_border_at_start = s_border;
}

void ula_frame_end(void)
{
    ula_run(INT_MAX);
    if (s_ev_n == 0) {
        if (!s_border_redraw && s_border_drawn == s_border) {
            return;
        }
        s_border_redraw = false;
        s_border_drawn = s_border;
    } else {
        s_border_redraw = true;         /* stripes now: draw the next frame too */
    }
    draw_border();
    s_signature++;
}

uint32_t zx_screen_signature(void)
{
    return s_signature;
}

void zx_render_line_rgb565(int y, uint16_t *dst)
{
    if (y >= 0 && y < ZX_SCREEN_HEIGHT) {
        memcpy(dst, zx.cfg.frame + y * ZX_SCREEN_WIDTH, ZX_SCREEN_WIDTH * sizeof(uint16_t));
    }
}

/* ---- Contention and the floating bus ---- */

static inline int wait_at(int t)
{
    return t >= 0 && t < TABLE_SIZE ? s_table[t] : 0;
}

/* 128K and +2 (the +3's I/O doesn't wait): the high byte as an address in
 * a contended bank, and the ULA's own ports (A0 = 0)
 */
int ula_io_contention(uint16_t port, int t)
{
    bool high = z80_contended[port >> 14];
    int start = t;
    if (high) {
        t += wait_at(t);
    }
    t++;
    if (!(port & 1)) {
        t += wait_at(t);
        t += 3;
    } else if (high) {
        for (int i = 0; i < 3; i++) {
            t += wait_at(t);
            t++;
        }
    } else {
        t += 3;
    }
    return t - start - 4;
}

/* The ULA reads two bitmap bytes and their attributes in the first 4 of
 * every 8 T-states of a paper line; elsewhere the bus floats at $FF
 */
uint8_t ula_floating_bus(int t)
{
    int off = t - s_start;
    if (off < 0 || off >= PAPER_LINES * LINE_T) {
        return 0xFF;
    }
    int line = off / LINE_T, col = off % LINE_T;
    if (col >= 128 || (col & 7) >= 4) {
        return 0xFF;
    }
    const uint8_t *screen = zx.cfg.ram + ((zx.p7ffd & 0x08) ? 7 : 5) * 0x4000;
    int x = (col >> 3) * 2 + ((col & 7) >> 1);
    if (col & 1) {
        return screen[0x1800 + (line >> 3) * 32 + x];
    }
    return screen[((line & 0xC0) << 5) | ((line & 7) << 8) | ((line & 0x38) << 2) | x];
}
