/*
 * The machine: memory map and paging, I/O decoding, the interrupt, the
 * frame loop with the TR-DOS paging and the tape traps.
 *
 *   128K, +2: $7FFD (A15 = 0, A1 = 0): bits 0-2 RAM bank at $C000, 3 shadow
 *             screen (bank 7), 4 ROM (0 editor, 1 48 BASIC), 5 lock
 *   +3:       $7FFD (A15-14 = 01, A1 = 0) as above, $1FFD (A15-12 = 0001,
 *             A1 = 0): bit 0 all-RAM modes (bits 1-2 pick one), else bit 2
 *             is the ROM number's high bit; bit 3 the disk motor
 *   ULA:      any even port: keyboard and EAR in; border, MIC, beeper out
 *   AY:       $FFFD select / read, $BFFD write
 *   +3 FDC:   $2FFD status, $3FFD data
 *   Kempston: A7-A5 = 0 (port $1F)
 *   Beta 128: $1F $3F $5F $7F $FF while its ROM is paged in
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zx_internal.h"
#include "emudbg.h"
#include "upd765.h"

zx_t zx;

static uint8_t *s_rom_sink;     /* writes to ROM go here */


/* ---- Memory ---- */

static inline uint8_t *bank(int n)
{
    return zx.cfg.ram + n * 0x4000;
}

static int rom_number(void)
{
    if (zx.plus3) {
        return ((zx.p7ffd >> 4) & 1) | ((zx.p1ffd >> 1) & 2);
    }
    return (zx.p7ffd >> 4) & 1;
}

bool zx_basic_rom(void)
{
    if (zx.trdos_active || (zx.plus3 && (zx.p1ffd & 1))) {
        return false;
    }
    return rom_number() == (zx.plus3 ? 3 : 1);
}

void mem_update(void)
{
    static const uint8_t special[4][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 4, 5, 6, 3 }, { 4, 7, 6, 3 },
    };
    int banks[4] = { -1, 5, 2, zx.p7ffd & 7 };
    if (zx.plus3 && (zx.p1ffd & 1)) {
        for (int i = 0; i < 4; i++) {
            banks[i] = special[(zx.p1ffd >> 1) & 3][i];
        }
    }
    for (int i = 0; i < 4; i++) {
        if (banks[i] < 0) {
            const uint8_t *rom = zx.trdos_active ? zx.cfg.trdos : zx.cfg.rom + rom_number() * ZX_ROM_SIZE;
            z80_rd[i] = (uint8_t *)rom;
            z80_wr[i] = s_rom_sink;
            z80_contended[i] = 0;
        } else {
            z80_rd[i] = z80_wr[i] = bank(banks[i]);
            /* 128K: the odd banks wait for the ULA, +3: banks 4-7 */
            z80_contended[i] = zx.plus3 ? banks[i] >= 4 : banks[i] & 1;
        }
    }
}

uint8_t zx_peek(uint16_t address)
{
    return z80_peek(address);
}

/* ---- I/O ---- */

static uint8_t ZX_HOT io_in(uint16_t port, int at)
{
    int t = z80.t + at - 1;             /* the I/O cycle's start */
    if (!zx.plus3) {
        int w = ula_io_contention(port, t);
        z80.wait += w;
        t += w;
    }
    if (!(port & 1)) {
        /* the tape, or without one what the 128's EAR output puts on the
         * pin (the +3 reads 0)
         */
        bool tape = tape_ear();
        bool ear = tape_inserted() ? tape : !zx.plus3 && (zx.ula_out & 0x10);
        return input_rows(port >> 8) | 0xA0 | (ear ? 0x40 : 0);
    }
    uint8_t lo = port & 0xFF;
    if (zx.trdos_active && beta_port(port)) {
        return beta_in(port);
    }
    if ((lo & 0xE0) == 0 && input_kempston_present()) {
        return input_kempston();
    }
    if ((port & 0xC002) == 0xC000) {
        return ay_read();
    }
    if (zx.plus3) {
        if ((port & 0xF002) == 0x2000) {
            return upd765_status();
        }
        if ((port & 0xF002) == 0x3000) {
            return upd765_read();
        }
        return 0xFF;
    }
    return ula_floating_bus(t + 2);
}

static void ZX_HOT io_out(uint16_t port, uint8_t v, int at)
{
    int t = z80.t + at - 1;
    if (!zx.plus3) {
        int w = ula_io_contention(port, t);
        z80.wait += w;
        t += w;
    }
    if (!(port & 1)) {
        ula_border(v & 7, t + 1);
        uint8_t changed = zx.ula_out ^ v;
        zx.ula_out = v;
        if (changed & 0x18) {
            beeper(((v >> 3) & 2) | ((v >> 3) & 1), t + 1);
        }
    }
    if (zx.trdos_active && beta_port(port)) {
        beta_out(port, v);
        return;
    }
    bool is_7ffd = zx.plus3 ? (port & 0xC002) == 0x4000 : (port & 0x8002) == 0;
    if (is_7ffd && !(zx.p7ffd & 0x20)) {
        if ((zx.p7ffd ^ v) & 0x08) {
            ula_run(t);                 /* the screen switches from here */
        }
        zx.p7ffd = v;
        mem_update();
    }
    if ((port & 0xC002) == 0xC000) {
        ay_select(v);
    } else if ((port & 0xC002) == 0x8000) {
        ay_write(v);
    }
    if (zx.plus3) {
        if ((port & 0xF002) == 0x1000 && !(zx.p7ffd & 0x20)) {
            zx.p1ffd = v;
            mem_update();
            upd765_motor(v & 0x08);
        } else if ((port & 0xF002) == 0x3000) {
            upd765_write(v);
        }
    }
}

static uint8_t int_ack(void)
{
    return 0xFF;                        /* nothing drives the bus */
}

/* ---- Running ---- */

void zx_reset(void)
{
    z80_reset();
    z80.t = 0;
    zx.p7ffd = zx.p1ffd = 0;
    zx.ula_out = 0;
    /* A Beta 128 with a disk in A: boots TR-DOS, which wants the 48 BASIC
     * ROM behind it
     */
    zx.trdos_active = !zx.plus3 && zx.cfg.trdos && beta_has_disk(0);
    if (zx.trdos_active) {
        zx.p7ffd = 0x10;
    }
    mem_update();
    ula_reset();
    ay_reset();
    upd765_reset();
    upd765_motor(false);
    beta_reset();
    input_reset();
    tape_reset();
}

/* ---- The debugger (emudbg.h) ---- */

static emudbg_cpu_t s_dbg_cpus[] = {
    { .name = "Z80", .arch = &emudbg_arch_z80 },
};

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu, T-state %ld", (unsigned long)zx.frames, (long)z80.t);
}

static void dbg_screen(int *width, int *height)
{
    *width = ZX_SCREEN_WIDTH;
    *height = ZX_SCREEN_HEIGHT;
}

static emudbg_machine_t s_dbg = {
    .cpus = s_dbg_cpus,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = zx_reset,
    .screen = dbg_screen,
    .render_line = zx_render_line_rgb565,
    .type = zx_type,
};

bool zx_init(const zx_config_t *config)
{
    if (config->ram == NULL || config->rom == NULL || config->frame == NULL ||
        config->model >= ZX_MODEL_COUNT) {
        return false;
    }
    if (s_rom_sink == NULL) {
        s_rom_sink = zx_malloc(ZX_ROM_SIZE);
        if (s_rom_sink == NULL) {
            return false;
        }
    }
    uint64_t clock = zx.clock;
    memset(&zx, 0, sizeof(zx));
    zx.cfg = *config;
    zx.clock = clock;
    zx.plus3 = config->model == ZX_MODEL_PLUS3;
    if (zx.plus3) {
        zx.cfg.trdos = NULL;
    }
    zx.int_len = zx.plus3 ? 32 : 36;
    if (!ula_init()) {
        return false;
    }
    static const z80_bus_t bus = { .in = io_in, .out = io_out, .int_ack = int_ack };
    z80_set_bus(&bus);
    upd765_set_creator("esp32zx");
    memset(config->ram, 0, ZX_RAM_SIZE);
    audio_init();
    zx_reset();
    s_dbg.name = zx.plus3 ? "ZX Spectrum +3" : "ZX Spectrum 128K";
    emudbg_set_machine(&s_dbg);
    return true;
}

/* The ROM's tape routines, if this is the 48K BASIC ROM */
static inline void tape_hooks(uint16_t pc)
{
    if (!zx_basic_rom()) {
        return;
    }
    if (pc == 0x0556) {
        tape_rom_loader();
    } else if (pc == 0x056B) {
        tape_trap_load();
    } else if (pc == 0x04C2) {
        tape_trap_save();
    }
}

void ZX_HOT zx_run_frame(void)
{
    if (zx.reset_pending) {
        zx.reset_pending = false;
        zx_reset();
    }
    input_frame();
    ula_frame_start();
    bool beta = zx.cfg.trdos != NULL;
    while (z80.t < ZX_FRAME_T) {
        z80.int_line = z80.t < zx.int_len;
        uint16_t pc = z80.pc.w;
        if (beta) {
            if (zx.trdos_active) {
                if (pc >= 0x4000) {
                    zx.trdos_active = false;
                    mem_update();
                }
            } else if ((pc & 0xFF00) == 0x3D00 && zx_basic_rom()) {
                zx.trdos_active = true;
                mem_update();
            }
        }
        if (pc < 0x0600 && (pc == 0x0556 || pc == 0x056B || pc == 0x04C2)) {
            tape_hooks(pc);
        }
        if (z80.halted && !z80.int_line) {
            /* nothing happens until the next interrupt: skip to it */
            int n = (ZX_FRAME_T - z80.t + 3) / 4;
            z80.r = (z80.r & 0x80) | ((z80.r + n) & 0x7F);
            z80.t += n * 4;
        } else {
            z80.t += z80_step();
        }
        if (z80.t >= ula_next_t) {
            ula_run(z80.t);
        }
    }
    ula_frame_end();
    audio_frame_end();
    tape_frame_end();
    upd765_frame();
    beta_frame();
    z80.t -= ZX_FRAME_T;
    zx.clock += ZX_FRAME_T;
    zx.frames++;
}

void zx_get_status(zx_status_t *s)
{
    s->frames = zx.frames;
    s->pc = z80.pc.w;
    s->port_7ffd = zx.p7ffd;
    s->port_1ffd = zx.p1ffd;
    s->border = zx.ula_out & 7;
    s->trdos = zx.trdos_active;
}

zx_model_t zx_model(void)
{
    return zx.cfg.model;
}
