/*
 * main.c — ESP-IDF entry point for doomgeneric on the DEFCON SG 1 badge.
 *
 * Responsibilities:
 *   1. Mount SPIFFS at /spiffs (so DOOM can fopen("/spiffs/DOOM1.WAD")).
 *   2. Bring up the ST7789 LCD and the four buttons.
 *   3. Hand off to doomgeneric: doomgeneric_Create() then doomgeneric_Tick()
 *      forever.
 *
 * The four DG_* platform hooks DOOM calls into are implemented in
 * doomgeneric_esp32c6.c; this file just bootstraps and runs the tick loop.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "doomgeneric.h"
#include "display.h"
#include "buttons.h"
#include "led_feedback.h"

/* DEFCON SG 1 badge has a vibration motor on GPIO 22, gated by a
 * P-channel MOSFET (Q1, DMP3056L-7) with the gate pulled to +3V3
 * via 10K (R2) and driven through R3=100Ω from the GPIO. P-channel
 * MOSFETs turn ON when the gate is LOW and OFF when the gate is HIGH.
 * So to keep the motor SILENT we must drive GPIO 22 HIGH (or leave
 * it as input and let the 10K pullup do the job). We drive it HIGH
 * explicitly so even a brief output-low glitch from another caller
 * doesn't kick the motor.
 *
 * This must run as the first thing app_main does, before any logging
 * or other init, so a crash later in boot leaves the motor inert. */
#define GPIO_VIBRA_MOTOR 22

static void vibration_off(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_VIBRA_MOTOR,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* belt-and-suspenders */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_VIBRA_MOTOR, 1);   /* HIGH = MOSFET off = motor off */
}

/* One-shot timer that drives the gate HIGH (motor off) when it fires.
 * vibration_pulse_ms() drives the gate LOW (motor on) and re-arms this
 * timer for `ms` milliseconds. Using esp_timer (FreeRTOS-friendly,
 * runs in a high-priority task) instead of a delay+set sequence so a
 * burst of FIRE events doesn't pile up on the calling task's stack. */
static esp_timer_handle_t s_vibra_off_timer;

static void vibra_off_cb(void *arg)
{
    (void)arg;
    gpio_set_level(GPIO_VIBRA_MOTOR, 1);   /* HIGH = motor off */
}

void vibration_pulse_ms(int ms)
{
    if (ms <= 0 || !s_vibra_off_timer) return;
    /* Restart on every call — a rapid burst of fires extends the buzz
     * instead of stuttering it. */
    esp_timer_stop(s_vibra_off_timer);
    gpio_set_level(GPIO_VIBRA_MOTOR, 0);   /* LOW = motor on */
    esp_timer_start_once(s_vibra_off_timer, (uint64_t)ms * 1000);
}

static void vibration_init_timer(void)
{
    const esp_timer_create_args_t args = {
        .callback = vibra_off_cb,
        .name     = "vibra_off",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_vibra_off_timer));
}

static const char *TAG = "doom_main";

static void check_wad_partition(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "wad");
    if (!p) {
        ESP_LOGE(TAG, "'wad' partition not found in flash. Build will boot");
        ESP_LOGE(TAG, "but DOOM init will fail. Run tools/flash_wad.sh to");
        ESP_LOGE(TAG, "write your DOOM1.WAD into the wad partition.");
        return;
    }
    ESP_LOGI(TAG, "'wad' partition: %u KiB at offset 0x%06lx",
             (unsigned)(p->size / 1024), (unsigned long)p->address);
}

void app_main(void)
{
    /* Silence the vibration motor BEFORE any logging or other init —
     * if anything below crashes we want the buzz to stop, not start. */
    vibration_off();
    vibration_init_timer();

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, " DEFCON SG 1 — DOOM ");
    ESP_LOGI(TAG, "══════════════════════════════════════");

    /* nvs_flash_init() removed — we never call nvs_get/nvs_set, so the
     * NVS subsystem (and its ~10 KiB of code) is dead weight. The nvs
     * partition stays in partitions.csv for forward compat. */
    check_wad_partition();

    /* Memory ordering for the heap-pressured boot:
     *
     *   1. display_preinit()  — grab the 10 KiB DMA-capable chunk
     *                           buffer FIRST, while the heap still
     *                           has clean DMA-capable contig blocks.
     *                           If we did this AFTER Z_Init the zone
     *                           would have eaten the largest contig
     *                           and the DMA malloc would fail.
     *
     *   2. doom_zone_preinit()— grab the DOOM zone next. Idempotent;
     *                           D_DoomMain's later Z_Init() picks up
     *                           the same block via __wrap_I_ZoneBase.
     *                           This claims the largest remaining
     *                           contig before SPI/button/LED drivers
     *                           pepper the heap with smaller allocs.
     *
     *   3. display_init() / buttons_init() / led_feedback_init() —
     *                           SPI bus + panel + button + LED RMT
     *                           setup. These do small heap allocs
     *                           that fit easily in the post-zone
     *                           fragments (~14 KiB largest contig).
     */
    ESP_ERROR_CHECK(display_preinit());

    extern void doom_zone_preinit(void);
    doom_zone_preinit();

    ESP_ERROR_CHECK(display_init());
    buttons_init();
    led_feedback_init();

    /* doomgeneric expects argv[0] / argv[1+] for command-line flags. The
     * `-iwad` path is ignored — our wrapped W_OpenFile uses the flash
     * mmap regardless. We pass it anyway so DOOM's auto-detect doesn't
     * scan random directories. */
    static char arg0[]      = "doom";
    static char arg_iwad[]  = "-iwad";
    static char arg_path[]  = "DOOM1.WAD";
    char *argv[] = { arg0, arg_iwad, arg_path };
    int   argc   = 3;

    ESP_LOGI(TAG, "calling doomgeneric_Create(argc=%d)", argc);
    doomgeneric_Create(argc, argv);

    ESP_LOGI(TAG, "entering tick loop");
    for (;;) {
        doomgeneric_Tick();
    }
}
