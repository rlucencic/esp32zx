#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The Spectrum's ROMs: built in, from the zx_roms flash partition
 * (roms/<name> in the project, written by idf.py flash into 16 KB slots). A
 * file of the same name in the SD card's "zx/roms" folder overrides one.
 */
typedef enum { ROM_128, ROM_PLUS2, ROM_PLUS3, ROM_TRDOS, ROM_COUNT } rom_id_t;

const char *rom_store_name(rom_id_t id);    /* "128.rom" */
uint32_t rom_store_size(rom_id_t id);       /* 32, 32, 64 or 16 KB */

/* The whole ROM into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);
