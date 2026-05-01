# DefCon SG-01 — DOOM

DOOM running on the DEFCON SG-01 badge. ESP32-C6, 512 KB SRAM (no
PSRAM), 8 MB flash, 320×240 ST7789 LCD over SPI, 4 GPIO buttons, 16
WS2812B ring, vibration motor. Built on top of
[doomgeneric](https://github.com/ozkl/doomgeneric).

## What runs

Three ship targets, picked at trim-time:

1. **Shareware `DOOM1.WAD`** — E1M1 plays end-to-end. Bigger maps
   (E1M2+) need ~30–60 KiB more PU_LEVEL geometry than the zone has.
   The static reclaims in this branch (status-bar backing, dead
   `rgb565_palette`, sound channels) free ~10.7 KiB more zone, which
   may be enough for the smaller maps but is not yet a guarantee.
2. **Trimmed `DOOM_TRIM_E12.WAD` (E1+E2, 18 maps, ~6.3 MiB)** — the
   safe ship target. `tools/trim_wad.py --strip all --prune-unused
   --pack --squash-patches` from the user's legally-owned Ultimate
   Doom `DOOM.WAD`.
3. **Trimmed `DOOM_TRIM_E123.WAD` (E1+E2+E3, 27 maps, ~7.2 MiB)** —
   the original-retail-Doom ship target. Fits the badge's 8 MiB
   flash thanks to the WadPtr-style packing pipeline (SIDEDEFS row
   dedup, BLOCKMAP cell merge, patch column dedup, identical-lump
   directory-share, fullscreen-graphic placeholder substitution).
   No audio, no music, no intermission/HUD screens — the trim
   replaces those lumps with a 13-byte transparent placeholder so
   the renderer's `V_DrawPatch` calls don't `I_Error`.

Episode 4 cannot fit even after packing — sprites/textures are
irreducible without invasive firmware-side recompression.

## The fight

Vanilla DOOM expects roughly 4 MiB of heap (zone + lump cache). The
ESP32-C6 has 512 KiB of internal SRAM total and ESP-IDF baseline eats
most of it; raw `idf.py monitor` reports ~70 KiB of free heap after
init. Getting from "70 KiB heap" to "176 KiB DOOM zone" was the
project — see defcon page on [blossoms.one](https://blossoms.one/defcon) for
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
11. **`-Os` + `LOG_LEVEL_NONE`** in `sdkconfig.defaults` shrinks the app
    binary from 597 KiB → 497 KiB (–100 KiB). The freed flash goes to
    the `wad` partition, which is now 7 MiB to fit a trimmed Ultimate
    Doom IWAD.
12. **Status-bar backing buffer dropped.** `ST_refreshBackground` used
    to draw into a 10 KiB `Z_Malloc`'d buffer and `V_CopyRect` it down;
    we now draw straight into the framebuffer at `(ST_X, ST_Y)`. Saves
    10 KiB of zone every level.
13. **Dead `rgb565_palette[256]`** in `i_video.c` removed (~512 B
    `.bss`) — the populating loop was commented out years ago and the
    only reader is the mouse-accel widget, gated behind `usemouse=0`.
14. **`S_Init` skipped without `FEATURE_SOUND`** — the no-op sound
    layer was still allocating an 8-channel `channel_t` table for
    nothing.
15. **WadPtr-style WAD packing in tools/trim_wad.py** (`--pack`,
    `--squash-patches`, plus identical-lump directory-share at write
    time): SIDEDEFS row dedup, BLOCKMAP cell merge, patch column
    dedup. Lossless, output is a standard WAD. **~660 KiB saved on
    Ultimate Doom**; without these E1+E2+E3 wouldn't fit 8 MiB flash.

End state with shareware DOOM1.WAD: pre-zone heap **184 KiB**, largest
contig **160 KiB**, DOOM zone **156 KiB**. E1M1 fits with maybe 2–5 KiB
of headroom; the 10 KiB st_backing_screen reclaim should help E1M2+
fit but isn't yet measured on hardware.

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
# 1. Audit the user-supplied IWAD (read-only).
python3 tools/wad_audit.py DOOM.WAD

# 2. Trim + pack. Two common targets:
#
#    E1+E2+E3 (27 maps — original retail Doom scope):
python3 tools/trim_wad.py DOOM.WAD DOOM_TRIM.WAD \
        --strip music,sfx,speaker,sndcfg,demos,endoom,\
e4,fullscreens,intermission,screens \
        --prune-unused --pack --squash-patches \
        --keep-lumps TITLEPIC,VICTORY2
#
#    E1+E2 (18 maps — comfortable fit, more zone headroom):
python3 tools/trim_wad.py DOOM.WAD DOOM_TRIM.WAD \
        --strip all --prune-unused --pack --squash-patches \
        --keep-lumps TITLEPIC,VICTORY2

# 3. Bake the texture column tables AGAINST THE TRIM WAD. The bake is
#    keyed to the WAD's lump indices — running against the wrong WAD
#    silently produces garbled walls.
python3 tools/bake_textures.py DOOM_TRIM.WAD > main/baked_textures.c

# 4. Build firmware.
idf.py set-target esp32c6
idf.py build flash

# 5. Flash the WAD into the raw 'wad' partition (no filesystem).
./tools/flash_wad.sh DOOM_TRIM.WAD
```

Partition layout: **640 KiB app + 7.31 MiB wad** (see
`partitions.csv`). The packed E1+E2+E3 trim (~7.2 MiB) fits with
~110 KiB headroom; the binary (497 KiB) has 143 KiB growth headroom
in its slot. E1+E2 fits trivially (~6.3 MiB).

The packing pipeline (`--pack` + `--squash-patches` + lump-merge in
the writer) is bytes-equivalent to running [WadPtr](https://github.com/fragglet/wadptr)
on the trimmed WAD. The output is a *standard WAD* — Chocolate
DOOM and other source ports read it normally. Verify on desktop
before flashing if desired.

## Layout

```
dcsgonefw-doom/
├── CMakeLists.txt              ESP-IDF project root
├── partitions.csv              nvs / 1 MB app / 7 MB wad
├── sdkconfig.defaults          RAM tunings + -Os release config
├── DOOM.WAD                    user-supplied source IWAD (gitignored)
├── DOOM_TRIM.WAD               build artifact (gitignored)
├── doomgeneric/                upstream clone, with port patches
├── tools/
│   ├── wad_audit.py            read-only WAD lump-category byte audit
│   ├── trim_wad.py             produces a trimmed IWAD that fits flash
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

## WAD trim pipeline

`tools/wad_audit.py` reads any IWAD and prints a category breakdown
(music, SFX, sprites, flats, patches, maps grouped by episode, etc.)
so you can see what to cut. `tools/trim_wad.py` then cuts. Both are
read-only against the source WAD; they write a new IWAD.

```sh
python3 tools/wad_audit.py DOOM.WAD                    # see what's there
python3 tools/trim_wad.py --list-strippable            # category list
python3 tools/trim_wad.py DOOM.WAD --strip all --prune-unused --dry-run
python3 tools/trim_wad.py DOOM.WAD OUT.WAD --strip music,sfx,e4 ...
```

`--prune-unused` walks every kept map's SIDEDEFS / SECTORS, expands
through TEXTURE1/2 + PNAMES + the animation + switch tables, and drops
patches/flats nothing references. Vanilla DOOM has tightly-packed
assets so this typically only saves a few KiB once E4 maps are also
gone — the headline savings come from the strippable categories
(music + SFX + speaker = ~2 MiB on Ultimate Doom).

`--pack` does WadPtr-style lossless transformations:

- **SIDEDEFS row dedup**: typically half of all SIDEDEFS bytes in a
  vanilla map are duplicates (corridors with same texture both sides).
  Saves ~470 KiB on E1+E2+E3.
- **BLOCKMAP cell merge**: blocklists with identical content are
  shared. Saves ~75 KiB.

`--squash-patches` deduplicates identical columns within each wall-
texture patch (P_*) — full-wall patches like W110_1 / WALL24_1
contain many identical columns. Saves ~85 KiB.

The writer additionally merges identical lumps by sharing directory
offsets (~26 KiB on Ultimate Doom).

All four are lossless and produce a standard WAD readable by any
source port. See `--help` for the full flag list and the conservative
allow-list.

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
