/*
 * doomgeneric_esp32c6.c — platform layer for doomgeneric on this badge.
 *
 * Implements the six DG_* hooks doomgeneric expects:
 *   DG_Init           : called once before the main loop
 *   DG_DrawFrame      : blit DG_ScreenBuffer to the LCD
 *   DG_SleepMs        : vTaskDelay wrapper
 *   DG_GetTicksMs     : esp_timer-based monotonic ms
 *   DG_GetKey         : pop a (pressed, doomkey) pair from the button queue
 *   DG_SetWindowTitle : ignored (we don't have a window title bar)
 *
 * In CMAP256 mode (uint8_t per pixel) the framebuffer is 320*200 = 64 KB
 * of palette indices. doomgeneric maintains a 256-entry palette internally
 * (via I_SetPalette) — we expose that via the helper below so the LCD
 * blit can convert palette index → RGB565 line by line.
 */
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "display.h"
#include "buttons.h"
#include "led_feedback.h"

static const char *TAG = "doomgen";

void DG_Init(void)
{
    ESP_LOGI(TAG, "DG_Init");
    /* Display + buttons were already brought up in app_main; nothing to
     * do here. doomgeneric will start writing into DG_ScreenBuffer
     * immediately after this returns. */
}

void DG_DrawFrame(void)
{
    /* DG_ScreenBuffer is 320*200 = 64,000 bytes of 8-bit palette indices.
     * The LCD wants RGB565. display_blit_doom_frame() handles the
     * palette-indexed→RGB565 conversion. */
    display_blit_doom_frame((const uint8_t *)DG_ScreenBuffer);

    /* Reflect game state on the LED ring once per frame. Reads players[0]
     * directly from the doom core. */
    led_feedback_tick();
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    return buttons_pop_doomkey(pressed, doomKey);
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
    /* No-op: the LCD doesn't have a title bar. */
}

/* doomgeneric calls I_SetPalette() which we hook by overriding the
 * symbol in the doom core. Easiest path is: doomgeneric core writes the
 * 768-byte (256 * RGB) palette into a global it owns; we read it from
 * display.c at blit time. We don't need to do anything here in the
 * platform layer. */
