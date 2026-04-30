/*
 * led_feedback.c — WS2812B ring driver + DOOM state → LED reflection.
 *
 * Drives the 16-LED ring on GPIO 8 via the espressif/led_strip managed
 * component (RMT-backed, no extra dependencies needed beyond what we
 * already pull). Polls doomgeneric's `players[0]` once per frame from
 * led_feedback_tick() and animates accordingly.
 *
 * Player struct access — doomgeneric exports the full DOOM source, which
 * declares `extern player_t players[MAXPLAYERS]` in d_player.h. We
 * include doomdef.h + d_player.h to get the type, then extern the array.
 */
#include "led_feedback.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "doomdef.h"
#include "d_player.h"
#include "doomstat.h"   /* declares `players[]` and `gamestate` */

static const char *TAG = "led_feedback";

#define LED_GPIO   8
#define NUM_LEDS   16
#define DOOM_AMMO_TYPES 4    /* am_clip, am_shell, am_cell, am_misl */

static led_strip_handle_t s_strip;

/* State diff trackers */
static int  s_prev_health     = 100;
static int  s_prev_armor      = 0;
static int  s_prev_ammo[DOOM_AMMO_TYPES] = {0};
static bool s_initialized_diff = false;

/* One-shot pulse state — when triggered, drives the ring for `dur_ms` ms. */
typedef struct {
    int64_t start_us;
    int64_t dur_ms;
    uint8_t r, g, b;
} pulse_t;
static pulse_t s_pulse = {0};

static inline void set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NUM_LEDS; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
}

static void trigger_pulse(uint8_t r, uint8_t g, uint8_t b, int dur_ms)
{
    s_pulse.start_us = esp_timer_get_time();
    s_pulse.dur_ms   = dur_ms;
    s_pulse.r = r; s_pulse.g = g; s_pulse.b = b;
}

static bool render_pulse(void)
{
    if (s_pulse.dur_ms == 0) return false;
    int64_t elapsed_ms = (esp_timer_get_time() - s_pulse.start_us) / 1000;
    if (elapsed_ms >= s_pulse.dur_ms) {
        s_pulse.dur_ms = 0;
        return false;
    }
    /* Linear fade from full → 0 over dur_ms. */
    int frac = (int)(255 - (elapsed_ms * 255 / s_pulse.dur_ms));
    if (frac < 0) frac = 0;
    set_all((s_pulse.r * frac) >> 8,
            (s_pulse.g * frac) >> 8,
            (s_pulse.b * frac) >> 8);
    return true;
}

static void render_health_idle(int health)
{
    /* Map 0-100 health to red→yellow→green. */
    if (health < 0)   health = 0;
    if (health > 100) health = 100;
    uint8_t r, g, b = 0;
    if (health >= 50) {
        /* green->yellow as health drops from 100 to 50 */
        int t = (100 - health) * 255 / 50;
        r = t; g = 200; b = 0;
    } else {
        /* yellow->red as health drops from 50 to 0 */
        int t = (50 - health) * 255 / 50;
        r = 200; g = 200 - t; b = 0;
    }
    /* Dim it a fair bit so idle ring isn't blinding. */
    set_all(r >> 3, g >> 3, b >> 3);
}

static void render_dying(void)
{
    /* Slow red breathing — sin-ish via simple sawtooth. */
    int64_t ms = esp_timer_get_time() / 1000;
    int phase = (ms / 4) % 256;     /* 0..255 over ~1024 ms */
    if (phase > 127) phase = 255 - phase;  /* up then down */
    uint8_t r = phase;
    set_all(r, 0, 0);
}

static void render_level_complete(void)
{
    /* Rainbow chase. */
    int64_t ms = esp_timer_get_time() / 1000;
    int rotation = (ms / 50) % NUM_LEDS;
    for (int i = 0; i < NUM_LEDS; i++) {
        int hue = ((i + rotation) * 256 / NUM_LEDS) & 0xFF;
        /* Crude HSV→RGB at full saturation/value */
        uint8_t r = 0, g = 0, b = 0;
        int region = hue / 43;       /* 6 regions, 256/6 ≈ 43 */
        int rem    = (hue - region * 43) * 6;
        switch (region) {
            case 0: r = 255;       g = rem;       b = 0;         break;
            case 1: r = 255 - rem; g = 255;       b = 0;         break;
            case 2: r = 0;         g = 255;       b = rem;       break;
            case 3: r = 0;         g = 255 - rem; b = 255;       break;
            case 4: r = rem;       g = 0;         b = 255;       break;
            default:r = 255;       g = 0;         b = 255 - rem; break;
        }
        led_strip_set_pixel(s_strip, i, r >> 1, g >> 1, b >> 1);
    }
}

void led_feedback_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = NUM_LEDS,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz, standard for WS2812B */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "LED ring init OK (GPIO %d, %d LEDs)", LED_GPIO, NUM_LEDS);
}

void led_feedback_tick(void)
{
    if (!s_strip) return;

    player_t *p = &players[0];

    /* Initialize the diff trackers on the very first tick after DOOM has
     * populated the player struct, so we don't immediately fire pulses
     * for "everything just appeared from 0". */
    if (!s_initialized_diff) {
        s_prev_health = p->health;
        s_prev_armor  = p->armorpoints;
        for (int i = 0; i < DOOM_AMMO_TYPES; i++) s_prev_ammo[i] = p->ammo[i];
        s_initialized_diff = true;
    }

    /* Detect changes and trigger pulses. Damage > pickup > weapon-fire
     * priority order — most recent pulse wins anyway. */
    int hp_delta = p->health - s_prev_health;
    if (hp_delta < 0) {
        trigger_pulse(255, 0, 0, 250);          /* damage flash */
    } else if (hp_delta > 0) {
        trigger_pulse(0, 200, 0, 350);          /* heal */
    }
    s_prev_health = p->health;

    int armor_delta = p->armorpoints - s_prev_armor;
    if (armor_delta > 0) {
        trigger_pulse(0, 100, 255, 350);        /* armor pickup */
    }
    s_prev_armor = p->armorpoints;

    for (int i = 0; i < DOOM_AMMO_TYPES; i++) {
        int delta = p->ammo[i] - s_prev_ammo[i];
        if (delta < 0) {
            trigger_pulse(255, 220, 200, 80);   /* fired = quick flash */
        }
        /* Pickup of ammo also rises s_prev_ammo without a special pulse;
         * health/armor green/blue are enough. */
        s_prev_ammo[i] = p->ammo[i];
    }

    /* Render priority: gamestate special > active pulse > idle ring. */
    if (gamestate == GS_INTERMISSION || gamestate == GS_FINALE) {
        render_level_complete();
    } else if (p->health <= 0) {
        render_dying();
    } else if (!render_pulse()) {
        render_health_idle(p->health);
    }
    led_strip_refresh(s_strip);
}
