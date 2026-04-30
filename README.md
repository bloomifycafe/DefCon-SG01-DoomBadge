# DefCon SG-01 — DOOM

DOOM running on the DEFCON SG-01 badge. ESP32-C6, 512 KB SRAM (no
PSRAM), 8 MB flash, 320×240 ST7789 LCD over SPI, 4 GPIO buttons, 16
WS2812B ring, vibration motor. Built on top of
[doomgeneric](https://github.com/ozkl/doomgeneric).

## What runs

Shareware **DOOM1.WAD** boots, the title cycle works, the menu works,
**E1M1 loads and plays** end-to-end. Bigger maps (E1M2+) probably
won't fit — they need ~30–60 KiB more PU_LEVEL geometry than we have
zone for.

## The fight

Vanilla DOOM expects roughly 4 MiB of heap (zone + lump cache). The
ESP32-C6 has 512 KiB of internal SRAM total and ESP-IDF baseline eats
most of it; raw `idf.py monitor` reports ~70 KiB of free heap after
init. Getting from "70 KiB heap" to "176 KiB DOOM zone" was the
project — see `/doom` page on [blossoms.one](https://blossoms.one) for
the full story.

Highlights of the techniques used:

1. **Texture column tables baked to flash** (`tools/bake_textures.py`).
   `R_GenerateLookup`'s output is deterministic from the WAD, so we
   pre-compute it offline and emit a `const` array. Saves 42 KiB of
   zone at runtime. Multi-patch composite columns are also baked away
   to single-patch fallbacks so `R_GenerateComposite` never runs.
2. **WAD mmap'd from a raw flash partition** (`main/wad_mmap.c`). No
   filesystem, no copying. `W_CacheLumpNum` returns a flash-resident
   pointer for every lump.
3. **Slim `lumpinfo_t`** (28 → 8 bytes). The `name` / `position` /
   `size` fields are derived on demand from the WAD's directory in
   flash. 1264 lumps × 20 bytes saved = ~25 KiB.
4. **`P_LoadBlockMap` reads directly from flash.** RISC-V is little-
   endian and so is the WAD, so the byte-swap loop is a no-op — point
   `blockmaplump` at the mmap'd lump and skip the Z_Malloc.
5. **Aggressive `.bss` cuts:** visplanes 128 → 48 (saves 63 KiB),
   drawsegs 256 → 96, vissprites 128 → 64, MAXOPENINGS scaled by
   `SCREENWIDTH*64` → `SCREENWIDTH*16`, BACKUPTICS 128 → 4, statdump
   buffer 32 → 1, `viewangletox` `int[]` → `short[]`. Total ~125 KiB
   reclaimed.
6. **Static framebuffer.** `I_VideoBuffer` is a 64 KiB `byte[]` in
   `.bss` instead of `malloc`'d, so it doesn't compete with the zone
   for the largest contig block.
7. **Zone allocated first.** `Z_Init` runs in `app_main` before
   display / buttons / LED init fragments the heap. The wrapper
   `__wrap_I_ZoneBase` is idempotent so DOOM's own later `Z_Init`
   gets the same buffer.
8. **Wipe transition disabled.** The screen-melt effect needed two
   64 KiB capture buffers we couldn't spare; gamestate just snaps
   between menu/level instead of melting.
9. **Auto-demo skipped.** Vanilla title cycle plays `demo1` (E1M5)
   which is too big to load — overridden to advance to the next
   static page instead.
10. **R_FindPlane gracefully degrades** on visplane overflow (reuses
    the last plane) instead of `I_Error`'ing.

End state: pre-zone heap **184 KiB**, largest contig **160 KiB**,
DOOM zone **156 KiB**. E1M1 fits with maybe 2–5 KiB of headroom.

## Hardware bring-up

| Peripheral       | Pin / driver                               |
| ---------------- | ------------------------------------------ |
| LCD ST7789       | SPI2 SCK=5 MOSI=11 CS=4 DC=3, 45 MHz       |
| 4× tact buttons  | GPIO 23 / 15 / 10 / 9 (active-low)         |
| WS2812B ring×16  | RMT on GPIO 8                              |
| Vibration motor  | GPIO 22 → P-channel MOSFET (active-LOW)    |

The vibration motor's gate is pulled to +3V3 through a 10 KΩ resistor
and driven by a P-channel MOSFET, so the GPIO must be driven HIGH for
the motor to stay OFF. Driven from `app_main` as the very first thing
so a crash mid-init doesn't leave the badge buzzing forever.

## Controls

| Button          | Single press     | Long press         | Other          |
| --------------- | ---------------- | ------------------ | -------------- |
| A (GPIO 23)     | turn LEFT        | —                  |                |
| B (GPIO 15)     | move FORWARD     | —                  | double-tap → USE |
| C (GPIO 10)     | turn RIGHT       | ENTER (menu)       |                |
| D (GPIO 9)      | FIRE + 70 ms 🔊  | —                  |                |
| **A + C chord** | —                | —                  | **ESCAPE / menu** |

The double-tap-USE pattern is the cleanest gesture for "walk up to
door and open it". The A+C chord opens the pause menu without
stealing a single-button slot from gameplay. `D` triggers a 70 ms
vibration pulse via `esp_timer` — short enough to feel like a tap;
re-firing extends the buzz instead of stuttering.

A subtle bug we hit: with `enqueue(press); enqueue(release)` back-to-
back, both events flush before `G_BuildTiccmd` reads `gamekeydown[]`,
so in-game USE/ENTER never registered. Menu navigation worked because
`M_Responder` consumes events as they arrive instead of latching at
tic boundaries. Fix: schedule the release ~50 ms later via
`esp_timer` so it lands in a different DOOM tic.

## Build

```sh
# Bake the texture column tables for whichever WAD you're shipping.
python3 tools/bake_textures.py DOOM1.WAD > main/baked_textures.c

# Build firmware.
idf.py set-target esp32c6
idf.py build flash

# Flash the WAD into the raw 'wad' partition (no filesystem).
./tools/flash_wad.sh DOOM1.WAD
```

The WAD partition is sized 5 MiB because the shareware DOOM1.WAD is
4,196,020 bytes — 1716 bytes over the natural 4 MiB boundary because
the lump-directory tail spills over. A 4 MiB partition silently
truncates ~107 directory entries (including F_START / F_END) and the
WAD reader returns garbage for them. Don't ask how long this took to
debug.

## Layout

```
dcsgonefw-doom/
├── CMakeLists.txt              ESP-IDF project root
├── partitions.csv              nvs / 2 MB app / 5 MB wad
├── sdkconfig.defaults          all the RAM tunings
├── DOOM1.WAD                   shareware WAD (provide your own)
├── doomgeneric/                upstream clone, with port patches
├── tools/
│   ├── bake_textures.py        offline column-table baker
│   └── flash_wad.sh            esptool wrapper for the wad partition
└── main/
    ├── main.c                  app_main, zone preinit, vibration
    ├── doomgeneric_esp32c6.c   DG_* hooks
    ├── display.c, display.h    ST7789 driver + palette→RGB565 blit
    ├── buttons.c, buttons.h    iot_button → DOOM key events
    ├── led_feedback.c, .h      WS2812B ring driven by player state
    ├── doom_stubs.c            I_ZoneBase + idempotent zone alloc
    ├── wad_mmap.c              raw-flash WAD backend
    ├── baked_textures.c        generated by bake_textures.py
    └── r_baked.h               struct for the baked tables
```

## What this isn't

- Multi-level. E1M1 is the only level that fits.
- Sound. `FEATURE_SOUND` is undefined; `i_sound.c` compiles to no-ops.
- Multiplayer. `FEATURE_MULTIPLAYER` off, `BACKUPTICS=4`.
- Pretty. Multi-patch decals (light fixtures on TEKWALL1, switch icons
  on some SW1XXX textures) don't render — we baked away the runtime
  composite step that overlays them.

## Credits

- [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl — the
  base port that did the heavy lifting of separating DOOM from SDL.
- [Chocolate DOOM](https://github.com/chocolate-doom/chocolate-doom)
  for the WAD-mmap fastpath in `w_wad.c`.
- [RP2040-DOOM](https://kilograham.github.io/rp2040-doom/) by Graham
  Sanderson for proving microcontroller DOOM is possible at all and
  setting the bar for "bake everything to flash."
