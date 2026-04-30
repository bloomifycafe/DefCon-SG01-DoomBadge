/*
 * display.c — ST7789 LCD driver + DOOM-framebuffer blit.
 *
 * Pinout matches the dcsgonefirm baseline (the only known-working config
 * on this badge revision):
 *     SCK = GPIO 5  ;  MOSI = GPIO 11
 *     CS  = GPIO 4  ;  DC   = GPIO 3   (NOT GPIO 2 — schematic mislabeled)
 *     RST = -1 (RC reset on board)
 * Init sequence: panel_reset → panel_init → disp_on_off → invert_color →
 * swap_xy(true) → mirror(false, true). Same as the badge's tjpgd config.
 */
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"   /* for DMA_ATTR */

/* doomgeneric in CMAP256 mode keeps the active palette in this 256-entry
 * BGRA bitfield array (i_video.h::struct color). When DOOM calls
 * I_SetPalette() the array is updated and `palette_changed` is set true.
 * We watch the flag and rebuild our packed RGB565 LUT lazily. */
#include "doomtype.h"   /* boolean typedef */
#include "i_video.h"
extern struct color colors[256];
extern boolean palette_changed;

#define PIN_SCK   5
#define PIN_MOSI  11
#define PIN_CS    4
#define PIN_DC    3
#define PIN_RST   -1

#define LCD_HOST  SPI2_HOST
/* 45 MHz — empirically the highest clock this badge's FPC reliably
 * tolerates. 60 MHz showed corruption. At 45 MHz the 128 KB blit takes
 * ~23 ms, leaving DOOM ~10 ms per tick at 30 FPS — workable for light
 * scenes; heavy scenes will drop to 15-20 FPS. */
#define PCLK_HZ   (45 * 1000 * 1000)

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static bool                      s_inited;

#define DOOM_W 320
#define DOOM_H 200
#define DOOM_PIXELS (DOOM_W * DOOM_H)

/* Per-chunk RGB565 staging buffer. 8 rows × 320 px × 2 B = 5 KB.
 * Was 16 rows / 10 KB; halved to give the DOOM zone ~5 KiB more
 * contig — the difference between mobjs spawning successfully and
 * Z_Malloc'ing 180 bytes too late. The blit just runs ~25 chunks
 * per frame instead of ~13; per-chunk SPI overhead is negligible
 * compared to the cache-miss cost of the inner conversion loop. */
#define CHUNK_ROWS 8
#define CHUNK_PIXELS (DOOM_W * CHUNK_ROWS)
static uint16_t *s_chunk565;

static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b)
{
    /* Pack to RGB565. Big-endian on the wire (high byte first) so the
     * SPI peripheral can blit the buffer raw. */
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}

/* RGB565-packed mirror of doomgeneric's colors[256]. Rebuilt lazily
 * (only when palette_changed flips true) so the per-pixel inner loop
 * stays a single LUT lookup — no math per pixel. */
static uint16_t s_palette_565[256];

static void rebuild_palette_lut(void)
{
    for (int i = 0; i < 256; i++) {
        /* colors[i] is BGRA bitfield order (per i_video.h::struct color). */
        s_palette_565[i] = rgb565_be(colors[i].r, colors[i].g, colors[i].b);
    }
    palette_changed = 0;
}

/* Forward decl — definition is below display_init() but display_init()
 * calls it during init. */
static void clear_letterbox_margins(void);

/* Pre-allocate the 10 KiB DMA-capable chunk buffer. Called from
 * app_main BEFORE doom_zone_preinit() — see main.c for ordering
 * rationale. Idempotent: a second call returns OK without re-malloc. */
esp_err_t display_preinit(void)
{
    if (s_chunk565) return ESP_OK;
    s_chunk565 = heap_caps_malloc(CHUNK_PIXELS * sizeof(uint16_t),
                                  MALLOC_CAP_DMA);
    if (!s_chunk565) {
        ESP_LOGE(TAG, "display_preinit: out of DMA RAM (%u bytes)",
                 (unsigned)(CHUNK_PIXELS * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t display_init(void)
{
    if (s_inited) return ESP_OK;

    /* Chunk buffer should already be allocated by display_preinit(). */
    if (!s_chunk565) {
        esp_err_t err = display_preinit();
        if (err != ESP_OK) return err;
    }

    /* Seed with a grey ramp so the screen isn't garbage before DOOM's
     * first I_SetPalette call. */
    for (int i = 0; i < 256; i++) {
        s_palette_565[i] = rgb565_be(i, i, i);
    }
    palette_changed = 1;   /* force a rebuild on the very first blit */

    spi_bus_config_t bus = {
        .sclk_io_num     = PIN_SCK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* One 16-row chunk per DMA transaction. */
        .max_transfer_sz = CHUNK_PIXELS * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = PCLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));

    s_inited = true;
    /* Pre-clear the letterbox bars so we never have to redraw them. */
    clear_letterbox_margins();

    ESP_LOGI(TAG, "init OK: %dx%d (DOOM 320x200 letterboxed) @ %d MHz",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, PCLK_HZ / 1000000);
    return ESP_OK;
}

/* Letterbox margins are written ONCE in display_init via this helper.
 * top_margin is 20 rows on a 320×240 panel — drawn as one CHUNK_ROWS-row
 * full-black blit plus a 4-row tail blit, top and bottom. */
static void clear_letterbox_margins(void)
{
    const int top_margin = (DISPLAY_HEIGHT - DOOM_H) / 2;  /* 20 */
    memset(s_chunk565, 0, CHUNK_PIXELS * 2);
    /* Top bar (20 rows = 16 + 4). */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                              DISPLAY_WIDTH, CHUNK_ROWS, s_chunk565);
    esp_lcd_panel_draw_bitmap(s_panel, 0, CHUNK_ROWS,
                              DISPLAY_WIDTH, top_margin, s_chunk565);
    /* Bottom bar. */
    int bot_y = DISPLAY_HEIGHT - top_margin;
    esp_lcd_panel_draw_bitmap(s_panel, 0, bot_y,
                              DISPLAY_WIDTH, bot_y + CHUNK_ROWS, s_chunk565);
    esp_lcd_panel_draw_bitmap(s_panel, 0, bot_y + CHUNK_ROWS,
                              DISPLAY_WIDTH, DISPLAY_HEIGHT, s_chunk565);
}

void display_blit_doom_frame(const uint8_t *fb320x200)
{
    if (!s_inited || !fb320x200) return;

    /* If DOOM updated the palette since the last frame, rebuild our LUT
     * before the per-pixel conversion — keeps the inner loop a flat
     * lookup without per-pixel math or condition. */
    if (palette_changed) rebuild_palette_lut();

    /* Convert + blit one CHUNK_ROWS-row chunk at a time. 200 rows / 16 rows
     * per chunk = 12 full chunks + 1 tail of 8 rows. */
    const int top_margin = (DISPLAY_HEIGHT - DOOM_H) / 2;

    for (int y = 0; y < DOOM_H; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= DOOM_H) ? CHUNK_ROWS : (DOOM_H - y);
        const uint8_t *src = fb320x200 + (size_t)y * DOOM_W;
        uint16_t      *dst = s_chunk565;

        /* 8-pixel unroll on the inner loop. `rows * DOOM_W` is divisible
         * by 8 for any rows >= 1 since DOOM_W = 320. */
        int n = (rows * DOOM_W) / 8;
        while (n--) {
            dst[0] = s_palette_565[src[0]];
            dst[1] = s_palette_565[src[1]];
            dst[2] = s_palette_565[src[2]];
            dst[3] = s_palette_565[src[3]];
            dst[4] = s_palette_565[src[4]];
            dst[5] = s_palette_565[src[5]];
            dst[6] = s_palette_565[src[6]];
            dst[7] = s_palette_565[src[7]];
            src += 8;
            dst += 8;
        }

        esp_lcd_panel_draw_bitmap(s_panel,
                                  0, top_margin + y,
                                  DOOM_W, top_margin + y + rows,
                                  s_chunk565);
    }
}
