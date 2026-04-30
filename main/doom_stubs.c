/*
 * doom_stubs.c — empty implementations + wrappers for DOOM functions
 * whose default behaviour doesn't suit this 512 KB-RAM badge.
 *
 *   I_Endoom               — terminal "ENDOOM" graphic on quit (no terminal).
 *   I_InitJoystick,        — SDL joystick subsystem (no SDL).
 *   I_BindJoystickVariables
 *   __wrap_I_ZoneBase      — replaces upstream I_ZoneBase. Vanilla DOOM
 *     hardcodes a 6 MiB minimum zone request; on this MCU that's hopeless,
 *     so we substitute a tiered fallback that grabs the largest block the
 *     heap can give us. DOOM still gets a working zone, just much smaller —
 *     enough for the title screen + menus; level loads probably fail.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"   /* esp_get_free_heap_size */

typedef unsigned char byte;
static const char *TAG = "doom_stub";

void I_Endoom(void)              { /* no terminal */ }
void I_InitJoystick(void)        { /* no joystick */ }
void I_BindJoystickVariables(void){ /* no joystick */ }

/* Linker-wrapped via -Wl,--wrap=I_ZoneBase in main/CMakeLists.txt.
 *
 * Vanilla DOOM grabs 6 MiB; we obviously can't. The trick on ESP32-C6
 * is that esp_get_free_heap_size() is misleading — it sums all heap
 * regions, but malloc needs a single contiguous block. The metric that
 * actually matters is heap_caps_get_largest_free_block().
 *
 * We probe the largest contiguous block once, then ask for that minus
 * a small safety margin so non-zone allocations (the wad_file_t struct,
 * the lumphash table, etc.) still have room. lumpinfo itself no longer
 * competes for heap — it's been moved to a static .bss pool inside
 * w_wad.c, which sidesteps fragmentation entirely. */
byte *__wrap_I_ZoneBase(int *size)
{
    /* Idempotent: if app_main already called Z_Init via doom_zone_preinit(),
     * DOOM's later Z_Init from D_DoomMain hits this wrapper a second time —
     * we hand back the same buffer + size. Without this the second call
     * would malloc again and leak the first zone. */
    static byte  *s_zone   = NULL;
    static size_t s_size   = 0;
    if (s_zone) {
        *size = (int)s_size;
        return s_zone;
    }

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t total   = esp_get_free_heap_size();
    ESP_LOGW(TAG, "heap pre-zone: total=%u KiB, largest contig=%u KiB",
             (unsigned)(total / 1024), (unsigned)(largest / 1024));

    /* No reserve — squeeze every byte of contig into the DOOM zone.
     * The post-zone heap callers (wad_file_t struct ~30 B, SPI driver
     * task state, button library) all fit in the smaller fragments
     * left after the largest contig is taken; a 2 KiB nominal reserve
     * was just costing us 2 KiB of zone for nothing. */
    const size_t RESERVE = 0;
    size_t target = (largest > RESERVE + 16 * 1024)
                  ? (largest - RESERVE)
                  : 16 * 1024;

    /* Step down in 8 KiB increments if our first ask fails (heap state
     * can change slightly between the probe and the malloc). */
    for (size_t s = target; s >= 16 * 1024; s -= 8 * 1024) {
        void *p = malloc(s);
        if (p) {
            *size = (int)s;
            s_zone = (byte *)p;
            s_size = s;
            ESP_LOGW(TAG, "zone: %u KiB at %p (free heap now %u KiB, "
                          "largest contig %u KiB)",
                     (unsigned)(s / 1024), p,
                     (unsigned)(esp_get_free_heap_size() / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
            return (byte *)p;
        }
    }

    /* No fallback. The 16 KiB static buffer we used to keep here
     * cost 16 KiB of .bss permanently and never resulted in a
     * playable boot anyway — better to fail loudly and recover the
     * RAM. If we get here the badge truly has no usable heap. */
    ESP_LOGE(TAG, "zone alloc completely failed — aborting");
    *size = 0;
    return NULL;
}

/* Called from app_main to grab the zone block BEFORE display/buttons/LED
 * init fragment the heap. Triggers the Z_Init machinery the same way
 * DOOM's D_DoomMain would, but minutes earlier in the boot timeline. */
void doom_zone_preinit(void)
{
    extern void Z_Init(void);
    Z_Init();
}
