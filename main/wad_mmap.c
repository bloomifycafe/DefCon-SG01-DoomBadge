/*
 * wad_mmap.c — flash-mmap'd WAD backend for doomgeneric.
 *
 * doomgeneric's WAD code (w_wad.c) already supports memory-mapped lump
 * access: at line 399 of W_CacheLumpNum, when wad_file->mapped is non-NULL
 * the function short-circuits and returns mapped+offset directly — no
 * Z_Malloc, no copy. The same `wad_file_t` is also used by W_Read which
 * dispatches through wad_file_class_t::Read (we provide a memcpy-from-
 * mmap implementation).
 *
 * On ESP32-C6 with no PSRAM, this is the difference between DOOM running
 * (with the entire 4 MB WAD addressable as a flash-XiP region) and DOOM
 * dying on the first lump cache (~no usable heap left after Z_Init).
 *
 * Linker-wraps W_OpenFile so doomgeneric's class-array machinery is
 * bypassed entirely — we always return our mmap-backed wad_file_t.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_log.h"

/* doomgeneric headers — must come after the IDF includes because doom's
 * `boolean` typedef collides with ESP-IDF's `bool` aliases on some
 * configs. */
#include "doomtype.h"
#include "z_zone.h"
#include "w_file.h"
#include "d_mode.h"   /* GameMission_t enum (for D_FindIWAD wrap) */

static const char *TAG = "wad_mmap";

/* Custom wad_file_t subclass that also holds the partition mmap handle so
 * we can munmap on close. The wad_file_t base must come first so a plain
 * cast (esp32_wad_file_t *) ↔ (wad_file_t *) works. */
typedef struct {
    wad_file_t                  base;
    esp_partition_mmap_handle_t handle;
    const esp_partition_t      *part;
} esp32_wad_file_t;

static void   esp32_CloseFile(wad_file_t *file);
static size_t esp32_Read(wad_file_t *file, unsigned int offset,
                         void *buffer, size_t buffer_len);

static wad_file_class_t esp32_wad_class = {
    /* W_OpenFile is wrapped, so this slot is unused — fill it for safety. */
    .OpenFile  = NULL,
    .CloseFile = esp32_CloseFile,
    .Read      = esp32_Read,
};

/* Linker-wrapped via -Wl,--wrap=W_OpenFile (see main/CMakeLists.txt).
 * Ignores `path` because the WAD lives in a raw flash partition, not in
 * any filesystem. */
wad_file_t *__wrap_W_OpenFile(char *path)
{
    (void)path;

    /* Find by label so the user can't accidentally break this by
     * renumbering the type/subtype in partitions.csv. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "wad");
    if (!part) {
        ESP_LOGE(TAG, "no 'wad' partition found in flash — flash one with "
                      "tools/flash_wad.sh");
        return NULL;
    }

    const void *map_ptr = NULL;
    esp_partition_mmap_handle_t map_handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &map_ptr, &map_handle);
    if (err != ESP_OK || !map_ptr) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s",
                 esp_err_to_name(err));
        return NULL;
    }

    /* Quick sanity check: a real WAD starts with "IWAD" or "PWAD". */
    const char *magic = (const char *)map_ptr;
    if (memcmp(magic, "IWAD", 4) != 0 && memcmp(magic, "PWAD", 4) != 0) {
        ESP_LOGW(TAG, "wad partition doesn't start with IWAD/PWAD — "
                      "got '%c%c%c%c'. Did you flash a WAD?",
                 magic[0], magic[1], magic[2], magic[3]);
        /* fall through anyway — DOOM will print its own error */
    }

    /* Sanity check the directory fits within the partition. The header
     * has numlumps (offset 4) and infotableofs (offset 8) as little-endian
     * uint32s. Truncating the directory silently leads to W_AddFile
     * filling lumpinfo[] with garbage from beyond the partition, which
     * surfaces hours later as bizarre "lump not found" errors deep in
     * R_Init. Catching it here saves the next person a debugging session. */
    {
        const uint8_t *hdr = (const uint8_t *)map_ptr;
        uint32_t numlumps = (uint32_t)hdr[4]  | ((uint32_t)hdr[5]  << 8)
                          | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        uint32_t ito      = (uint32_t)hdr[8]  | ((uint32_t)hdr[9]  << 8)
                          | ((uint32_t)hdr[10]<< 16) | ((uint32_t)hdr[11]<< 24);
        uint64_t dir_end  = (uint64_t)ito + (uint64_t)numlumps * 16;
        if (dir_end > (uint64_t)part->size) {
            ESP_LOGE(TAG, "WAD directory overflows partition: header says "
                          "numlumps=%u ito=0x%x (dir_end=0x%llx) but "
                          "partition is 0x%x bytes — grow the wad partition "
                          "in partitions.csv",
                     (unsigned)numlumps, (unsigned)ito,
                     (unsigned long long)dir_end, (unsigned)part->size);
        }
    }

    /* Allocated on the (small) C heap via malloc rather than Z_Malloc
     * because the zone allocator might not exist yet at this point
     * (Z_Init runs before W_Init). */
    esp32_wad_file_t *r = calloc(1, sizeof(esp32_wad_file_t));
    if (!r) {
        esp_partition_munmap(map_handle);
        return NULL;
    }
    r->base.file_class = &esp32_wad_class;
    r->base.mapped     = (byte *)map_ptr;
    r->base.length     = part->size;
    r->handle          = map_handle;
    r->part            = part;

    ESP_LOGI(TAG, "WAD mmap'd: %u KiB at %p (label='%s')",
             (unsigned)(part->size / 1024), map_ptr, part->label);
    return &r->base;
}

static void esp32_CloseFile(wad_file_t *file)
{
    esp32_wad_file_t *w = (esp32_wad_file_t *)file;
    esp_partition_munmap(w->handle);
    free(w);
}

static size_t esp32_Read(wad_file_t *file, unsigned int offset,
                         void *buffer, size_t buffer_len)
{
    /* The WAD is already mapped into the address space — straight memcpy
     * from the mapped region. Bounds-check defensively. */
    if (offset >= file->length) return 0;
    if (offset + buffer_len > file->length) {
        buffer_len = file->length - offset;
    }
    memcpy(buffer, file->mapped + offset, buffer_len);
    return buffer_len;
}

/* Linker-wrapped via -Wl,--wrap=D_FindIWAD. Upstream's D_FindIWAD calls
 * D_FindWADByName which stat()s real filesystem paths — useless here
 * because the WAD lives in raw flash, not on a filesystem. We return a
 * constant filename and pin the mission to `doom` (Doom 1 family).
 *
 * NOTE: gamemode (shareware vs registered vs retail) is NOT pinned
 * here. Setting `gamemission = doom` makes D_IdentifyVersion() probe
 * the WAD for `E4M1` / `E3M1` lumps and choose retail / registered /
 * shareware automatically — which matches whatever trim_wad.py shipped.
 * If the trim drops E4 maps the badge correctly downgrades to
 * `registered`, the episode menu hides E4 (m_menu.c:2118), and the
 * intermission/finale graphics paths use the right gamemode. */
char *__wrap_D_FindIWAD(int mask, GameMission_t *mission)
{
    (void)mask;
    if (mission) *mission = doom;
    /* Non-NULL pointer so callers don't bail. The string itself is
     * referenced by DOOM in printf-style logging, so make it look right. */
    static char fake_path[] = "DOOM.WAD";
    return fake_path;
}
