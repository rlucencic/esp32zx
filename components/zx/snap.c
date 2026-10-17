/*
 * Snapshots: .SNA (48K and 128K) and .Z80 (versions 1 to 3, compressed or
 * not). A 48K snapshot runs in 48K mode: 48 BASIC ROM, paging locked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zx_internal.h"

#define SNA_HEADER      27
#define SNA_48K         (SNA_HEADER + 49152)
#define SNA_128K        (SNA_48K + 4)

static inline uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint8_t *bank(int n)
{
    return zx.cfg.ram + n * 0x4000;
}

/* 48K RAM at $4000-$FFFF: banks 5, 2 and 0 */
static uint8_t *ram48(uint16_t addr)
{
    static const uint8_t banks[4] = { 0, 5, 2, 0 };
    return bank(banks[addr >> 14]) + (addr & 0x3FFF);
}

static void set_48k_mode(void)
{
    zx.p7ffd = 0x30;                    /* 48 BASIC ROM, bank 0 at $C000, locked */
    zx.p1ffd = zx.plus3 ? 0x04 : 0;     /* +3: ROM 3 */
}

/* 128K paging from a snapshot; false if this model can't do it */
static bool set_paging(uint8_t p7ffd, uint8_t p1ffd)
{
    zx.p7ffd = p7ffd;
    if (zx.plus3) {
        /* a 128K snapshot in 48 BASIC wants ROM 3 */
        zx.p1ffd = p1ffd ? p1ffd : (p7ffd & 0x10) ? 0x04 : 0;
    } else if (p1ffd & 0x05) {
        return false;                   /* +3 all-RAM or ROM 2/3: not here */
    }
    return true;
}

static void set_border(uint8_t colour)
{
    zx.ula_out = colour & 7;
    ula_border(colour & 7, z80.t);
}

static bool load_sna(const uint8_t *f, long size)
{
    const uint8_t *h = f;
    bool is128 = size >= SNA_128K;
    z80.i = h[0];
    z80.hl2 = le16(h + 1);
    z80.de2 = le16(h + 3);
    z80.bc2 = le16(h + 5);
    z80.af2 = le16(h + 7);
    z80.hl.w = le16(h + 9);
    z80.de.w = le16(h + 11);
    z80.bc.w = le16(h + 13);
    z80.iy.w = le16(h + 15);
    z80.ix.w = le16(h + 17);
    z80.iff1 = z80.iff2 = (h[19] >> 2) & 1;
    z80.r = h[20];
    z80.af.w = le16(h + 21);
    z80.sp.w = le16(h + 23);
    z80.im = h[25] & 3;
    set_border(h[26]);
    const uint8_t *ram = f + SNA_HEADER;
    if (!is128) {
        set_48k_mode();
        for (int i = 0; i < 3; i++) {
            memcpy(ram48(0x4000 + i * 0x4000), ram + i * 0x4000, 0x4000);
        }
        uint16_t sp = z80.sp.w;
        z80.pc.w = *ram48(sp) | (*ram48(sp + 1) << 8);    /* RETN */
        z80.sp.w = sp + 2;
        return true;
    }
    const uint8_t *x = f + SNA_48K;
    z80.pc.w = le16(x);
    uint8_t p7ffd = x[2];
    if (!set_paging(p7ffd, 0)) {
        return false;
    }
    int paged = p7ffd & 7;
    memcpy(bank(5), ram, 0x4000);
    memcpy(bank(2), ram + 0x4000, 0x4000);
    memcpy(bank(paged), ram + 0x8000, 0x4000);
    const uint8_t *rest = x + 4;
    for (int b = 0; b < 8; b++) {
        if (b == 5 || b == 2 || b == paged) {
            continue;
        }
        if (rest + 0x4000 > f + size) {
            return false;
        }
        memcpy(bank(b), rest, 0x4000);
        rest += 0x4000;
    }
    return true;
}

/* Z80 compression: ED ED n b = n times b. Into dst (len bytes); bytes used */
static long unpack(const uint8_t *src, long avail, uint8_t *dst, long len)
{
    long i = 0, o = 0;
    while (o < len && i < avail) {
        if (i + 3 < avail && src[i] == 0xED && src[i + 1] == 0xED) {
            int n = src[i + 2];
            uint8_t b = src[i + 3];
            for (int k = 0; k < n && o < len; k++) {
                dst[o++] = b;
            }
            i += 4;
        } else {
            dst[o++] = src[i++];
        }
    }
    return i;
}

static bool load_z80(const uint8_t *f, long size)
{
    if (size < 30) {
        return false;
    }
    const uint8_t *h = f;
    uint8_t flags = h[12] == 0xFF ? 1 : h[12];
    uint16_t pc = le16(h + 6);
    int extra = 0;
    bool is128 = false;
    uint8_t p7ffd = 0, p1ffd = 0;
    if (pc == 0) {
        extra = le16(h + 30);
        if (size < 32 + extra || (extra != 23 && extra != 54 && extra != 55)) {
            return false;
        }
        pc = le16(h + 32);
        uint8_t hw = h[34];
        if (extra == 23) {
            is128 = hw == 3 || hw == 4;
        } else {
            is128 = hw >= 4 && hw != 9 && hw != 10 && hw != 11 && hw <= 13;
            if (hw == 9 || hw == 10 || hw == 11 || hw > 13) {
                return false;           /* Pentagon, Scorpion, Didaktik ...: not these */
            }
        }
        p7ffd = h[35];
        if (extra == 55) {
            p1ffd = h[86];
        }
    }
    z80.af.b.h = h[0];
    z80.af.b.l = h[1];
    z80.bc.w = le16(h + 2);
    z80.hl.w = le16(h + 4);
    z80.sp.w = le16(h + 8);
    z80.i = h[10];
    z80.r = (h[11] & 0x7F) | ((flags & 1) << 7);
    set_border((flags >> 1) & 7);
    z80.de.w = le16(h + 13);
    z80.bc2 = le16(h + 15);
    z80.de2 = le16(h + 17);
    z80.hl2 = le16(h + 19);
    z80.af2 = (h[21] << 8) | h[22];
    z80.iy.w = le16(h + 23);
    z80.ix.w = le16(h + 25);
    z80.iff1 = h[27] ? 1 : 0;
    z80.iff2 = h[28] ? 1 : 0;
    z80.im = h[29] & 3;
    z80.pc.w = pc;

    if (extra == 0) {
        set_48k_mode();
        uint8_t *ram = zx_malloc(49152);
        bool ok = ram != NULL;
        if (ok && (flags & 0x20)) {
            memset(ram, 0, 49152);
            unpack(f + 30, size - 30, ram, 49152);
        } else if (ok && size >= 30 + 49152) {
            memcpy(ram, f + 30, 49152);
        } else {
            ok = false;
        }
        for (int i = 0; ok && i < 3; i++) {
            memcpy(ram48(0x4000 + i * 0x4000), ram + i * 0x4000, 0x4000);
        }
        free(ram);
        return ok;
    }
    if (is128) {
        if (!set_paging(p7ffd, p1ffd)) {
            return false;
        }
    } else {
        set_48k_mode();
    }
    /* AY registers (v2 and v3) */
    for (int r = 0; r < 16; r++) {
        ay_select(r);
        ay_write(h[39 + r]);
    }
    ay_select(h[38]);
    long pos = 32 + extra;
    while (pos + 3 <= size) {
        uint16_t len = le16(f + pos);
        uint8_t page = f[pos + 2];
        pos += 3;
        uint8_t *dst = NULL;
        if (is128) {
            if (page >= 3 && page <= 10) {
                dst = bank(page - 3);
            }
        } else {
            dst = page == 4 ? ram48(0x8000) : page == 5 ? ram48(0xC000) : page == 8 ? ram48(0x4000) : NULL;
        }
        long used = len == 0xFFFF ? 0x4000 : len;
        if (pos + used > size) {
            return false;
        }
        if (dst) {
            if (len == 0xFFFF) {
                memcpy(dst, f + pos, 0x4000);
            } else {
                unpack(f + pos, len, dst, 0x4000);
            }
        }
        pos += used;
    }
    return true;
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

bool zx_snapshot_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *f = size > 0 && size <= 256 * 1024 ? zx_malloc(size) : NULL;
    bool ok = f && fread(f, 1, size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) {
        free(f);
        return false;
    }
    /* Start from a reset machine, keep its RAM if the snapshot fails */
    uint8_t *saved = zx_malloc(ZX_RAM_SIZE);
    if (saved == NULL) {
        free(f);
        return false;
    }
    memcpy(saved, zx.cfg.ram, ZX_RAM_SIZE);
    z80_t cpu = z80;
    uint8_t p7 = zx.p7ffd, p1 = zx.p1ffd, out = zx.ula_out;
    bool trdos = zx.trdos_active;

    z80.halted = 0;
    z80.ei_delay = 0;
    zx.trdos_active = false;
    if (has_ext(path, ".sna")) {
        ok = (size == SNA_48K || size >= SNA_128K) && load_sna(f, size);
    } else {
        ok = load_z80(f, size);
    }
    free(f);
    if (!ok) {
        memcpy(zx.cfg.ram, saved, ZX_RAM_SIZE);
        z80 = cpu;
        zx.p7ffd = p7;
        zx.p1ffd = p1;
        zx.ula_out = out;
        zx.trdos_active = trdos;
    }
    free(saved);
    mem_update();
    if (ok) {
        ZX_LOG("snapshot %s, PC %04X", path, z80.pc.w);
    }
    return ok;
}
