#pragma once

#include <stdint.h>
#include "esp_err.h"

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

/* display_preinit() pre-allocates the 10 KiB DMA-capable chunk buffer.
 * Call from app_main BEFORE doom_zone_preinit() so the chunk gets a
 * clean DMA-capable block before the DOOM zone eats the largest
 * contiguous fragment. display_init() does the SPI/panel bring-up
 * and is fine to call after the zone is allocated. */
esp_err_t display_preinit(void);
esp_err_t display_init(void);

/* Blit a 320x200 8-bit palette-indexed DOOM framebuffer to the LCD.
 * Reads doomgeneric's `colors[256]` global (via I_SetPalette's side
 * effects, watched through `palette_changed`) for the conversion.
 * The 320x200 image is letterboxed onto the 320x240 panel with
 * 20-pixel black bars top/bottom. */
void display_blit_doom_frame(const uint8_t *fb320x200);
