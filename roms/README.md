# ZX Spectrum ROM images

Put the ROM images here (not part of this repository). `idf.py flash` writes
them into 16 KB slots of the `zx_roms` flash partition; only the ones for the
models you want are needed.

| File | What | Size | Models |
|---|---|---|---|
| `128.rom` | Sinclair ZX Spectrum 128: the 128 editor (ROM 0) and 48 BASIC (ROM 1) | 32 KB | 128K |
| `plus2.rom` | Amstrad ZX Spectrum +2 (grey): its editor and 48 BASIC | 32 KB | +2 |
| `plus3.rom` | ZX Spectrum +3 v4.0 (English): ROMs 0-3, +3DOS in ROM 2 | 64 KB | +3 |
| `trdos.rom` | TR-DOS 5.04T, the Beta 128 disk interface's ROM | 16 KB | 128K, +2 (disks) |

Without `trdos.rom` the 128K and +2 run without disks.

Without flashing, the emulator also finds them in the `zx/roms` folder on the
SD card under the same names. A card set up before 2026-11-17 has them in a
shared `roms` folder: esp32emu's `tools/sd_folders.ps1 E:\` (PowerShell 7,
the card in a card reader) moves them there.

Amstrad allowed the Spectrum ROMs to be distributed with emulators, as long
as the copyright messages stay intact. Dumps are on Planet Emulation's
ZX Spectrum firmware page (planetemu.net), among others: "ZX Spectrum 128K
(1986)(Sinclair Research)(128K)", "ZX Spectrum 128 +2 (1986)(Amstrad)[aka ZX
Spectrum +2]", "ZX Spectrum 128 +3 v4.0 (1987)(Amstrad)(+2A-+3)[aka ZX
Spectrum +2A-+3]" (the 64 KB one; the "(GB)(es)" set is the Spanish +3) and
"Beta 128 Disk Interface v5.04T (1987)(Technology Research)".
