/*
 * Free space on the disks, for the web page, as their DOS counts it
 * (esp32emu's diskspace).
 *
 * TR-DOS: the free sectors in the system sector. +3DOS: CP/M's directory;
 * the format from track 0's lowest sector ID: the CPC's system (&41-&49,
 * two reserved tracks) or data (&C1-&C9, none), or with IDs from 1 the
 * disk specification in the first sector (type 0, or 3: two sides), the
 * +3's own format when that sector is filler (40 tracks of 9 sectors of
 * 512, one reserved, 1 KB blocks, 64 directory entries).
 *
 * Worked out at insert, eject and flush (where the disks may be read; the
 * writes in between are in memory and get flushed), kept for the web
 * page's task.
 */
#include <string.h>
#include "zx_internal.h"
#include "upd765.h"
#include "diskspace.h"

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
} space_t;

static space_t s_space[ZX_DRIVES];

/* ---- TR-DOS ---- */

static bool trd_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    int drive = (int)(intptr_t)ctx;
    uint8_t *p = buf;
    while (len) {
        const uint8_t *sec = beta_sector(drive, offset / 256);
        uint32_t in = offset % 256, part = 256 - in < len ? 256 - in : len;
        if (sec == NULL) {
            return false;
        }
        memcpy(p, sec + in, part);
        offset += part;
        p += part;
        len -= part;
    }
    return true;
}

/* ---- +3DOS ---- */

typedef struct {
    int drive;
    uint8_t first_id;
    uint32_t sector_size, sectors;
    uint32_t sides, tracks;         /* two sides: alternate, or successive */
    bool successive;
} cpm_disk_t;

/* CP/M's track t, sector s: on its cylinder and side, ID first_id + s */
static bool cpm_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const cpm_disk_t *c = ctx;
    uint8_t *p = buf;
    while (len) {
        uint32_t n = offset / c->sector_size, in = offset % c->sector_size;
        uint32_t part = c->sector_size - in < len ? c->sector_size - in : len;
        uint32_t t = n / c->sectors, cyl = t, head = 0;
        if (c->sides == 2 && c->successive) {
            cyl = t < c->tracks ? t : t - c->tracks;
            head = t < c->tracks ? 0 : 1;
        } else if (c->sides == 2) {
            cyl = t / 2;
            head = t % 2;
        }
        if (!upd765_disk_read(c->drive, cyl, head, c->first_id + n % c->sectors, in, p, part)) {
            return false;
        }
        offset += part;
        p += part;
        len -= part;
    }
    return true;
}

static bool plus3_count(int drive, diskspace_t *s)
{
    uint8_t ids[32];
    int n = upd765_disk_track_ids(drive, 0, 0, ids, sizeof(ids));
    if (n == 0) {
        return false;
    }
    uint8_t first = ids[0];
    for (int i = 1; i < n; i++) {
        first = ids[i] < first ? ids[i] : first;
    }
    diskspace_cpm_geometry_t g = { 512, 9, 40, 1, 1024, 64 };
    cpm_disk_t c = { drive, first, 512, 9, 1, 40, false };
    if (first == 0x41 || first == 0xC1) {
        g.reserved_tracks = first == 0x41 ? 2 : 0;
    } else if (first == 0x01) {
        /* type, sides, tracks a side, sectors a track, log2(sector size) - 7,
         * reserved tracks, log2(block size) - 7, directory blocks
         */
        uint8_t spec[8];
        if (!upd765_disk_read(drive, 0, 0, first, 0, spec, sizeof(spec))) {
            return false;
        }
        if (spec[0] == 0 || spec[0] == 3) {
            if (spec[4] > 3 || spec[6] < 3 || spec[6] > 7) {
                return false;
            }
            c.sides = (spec[1] & 3) ? 2 : 1;
            c.successive = (spec[1] & 3) == 2;
            c.tracks = spec[2];
            c.sectors = g.sectors = spec[3];
            c.sector_size = g.sector_size = 128u << spec[4];
            g.tracks = spec[2] * c.sides;
            g.reserved_tracks = spec[5];
            g.block_size = 128u << spec[6];
            g.dir_entries = spec[7] * g.block_size / 32;
        } else if (spec[0] != 0xE5) {
            return false;
        }
    } else {
        return false;
    }
    diskspace_disk_t d = { cpm_read, &c, 0 };
    return diskspace_cpm(&d, &g, s);
}

static bool count(int drive, uint64_t *free_bytes, uint64_t *size)
{
    diskspace_t s;
    bool known;
    if (beta_has_disk(drive)) {
        diskspace_disk_t d = { trd_read, (void *)(intptr_t)drive, 0 };
        known = diskspace_trdos(&d, &s);
    } else {
        known = upd765_disk_path(drive) && plus3_count(drive, &s);
    }
    if (known) {
        *free_bytes = s.free;
        *size = s.size;
    }
    return known;
}

void space_update(void)
{
    for (int d = 0; d < ZX_DRIVES; d++) {
        uint64_t f = 0, size = 0;
        bool known = count(d, &f, &size);
        s_space[d].known = false;
        s_space[d].free = f;
        s_space[d].size = size;
        s_space[d].known = known;
    }
}

bool zx_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= ZX_DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
