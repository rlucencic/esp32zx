/*
 * Beta 128 disk interface (128K and +2): a WD1793 and the TR-DOS ROM, and
 * the disk calls for both kinds of drive (the +3's uPD765 is esp32emu's).
 *
 *   $1F command / status   $3F track   $5F sector   $7F data
 *   $FF system: write bits 0-1 drive, 2 reset (0), 4 side (0 = side 1),
 *       read bit 6 DRQ, bit 7 INTRQ
 *
 * The ports exist only while the TR-DOS ROM is paged in (zx.c pages it
 * when the CPU runs at $3D00-$3DFF with the 48K BASIC ROM, and out above
 * $3FFF).
 *
 * Images: .TRD (the sectors in order: 256 bytes, 16 to a track, the sides
 * of a cylinder one after the other) and .SCL (files, turned into a TRD in
 * memory, read-only). Data is ready at once; the index pulse comes five
 * times a second. Writes are kept by sector and go back with
 * zx_disk_flush().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zx_internal.h"
#include "upd765.h"

#define CYLS            80
#define SIDES           2
#define SECTORS         16
#define SECTOR_SIZE     256
#define TRACK_SIZE      (SECTORS * SECTOR_SIZE)
#define IMAGE_SIZE      (CYLS * SIDES * TRACK_SIZE)
#define ALL_SECTORS     (CYLS * SIDES * SECTORS)
#define ROTATION_T      (ZX_CLOCK_HZ / 5)
#define INDEX_T         (ZX_CLOCK_HZ / 250)     /* 4 ms */
#define RAW_TRACK       6400

typedef struct {
    char path[256];
    bool wp;
    uint8_t sides;
    uint32_t file_size;
    uint8_t *data;                      /* IMAGE_SIZE: [cyl][side][sector] */
    uint8_t dirty[ALL_SECTORS / 8];
    bool any_dirty;
} trd_t;

static trd_t *s_disk[ZX_DRIVES];

static struct {
    uint8_t status, track, sector, data, cmd;
    uint8_t system;
    int drive, side;
    uint8_t cyl[ZX_DRIVES];
    bool intrq, drq, busy;
    int type;                           /* of the last command: 1-4 */
    /* Transfers */
    uint8_t *buf;
    int len, pos;
    bool write, multi;
    uint8_t id[6];
    uint8_t *raw;                       /* write track */
    uint64_t index_deadline;            /* write track: the first byte before this */
    int id_index;
    zx_disk_status_t stat;
    int light_frames;
} w;

/* ---- Images ---- */

static inline uint8_t *sector_ptr(trd_t *d, int cyl, int side, int sec)
{
    return d->data + ((cyl * SIDES + side) * SECTORS + sec) * SECTOR_SIZE;
}

/* TRD file order: cylinders, each with its sides (one side: only side 0) */
static uint32_t file_offset(const trd_t *d, int cyl, int side, int sec)
{
    return ((cyl * d->sides + side) * SECTORS + sec) * SECTOR_SIZE;
}

static void free_disk(trd_t *d)
{
    if (d) {
        free(d->data);
        free(d);
    }
}

static uint8_t sides_of(const uint8_t *system)
{
    uint8_t type = system[0xE3];
    return type == 0x18 || type == 0x19 ? 1 : 2;
}

static void make_system_sector(uint8_t *sys, int files, int free_sector, int free_track, int free_count,
                               const char *label)
{
    memset(sys, 0, SECTOR_SIZE);
    sys[0xE1] = free_sector;
    sys[0xE2] = free_track;
    sys[0xE3] = 0x16;                   /* 80 tracks, two sides */
    sys[0xE4] = files;
    sys[0xE5] = free_count;
    sys[0xE6] = free_count >> 8;
    sys[0xE7] = 0x10;                   /* TR-DOS */
    memset(sys + 0xEA, ' ', 9);
    memset(sys + 0xF5, ' ', 8);
    memcpy(sys + 0xF5, label, strlen(label) < 8 ? strlen(label) : 8);
}

/* An SCL archive into a TRD image */
static bool scl_to_trd(const uint8_t *f, uint32_t size, uint8_t *img)
{
    if (size < 9 || memcmp(f, "SINCLAIR", 8) != 0) {
        return false;
    }
    int files = f[8];
    if (files > 128 || 9 + files * 14 > (int)size) {
        return false;
    }
    const uint8_t *data = f + 9 + files * 14;
    int pos = SECTORS;                  /* logical sector: track 1, sector 0 */
    for (int i = 0; i < files; i++) {
        const uint8_t *h = f + 9 + i * 14;
        int n = h[13];
        if (data + n * SECTOR_SIZE > f + size || pos + n > ALL_SECTORS) {
            return false;
        }
        uint8_t *e = img + i * 16;      /* directory: track 0, sectors 0-7 */
        memcpy(e, h, 14);
        e[14] = pos % SECTORS;
        e[15] = pos / SECTORS;
        memcpy(img + pos * SECTOR_SIZE, data, n * SECTOR_SIZE);
        data += n * SECTOR_SIZE;
        pos += n;
    }
    make_system_sector(img + 8 * SECTOR_SIZE, files, pos % SECTORS, pos / SECTORS, ALL_SECTORS - pos,
                       "SCL");
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

static trd_t *load_image(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    bool scl = has_ext(path, ".scl");
    trd_t *d = zx_calloc(1, sizeof(trd_t));
    uint8_t *img = d ? zx_calloc(1, IMAGE_SIZE) : NULL;
    uint8_t *file = (scl && size > 0 && size <= 2 * IMAGE_SIZE) ? zx_malloc(size) : NULL;
    bool ok = img != NULL && size > 0 && (!scl || file != NULL);
    if (ok && scl) {
        ok = fread(file, 1, size, fp) == (size_t)size && scl_to_trd(file, size, img);
    } else if (ok) {
        long n = size < IMAGE_SIZE ? size : IMAGE_SIZE;
        ok = n >= 9 * SECTOR_SIZE && fread(img, 1, n, fp) == (size_t)n;
    }
    fclose(fp);
    free(file);
    if (!ok) {
        free(img);
        free(d);
        return NULL;
    }
    d->sides = sides_of(img + 8 * SECTOR_SIZE);
    if (d->sides == 1) {
        /* the file has the cylinders of side 0 only: spread them out */
        for (int c = CYLS - 1; c >= 0; c--) {
            memmove(img + (c * SIDES) * TRACK_SIZE, img + c * TRACK_SIZE, TRACK_SIZE);
            memset(img + (c * SIDES + 1) * TRACK_SIZE, 0, TRACK_SIZE);
        }
    }
    d->data = img;
    d->file_size = scl ? 0 : size;
    d->wp = scl;
    snprintf(d->path, sizeof(d->path), "%s", path);
    if (!scl) {
        FILE *rw = fopen(path, "r+b");
        d->wp = rw == NULL;
        if (rw) {
            fclose(rw);
        }
    }
    return d;
}

static bool flush_disk(trd_t *d)
{
    if (!d->any_dirty || d->wp) {
        return true;
    }
    FILE *fp = fopen(d->path, "r+b");
    if (fp == NULL) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < ALL_SECTORS && ok; i++) {
        if (!(d->dirty[i / 8] & (1u << (i & 7)))) {
            continue;
        }
        int sec = i % SECTORS, side = (i / SECTORS) % SIDES, cyl = i / (SECTORS * SIDES);
        if (side >= d->sides) {
            continue;
        }
        ok = fseek(fp, file_offset(d, cyl, side, sec), SEEK_SET) == 0 &&
             fwrite(sector_ptr(d, cyl, side, sec), 1, SECTOR_SIZE, fp) == SECTOR_SIZE;
    }
    fclose(fp);
    if (ok) {
        memset(d->dirty, 0, sizeof(d->dirty));
        d->any_dirty = false;
    }
    return ok;
}

static void mark_dirty(trd_t *d, int cyl, int side, int sec)
{
    int i = (cyl * SIDES + side) * SECTORS + sec;
    d->dirty[i / 8] |= 1u << (i & 7);
    d->any_dirty = true;
}

bool beta_has_disk(int drive)
{
    return drive >= 0 && drive < ZX_DRIVES && s_disk[drive] != NULL;
}

/* ---- The disk calls, for both kinds of drive ---- */

static bool is_dsk(const char *path)
{
    return has_ext(path, ".dsk");
}

bool zx_disk_fits(zx_model_t model, const char *name)
{
    if (model == ZX_MODEL_PLUS3) {
        return is_dsk(name);
    }
    return has_ext(name, ".trd") || has_ext(name, ".scl");
}

static void beta_eject(int drive)
{
    if (s_disk[drive]) {
        if (!flush_disk(s_disk[drive])) {
            ZX_LOG("can't write %s", s_disk[drive]->path);
        }
        free_disk(s_disk[drive]);
        s_disk[drive] = NULL;
    }
}

bool zx_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= ZX_DRIVES) {
        return false;
    }
    if (is_dsk(path)) {
        if (!upd765_disk_insert(drive, path)) {
            return false;
        }
        beta_eject(drive);
        return true;
    }
    trd_t *d = load_image(path);
    if (d == NULL) {
        return false;
    }
    beta_eject(drive);
    upd765_disk_eject(drive);
    s_disk[drive] = d;
    return true;
}

void zx_disk_eject(int drive)
{
    if (drive >= 0 && drive < ZX_DRIVES) {
        beta_eject(drive);
        upd765_disk_eject(drive);
    }
}

const char *zx_disk_path(int drive)
{
    if (drive < 0 || drive >= ZX_DRIVES) {
        return NULL;
    }
    return s_disk[drive] ? s_disk[drive]->path : upd765_disk_path(drive);
}

bool zx_disk_write_protected(int drive)
{
    if (drive < 0 || drive >= ZX_DRIVES) {
        return false;
    }
    return s_disk[drive] ? s_disk[drive]->wp : upd765_disk_write_protected(drive);
}

bool zx_disk_dirty(void)
{
    for (int d = 0; d < ZX_DRIVES; d++) {
        if (s_disk[d] && s_disk[d]->any_dirty && !s_disk[d]->wp) {
            return true;
        }
    }
    return upd765_disk_dirty();
}

void zx_disk_flush(void)
{
    for (int d = 0; d < ZX_DRIVES; d++) {
        if (s_disk[d] && !flush_disk(s_disk[d])) {
            ZX_LOG("can't write %s", s_disk[d]->path);
        }
    }
    upd765_disk_flush();
}

bool zx_disk_create(const char *path, uint32_t bytes)
{
    if (bytes == ZX_PLUS3_BYTES) {
        /* +3DOS disk specification in the first sector: type 0, one side,
         * 40 tracks, 9 sectors of 512, 1 reserved track, 1 KB blocks, 2
         * directory blocks, gaps
         */
        uint8_t spec[512];
        memset(spec, 0xE5, sizeof(spec));
        static const uint8_t head[10] = { 0x00, 0x00, 0x28, 0x09, 0x02, 0x01, 0x03, 0x02, 0x2A, 0x52 };
        memcpy(spec, head, sizeof(head));
        memset(spec + 10, 0, 6);
        return upd765_disk_create(path, 40, 9, 1, spec);
    }
    if (bytes != ZX_TRD_BYTES) {
        return false;
    }
    uint8_t *img = zx_calloc(1, IMAGE_SIZE);
    if (img == NULL) {
        return false;
    }
    make_system_sector(img + 8 * SECTOR_SIZE, 0, 0, 1, ALL_SECTORS - SECTORS, "BLANK");
    FILE *fp = fopen(path, "wb");
    bool ok = fp && fwrite(img, 1, IMAGE_SIZE, fp) == IMAGE_SIZE;
    if (fp) {
        fclose(fp);
    }
    free(img);
    return ok;
}

void zx_disk_status(zx_disk_status_t *s)
{
    if (zx.plus3) {
        upd765_status_t u;
        upd765_disk_status(&u);
        s->light = u.light;
        for (int d = 0; d < ZX_DRIVES; d++) {
            s->reads[d] = u.reads[d];
            s->writes[d] = u.writes[d];
            s->track[d] = u.track[d];
        }
        return;
    }
    *s = w.stat;
    for (int d = 0; d < ZX_DRIVES; d++) {
        s->track[d] = w.cyl[d];
    }
}

/* ---- The WD1793 ---- */

enum {
    ST_BUSY = 0x01, ST_INDEX = 0x02, ST_DRQ = 0x02, ST_TRACK0 = 0x04, ST_LOST = 0x04,
    ST_CRC = 0x08, ST_SEEK = 0x10, ST_RNF = 0x10, ST_HEAD = 0x20, ST_WP = 0x40, ST_NOT_READY = 0x80,
};

static trd_t *disk(void)
{
    return s_disk[w.drive];
}

static void light(void)
{
    w.stat.light = w.drive;
    w.light_frames = 15;
}

static void done(uint8_t status)
{
    w.status = status;
    w.busy = false;
    w.drq = false;
    w.intrq = true;
}

/* Type I status: head position, index pulse, write protect */
static uint8_t type1_status(void)
{
    uint8_t s = ST_HEAD;
    trd_t *d = disk();
    if (d == NULL) {
        return s | ST_NOT_READY;
    }
    if (w.cyl[w.drive] == 0) {
        s |= ST_TRACK0;
    }
    if (d->wp) {
        s |= ST_WP;
    }
    if (zx_now() % ROTATION_T < INDEX_T) {
        s |= ST_INDEX;
    }
    return s;
}

static void seek_to(int cyl)
{
    if (cyl < 0) {
        cyl = 0;
    }
    if (cyl >= CYLS) {
        cyl = CYLS - 1;
    }
    w.cyl[w.drive] = cyl;
}

static void type1(uint8_t cmd)
{
    int step = (w.cmd & 0x20) ? -1 : 1;     /* remembered for STEP */
    switch (cmd >> 4) {
    case 0x0:                               /* RESTORE */
        seek_to(0);
        w.track = 0;
        break;
    case 0x1:                               /* SEEK to the data register */
        seek_to(w.cyl[w.drive] + (w.data - w.track));
        w.track = w.data;
        break;
    default: {
        int dir;
        if ((cmd >> 5) == 1) {
            dir = step;                     /* STEP: as the last one */
        } else {
            dir = (cmd >> 5) == 2 ? 1 : -1; /* STEP IN, STEP OUT */
        }
        seek_to(w.cyl[w.drive] + dir);
        if (cmd & 0x10) {
            w.track += dir;
        }
        w.cmd = (w.cmd & ~0x20) | (dir < 0 ? 0x20 : 0);
        break;
    }
    }
    uint8_t s = type1_status();
    if ((cmd & 0x04) && disk() && w.track != w.cyl[w.drive]) {
        s |= ST_SEEK;                       /* verify: the track register doesn't match */
    }
    done(s);
}

static uint8_t *find_sector(void)
{
    trd_t *d = disk();
    if (d == NULL || w.side >= d->sides || w.track != w.cyl[w.drive] || w.sector < 1 ||
        w.sector > SECTORS) {
        return NULL;
    }
    return sector_ptr(d, w.cyl[w.drive], w.side, w.sector - 1);
}

static void start_sector(void)
{
    uint8_t *p = find_sector();
    if (p == NULL) {
        done(disk() ? ST_RNF : ST_NOT_READY);
        return;
    }
    if (w.write && disk()->wp) {
        done(ST_WP);
        return;
    }
    w.buf = p;
    w.len = SECTOR_SIZE;
    w.pos = 0;
    w.busy = true;
    w.drq = true;
    w.intrq = false;
    w.status = ST_BUSY | ST_DRQ;
    light();
    if (w.write) {
        w.stat.writes[w.drive]++;
    } else {
        w.stat.reads[w.drive]++;
    }
}

/* A sector's bytes are through */
static void sector_done(void)
{
    if (w.write) {
        mark_dirty(disk(), w.cyl[w.drive], w.side, w.sector - 1);
    }
    if (w.multi && w.sector < SECTORS) {
        w.sector++;
        start_sector();
        return;
    }
    done(0);
}

/* FORMAT: the raw track written; its sectors from the ID and data marks */
static void write_track_done(void)
{
    trd_t *d = disk();
    for (int i = 0; i + 8 < w.len; i++) {
        if (w.raw[i] != 0xFE) {
            continue;
        }
        int r = w.raw[i + 3], n = w.raw[i + 4];
        int j = i + 7;
        while (j < w.len && w.raw[j] != 0xFB && w.raw[j] != 0xF8 && w.raw[j] != 0xFE) {
            j++;
        }
        if (j >= w.len || w.raw[j] == 0xFE || r < 1 || r > SECTORS || n != 1 || w.side >= SIDES) {
            continue;
        }
        int k = j + 1, m = w.len - k < SECTOR_SIZE ? w.len - k : SECTOR_SIZE;
        uint8_t *p = sector_ptr(d, w.cyl[w.drive], w.side, r - 1);
        memcpy(p, w.raw + k, m);
        mark_dirty(d, w.cyl[w.drive], w.side, r - 1);
        i = k + m - 1;
    }
    if (w.side == 1 && d->sides == 1) {
        d->sides = 2;                   /* formatted double-sided now */
    }
    done(0);
}

static void command(uint8_t cmd)
{
    w.cmd = cmd;
#ifndef ESP_PLATFORM
    static int log = -1;
    if (log < 0) {
        log = getenv("ZX_BETALOG") != NULL;
    }
    if (log) {
        printf("wd1793: cmd %02X PC %04X drive %d side %d cyl %d track %d sector %d data %02X (frame %u)\n", cmd, z80.pc.w,
               w.drive, w.side, w.cyl[w.drive], w.track, w.sector, w.data, zx.frames);
    }
#endif
    if ((cmd & 0xF0) == 0xD0) {         /* FORCE INTERRUPT */
        w.busy = false;
        w.drq = false;
        w.intrq = (cmd & 0x0F) != 0;
        w.type = 4;
        w.status = type1_status();
        return;
    }
    if (w.busy) {
        return;
    }
    w.intrq = false;
    w.drq = false;
    if (!(cmd & 0x80)) {
        w.type = 1;
        if (disk()) {
            light();
        }
        type1(cmd);
        return;
    }
    if (disk() == NULL) {
        w.type = 2;
        done(ST_NOT_READY);
        return;
    }
    switch (cmd >> 5) {
    case 4: case 5:                     /* READ SECTOR, WRITE SECTOR */
        w.type = 2;
        w.write = cmd & 0x20;
        w.multi = cmd & 0x10;
        start_sector();
        break;
    case 6:                             /* READ ADDRESS */
        w.type = 3;
        w.id[0] = w.cyl[w.drive];
        w.id[1] = w.side;
        w.id[2] = 1 + w.id_index++ % SECTORS;
        w.id[3] = 1;                    /* 256 bytes */
        w.id[4] = w.id[5] = 0;
        w.sector = w.id[0];
        w.buf = w.id;
        w.len = 6;
        w.pos = 0;
        w.write = false;
        w.multi = false;
        w.busy = w.drq = true;
        w.status = ST_BUSY | ST_DRQ;
        break;
    case 7:
        w.type = 3;
        if (cmd & 0x10) {               /* WRITE TRACK */
            if (disk()->wp) {
                done(ST_WP);
                break;
            }
            if (w.raw == NULL) {
                w.raw = zx_malloc(RAW_TRACK);
                if (w.raw == NULL) {
                    done(ST_LOST);
                    break;
                }
            }
            w.buf = w.raw;
            w.len = RAW_TRACK;
            w.write = true;
            /* it starts at the next index pulse: no data by then is lost data */
            w.index_deadline = (zx_now() / ROTATION_T + 1) * ROTATION_T;
        } else {                        /* READ TRACK: the sectors' data */
            w.buf = sector_ptr(disk(), w.cyl[w.drive], w.side < SIDES ? w.side : 0, 0);
            w.len = TRACK_SIZE;
            w.write = false;
        }
        w.pos = 0;
        w.multi = false;
        w.busy = w.drq = true;
        w.status = ST_BUSY | ST_DRQ;
        light();
        break;
    }
}

bool beta_port(uint16_t port)
{
    uint8_t lo = port & 0xFF;
    return lo == 0x1F || lo == 0x3F || lo == 0x5F || lo == 0x7F || lo == 0xFF;
}

/* WRITE TRACK with no byte written by the index pulse ends at once, writing
 * nothing (TR-DOS sends a stray one: its seek helper ORs in whatever is
 * in B)
 */
static void check_lost_data(void)
{
    if (w.busy && w.type == 3 && w.write && w.pos == 0 && zx_now() >= w.index_deadline) {
        done(ST_LOST);
    }
}

uint8_t beta_in(uint16_t port)
{
    check_lost_data();
#ifndef ESP_PLATFORM
    static int log = -1, reads;
    static uint8_t last_cmd;
    if (log < 0) {
        log = getenv("ZX_BETALOG") != NULL;
    }
    if (last_cmd != w.cmd) {
        last_cmd = w.cmd;
        reads = 0;
    }
    if (log && reads++ < 12) {
        printf("wd1793: in %02X: status %02X intrq %d drq %d busy %d PC %04X\n", port & 0xFF, w.status, w.intrq,
               w.drq, w.busy, z80.pc.w);
    }
#endif
    switch (port & 0xFF) {
    case 0x1F:
        w.intrq = false;
        if (w.type == 1 || w.type == 4) {
            w.status = (w.status & ~(ST_INDEX | ST_TRACK0 | ST_NOT_READY | ST_WP)) |
                       (type1_status() & (ST_INDEX | ST_TRACK0 | ST_NOT_READY | ST_WP));
        }
        return w.status;
    case 0x3F:
        return w.track;
    case 0x5F:
        return w.sector;
    case 0x7F:
        if (w.drq && !w.write) {
            w.data = w.buf[w.pos++];
            if (w.pos >= w.len) {
                if (w.type == 2) {
                    sector_done();
                } else {
                    done(0);
                }
            }
        }
        return w.data;
    default:                            /* $FF */
        return (w.intrq ? 0x80 : 0) | (w.drq ? 0x40 : 0) | 0x3F;
    }
}

void beta_out(uint16_t port, uint8_t v)
{
    switch (port & 0xFF) {
    case 0x1F:
        command(v);
        break;
    case 0x3F:
        w.track = v;
        break;
    case 0x5F:
        w.sector = v;
        break;
    case 0x7F:
        w.data = v;
        if (w.drq && w.write) {
            w.buf[w.pos++] = v;
            if (w.pos >= w.len) {
                if (w.type == 2) {
                    sector_done();
                } else {
                    write_track_done();
                }
            }
        }
        break;
    default:                            /* $FF: system register */
        w.system = v;
        w.drive = v & 3;
        if (w.drive >= ZX_DRIVES) {
            w.drive = ZX_DRIVES - 1;
        }
        w.side = (v & 0x10) ? 0 : 1;
        if (!(v & 0x04)) {
            /* controller reset */
            w.busy = w.drq = false;
            w.intrq = false;
            w.status = 0;
            w.type = 1;
        }
        break;
    }
}

void beta_frame(void)
{
    if (w.light_frames && --w.light_frames == 0) {
        w.stat.light = -1;
    }
}

void beta_reset(void)
{
    uint8_t cyl[ZX_DRIVES];
    memcpy(cyl, w.cyl, sizeof(cyl));
    zx_disk_status_t stat = w.stat;
    uint8_t *raw = w.raw;
    memset(&w, 0, sizeof(w));
    memcpy(w.cyl, cyl, sizeof(cyl));
    w.stat = stat;
    w.stat.light = -1;
    w.raw = raw;
    w.type = 1;
}
