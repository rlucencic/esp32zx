# esp32zx: ZX Spectrum 128K / +2 / +3 on the Makerfabs MaTouch ESP32-S3 7"

A ZX Spectrum (Z80 with the ULA's contention, ULA, AY, 128K/+3 paging; the
+3's uPD765, a Beta 128 with TR-DOS on the 128K and +2) on an ESP32-S3
board with a 1024x600 touch LCD. Bluetooth keyboard, tapes (TAP, TZX),
disks (DSK, TRD, SCL), snapshots (SNA, Z80), an LVGL setup screen, a web
page for images and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console), the Z80 core (`components/z80`) and the uPD765
(`components/upd765`) are the shared library in `../esp32emu`, also used by
`../esp32ste`, `../esp32mac` and `../esp32cpc` (which shares the Z80 and
the uPD765). See its CLAUDE.md for the runtime and the board-level lessons;
changes there need all four projects built.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac.

## Build, flash, debug

Build like the others (the shell's default Python environment doesn't match):

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 -b 115200 app-flash'
```

- **Flash with `-b 115200`**: at the default 460800 the chip stopped
  responding twice while writing the app (2026-10-17); 115200 worked (the
  stub still writes at ~1.1 Mbit/s).
- `app-flash` writes only the app; `idf.py flash` also writes the ROMs in
  `roms/` (not in git) into 16 KB slots of the `zx_roms` partition: 128.rom
  at 0, plus2.rom at 2, plus3.rom at 4, trdos.rom at 8 (top `CMakeLists.txt`,
  `main/rom_store.c`). That partition overlaps the other machines' ROM
  partitions: switching the board needs a full `idf.py flash` there (and
  here). NVS (WiFi, BLE pairings) is shared.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset`
  with the IDF venv's Python (`-I`). One touch per `send` call.
- `curl http://spectrum.local/api/status`; disks: see `../esp32emu/README.md`
  (drives 1 A:, 2 B:, 3 Tape, 4 Snapshot).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i`, see
`emu/console.h`; `F <d> +` makes the model's kind of blank disk, or an
empty tape in drive 2) and the Spectrum's (`main/main.c`): `k <n>` tap key
number n (half-row * 8 + bit, 48 = ENTER, 56 = SPACE, 0 = CAPS SHIFT),
`K <n> 0|1`, `T <text>` type (`\n` = ENTER), `j <bits>` joystick, `P` tape
play/stop, `W` rewind, `R <model>` restart as 0 128K, 1 +2, 2 +3.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Spectrum as an `emu_machine_t`: memory, start, key map, joystick, drives, status, console |
| `main/rom_store.c` | ROMs from the `zx_roms` partition slots, else the SD card's `roms/` |
| `main/settings.c` | NVS `zx`: `model`, `joystick`, `tape_fast` |
| `components/zx/` | The Spectrum, platform independent (`include/zx.h`) |
| `components/zx/host/` | Host build: `zx_host` (see Testing) |
| `roms/` | `128.rom`, `plus2.rom`, `plus3.rom`, `trdos.rom` (see `roms/README.md`; not in git) |
| `partitions.csv` | nvs, phy, factory 8 MB, zx_roms 256K (type 0x40), storage |

## The core (`components/zx`)

- Z80: esp32emu's, `CONFIG_Z80_TIMING_TSTATES` + `CONFIG_Z80_CONTENTION`.
  Each memory access to a bank flagged in `z80_contended[]` waits
  `z80_contention[z80.t + offset]` (the table is 70908 + 512 bytes in
  PSRAM); offsets count 4 per opcode fetch and 3 per other access, not the
  internal cycles. I/O gets the time of the I/O cycle (start + 1) and adds
  its own wait through `z80.wait`.
- `zx.c`: paging (128K `$7FFD` A15=0 A1=0; +3 `$7FFD` A15-14=01, `$1FFD`
  A15-12=0001, all-RAM configs), contended banks (128K odd, +3 4-7), ROM
  writes into a sink, I/O decoding, the interrupt (36 T on the 128K, 32 on
  the +3; vector $FF), the frame loop: TR-DOS paging (in when an
  instruction starts at $3D00-$3DFF with the 48 BASIC ROM, out at $4000+),
  tape traps at $0556/$056B/$04C2 (48 BASIC ROM only), HALT skipped to the
  frame's end.
- `ula.c`: contention table (128K: 6,5,4,3,2,1,0,0 from T 14361; +3:
  1,0,7,6,5,4,3,2 from 14365), port contention (Fuse's rules: high byte in
  a contended bank, A0=0), floating bus (128K: bitmap/attribute bytes in
  the first 4 T of each 8), the picture: 352x300 RGB565, paper lines drawn
  at their start time from the screen bank then (cached: unchanged lines
  not redrawn), border changes recorded (time, colour) and drawn at frame
  end (two pixels per T), skipped when the border stayed one colour.
- `audio.c`: AY (render side as in the CPC, 7 ticks of 16 T per sample of
  112 T, 31669 Hz, mono), beeper and tape level as events averaged over each
  sample, DC filter.
- `tape.c`: TAP/TZX index, player as segments (each pulse flips the level
  at its start; pauses: an edge, 1 ms, then low; a stop or the end adds
  the closing edge), traps, loader detection, edge-loop acceleration.
- `beta.c`: WD1793 (types I-IV, no rotation timing except the index pulse:
  4 ms every 200 ms from the T-state clock), TRD in memory as
  [cyl][side][sector] (one-sided images spread out), SCL turned into a TRD
  (read-only), writes by sector, FORMAT parsed from the raw track; and the
  `zx_disk_*` calls for both kinds of drive (by extension).
- `snap.c`: SNA 48K/128K, Z80 v1-v3 (Pentagon, Scorpion ... refused);
  48K snapshots get `$7FFD` = $30 (+3: `$1FFD` = $04).
- `input.c`: queue, matrix, typing (3 frames down, 3 up), joystick types.

## Persistent state

- NVS `zx`: `model`, `joystick` (F11 cycles it), `tape_fast`. NVS
  `zxdisks`: `disk_a`..`disk_d` (A:, B:, tape, snapshot).
- SD card: `zx/` (all images; made if missing), `roms/` (optional ROMs).
- A snapshot "in the drive" loads when inserted and again at every start.

## Memory budget (measured on the board)

- PSRAM: RAM 128 KB, ROM 64 KB, TR-DOS 16 KB, picture 211 KB, contention
  table 71 KB, at power-on; ~5.1 MB free while running.
- Internal RAM: ~77 KB free while running (min ~59 KB).
- Core 1: ~35% busy at the 128 menu, 54-56% in Exolon at 100% speed.

## Lessons

- **TR-DOS sends a stray WRITE TRACK ($F8)** when SAVE selects the drive:
  its seek helper ORs whatever is in B (the SAVE token) into the command. A
  real WD1793 ends a WRITE TRACK with LOST DATA, writing nothing, if no
  byte comes before the next index pulse; waiting for data hung TR-DOS.
- **A TR-DOS boot needs the 48 BASIC ROM behind it**: at reset with a disk
  in A:, TR-DOS is paged in and `$7FFD` = $10, else it leaves its ROM into
  the 128 editor and crashes.
- **TZX pauses start with an edge** (then 1 ms, then low), and a stop or
  the end of the tape ends the last pulse with an edge; a stopped tape
  leaves EAR at its last level (the Issue 3 EAR-out coupling only without a
  tape). Speedlock 2 and 4 failed on the last bit of each block until all
  three were right.
- **The LD-BYTES trap returns through the stack slot LD-BYTES pushed**
  ($053F normally): loaders that load over the stack (Turrican) put their
  own address there and expect the ROM's final RET to go to it.
- **The SAVE trap skips SA/LD-RET's BREAK test**: the key that answered
  "press any key" (SPACE) is still down when the save ends at once, and
  the ROM stopped with "D BREAK" between header and data.
- **Edge-loop acceleration only while the level is still the one the loop
  waits to change**, else B is counted up to the following edge and one
  edge is lost.
- **Loader detection**: a loader reads EAR >1000 times a frame, a game its
  keys a few dozen. The tape plays after 3 such frames while stopped, and
  pauses after 2 s without (when it started that way).
- In host scripts: the 128 and +3 editors use the first key after a report
  to clear it, and NEW goes back to the start-up menu; the +3 waits for a
  key after a disk SAVE; give disk commands ~200 frames.

## Testing

- **Host build**: `make -C components/zx/host`, then
  `build/zx_host <128|plus2|plus3> [frames] [out.ppm]` (704x600). Options in
  `host_main.c`: `ZX_ROMS`, `ZX_DISKA/B`, `ZX_NEWDISK` (.dsk: +3, else TRD),
  `ZX_TAPE`, `ZX_NEWTAPE`, `ZX_SLOW` (real-time tape), `ZX_SNAP` (at frame
  100), `ZX_JOY`, `ZX_SCRIPT` (lines `<frame> type <text> | key <n> 0|1 |
  joy <bits> | shot <ppm> | insert <d> <path> | eject <d> | tape
  <path>|play|stop|rewind | snap <path> | peek <hex> | dump <file> |
  reset`), `ZX_STATUS=<n>`, `ZX_TRACE=<pc>[:<n>]` + `ZX_TRACE_FRAME` +
  `ZX_RING=<n>` (the n instructions before), `ZX_WAV`, `ZX_QUIET`,
  `ZX_BETALOG` (WD1793 commands), `ZX_NOACCEL`.
- Verified on the host: all three models boot to their menus; 128 BASIC
  program, BORDER, BEEP (529 Hz, 0.19 s for `BEEP 0.2,12`); SAVE/LOAD
  through the traps and LOAD in real time; +3 SAVE/CAT/LOAD on a blank
  disk; TR-DOS boot, SAVE, LIST on a blank TRD; tapes: Cybernoid, Exolon,
  Nebulus (Hewson), Arkanoid (Speedlock 2), Renegade 128K (Speedlock 4)
  load and run; Turrican and Rainbow Islands load (long real-time loads,
  not watched to the end); snapshots: 48K SNA, Z80 v1 and v2.
- On the board: 128K menu at 100% speed; Exolon from a TZX uploaded through
  the web page, in game at 100%; +3 SAVE/CAT on a blank disk made with
  `F 0 +` (written back to the SD card); TR-DOS boot, SAVE, LIST; the Disks
  page (four drives in one row each), Insert lists by drive, a Z80 snapshot
  inserted from the setup screen.
- Not verified yet: sound on the speaker, a BLE keyboard on the Spectrum,
  the web page's drop area with ZIPs, a +3 game disk, TRD/SCL game disks,
  128K snapshots.

## Open items

- No 48K model, no clones, no Interface 1/Microdrive, no printer.
- Contention without internal cycles; screen changes within a line show
  from the next line.
- No rotation timing in the floppy controllers; SCL read-only.
- Turbo loaders not built on the ROM's edge loop load in real time.

## README and GitHub

Public at `github.com/rlucencic/esp32zx` (pushed to `main`), with topics
and an About text like the siblings'.

## README images

In `docs/images/` (board screenshots doubled to 704x600 with a nearest
upscale; `screenshot` saves 352x300): `menu-128.png`, `plus3-cat.png`,
`trdos-catalogue.png`, `setup-disks.png` (the Disks page, no private data).

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing. Screenshots: a new file each time, never over an
  existing one.
