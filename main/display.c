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
#include "freertos/semphr.h"
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

/* DMA-completion semaphore. Each blit submits one transaction and then
 * takes the semaphore — the on_color_trans_done callback gives it from
 * the SPI ISR after DMA finishes. By the time submit_blit() returns the
 * panel has finished reading s_chunk565 so the next iteration's pixel
 * conversion is safe to overwrite it. Without this the loop body races
 * the DMA on the shared buffer; doom's textured frames mostly mask the
 * resulting per-row tearing but it's there. */
static SemaphoreHandle_t s_dma_done;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel_io; (void)edata; (void)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_done, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* Synchronous blit helper: submit, then block until DMA done. */
static inline void submit_blit(int x_start, int y_start, int x_end, int y_end,
                               const void *pixels)
{
    esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, pixels);
    xSemaphoreTake(s_dma_done, portMAX_DELAY);
}

/* Extra ST7789 init commands (Bodmer/TFT_eSPI JLX240 reference) for the
 * registers IDF's built-in init leaves at silicon factory defaults: porch,
 * gate, VCOM, power, gamma. Pinning these eliminates the per-unit factory
 * NVM lottery on this panel class and is the single biggest win for
 * static-frame stability. */
static void apply_st7789_quality_init(void)
{
#define TX(cmd, ...) do { \
    static const uint8_t _d[] = {__VA_ARGS__}; \
    esp_lcd_panel_io_tx_param(s_io, (cmd), _d, sizeof(_d)); \
} while (0)
    TX(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
    TX(0xB7, 0x35);
    TX(0xBB, 0x28);
    TX(0xC0, 0x0C);
    TX(0xC2, 0x01, 0xFF);
    TX(0xC3, 0x10);
    TX(0xC4, 0x20);
    TX(0xC6, 0x0F);
    TX(0xD0, 0xA4, 0xA1);
    TX(0xE0, 0xD0,0x00,0x02,0x07,0x0A,0x28,0x32,0x44,0x42,0x06,0x0E,0x12,0x14,0x17);
    TX(0xE1, 0xD0,0x00,0x02,0x07,0x0A,0x28,0x31,0x54,0x47,0x0E,0x1C,0x17,0x1B,0x1E);
    /* RAMCTRL: explicit big-endian on the wire so our pre-byte-swapped
     * RGB565 buffers blit raw without color corruption. Defends against
     * IDF GH#11416 where the default value occasionally lands wrong. */
    TX(0xB0, 0x00, 0xE0);
#undef TX
}

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

/* CRT scanline effect: every other panel row uses the dimmed LUT so the
 * frame has alternating bright/dim horizontal lines. Side benefits beyond
 * the retro look: visually masks any per-row SPI tearing (since alternate
 * rows are *expected* to differ in brightness), and makes residual
 * panel-level pixel noise blend into the natural row variation.
 *
 * SCANLINE_BRIGHT_NUM/256 is the dim factor — 192/256 = 75% (subtle, more
 * playable), 160/256 = 62%, 128/256 = 50% (full CRT vibe but darker). */
#define SCANLINE_BRIGHT_NUM 192

static uint16_t s_palette_565[256];        /* bright row */
static uint16_t s_palette_565_dim[256];    /* dim row (scanline) */

static void rebuild_palette_lut(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t r = colors[i].r, g = colors[i].g, b = colors[i].b;
        s_palette_565[i]     = rgb565_be(r, g, b);
        s_palette_565_dim[i] = rgb565_be((r * SCANLINE_BRIGHT_NUM) >> 8,
                                         (g * SCANLINE_BRIGHT_NUM) >> 8,
                                         (b * SCANLINE_BRIGHT_NUM) >> 8);
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

    /* Seed both LUTs with a grey ramp so the screen isn't garbage before
     * DOOM's first I_SetPalette call. */
    for (int i = 0; i < 256; i++) {
        s_palette_565[i]     = rgb565_be(i, i, i);
        s_palette_565_dim[i] = rgb565_be((i * SCANLINE_BRIGHT_NUM) >> 8,
                                         (i * SCANLINE_BRIGHT_NUM) >> 8,
                                         (i * SCANLINE_BRIGHT_NUM) >> 8);
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

    /* Binary semaphore starts EMPTY. submit_blit() submits one DMA
     * transaction, then takes the semaphore — the on_color_trans_done
     * ISR gives it when the panel finishes reading the chunk. */
    s_dma_done = xSemaphoreCreateBinary();
    if (!s_dma_done) {
        ESP_LOGE(TAG, "could not create DMA-done semaphore");
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    /* FPC signal-integrity tuning: lower SCK/MOSI drive (ringing → bit
     * flips on ribbon cable), pull-ups on CS/DC. CAP_1 trades a tiny
     * speed margin for clean edges on this badge revision. Must run
     * after esp_lcd_new_panel_io_spi which (re)configures the iomux. */
    gpio_set_drive_capability(PIN_SCK,  GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability(PIN_MOSI, GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability(PIN_CS,   GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(PIN_DC,   GPIO_DRIVE_CAP_0);
    gpio_set_pull_mode(PIN_CS, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_DC, GPIO_PULLUP_ONLY);

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    apply_st7789_quality_init();
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
    submit_blit(0, 0,         DISPLAY_WIDTH, CHUNK_ROWS,  s_chunk565);
    submit_blit(0, CHUNK_ROWS, DISPLAY_WIDTH, top_margin, s_chunk565);
    /* Bottom bar. */
    int bot_y = DISPLAY_HEIGHT - top_margin;
    submit_blit(0, bot_y,                DISPLAY_WIDTH, bot_y + CHUNK_ROWS,  s_chunk565);
    submit_blit(0, bot_y + CHUNK_ROWS,   DISPLAY_WIDTH, DISPLAY_HEIGHT,      s_chunk565);
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

        /* Convert one row at a time so we can swap the LUT per row for
         * the scanline effect. The 8-pixel unroll is preserved inside
         * the row; per-row overhead is one pointer assignment. */
        for (int yy = 0; yy < rows; yy++) {
            int panel_y = top_margin + y + yy;
            const uint16_t *lut = (panel_y & 1) ? s_palette_565_dim
                                                : s_palette_565;
            const uint8_t *src  = fb320x200 + (size_t)(y + yy) * DOOM_W;
            uint16_t      *dst  = s_chunk565 + (size_t)yy * DOOM_W;

            int n = DOOM_W / 8;
            while (n--) {
                dst[0] = lut[src[0]];
                dst[1] = lut[src[1]];
                dst[2] = lut[src[2]];
                dst[3] = lut[src[3]];
                dst[4] = lut[src[4]];
                dst[5] = lut[src[5]];
                dst[6] = lut[src[6]];
                dst[7] = lut[src[7]];
                src += 8;
                dst += 8;
            }
        }

        submit_blit(0, top_margin + y,
                    DOOM_W, top_margin + y + rows,
                    s_chunk565);
    }
}
