/*
 * buttons.c — 4 hardware buttons → DOOM key events.
 *
 * Mapping per user request:
 *     A (GPIO 23) = TURN LEFT   (KEY_LEFTARROW)
 *     B (GPIO 15) = MOVE FORWARD (KEY_UPARROW)
 *     C (GPIO 10) = TURN RIGHT  (KEY_RIGHTARROW)
 *     D (GPIO  9) = FIRE        (KEY_FIRE)
 *
 * Long-press of D doubles as USE (opens doors, presses switches) since
 * we don't have a fifth button. Long-press of B is ENTER (menu select).
 * Long-press of A is ESCAPE (menu open/back).
 */
#include "buttons.h"
#include "doomkeys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "buttons";

#define KEYQUEUE_LEN 32

typedef struct {
    int pressed;          /* 1 = down, 0 = up */
    unsigned char doomkey;
} key_event_t;

static QueueHandle_t s_queue;

typedef struct {
    const char   *name;
    int           gpio;
    unsigned char doomkey;
} btn_def_t;

static const btn_def_t BTN_DEFS[] = {
    { "A", 23, KEY_LEFTARROW  },  /* turn left */
    { "B", 15, KEY_UPARROW    },  /* move forward */
    { "C", 10, KEY_RIGHTARROW },  /* turn right */
    { "D",  9, KEY_FIRE       },  /* fire */
};
#define NUM_BTNS (sizeof(BTN_DEFS) / sizeof(BTN_DEFS[0]))

/* Long-press secondaries: each button gets a second function on hold,
 * triggered by BUTTON_LONG_PRESS_START. They behave as a single-shot
 * (press+release pair, 50 ms apart) so DOOM's input pipeline registers
 * a clean tap. */
typedef struct {
    int           gpio;
    unsigned char doomkey;
} btn_long_def_t;

static const btn_long_def_t BTN_LONG_DEFS[] = {
    /* A long was ESCAPE but it kept firing during sustained turn-left
     * gameplay. ESCAPE is now the A+C chord (see CHORD_* below) — that
     * combo is impossible during normal play (you can't turn left and
     * right simultaneously). C long stays for ENTER because pause-menu
     * confirmation while turning right is rare enough not to misfire. */
    { 10, KEY_ENTER  },     /* C long  = menu select */
    /* D long is no longer USE — held-D rapid-fires the gun, so the
     * long-press would unintentionally trigger USE in the middle of
     * combat. USE is now B double-click (see BTN_DOUBLE_DEFS). */
};
#define NUM_BTN_LONGS (sizeof(BTN_LONG_DEFS) / sizeof(BTN_LONG_DEFS[0]))

/* Double-click secondaries — same shape as long-press but fires when the
 * iot_button library detects two short presses within its double-click
 * window (default ~300 ms). The button's normal press_down/press_up
 * callbacks still fire on each tap, so a double-tap on Forward gives a
 * brief two-step forward shuffle followed by USE — natural for "walk
 * up to a door and tap-tap to open it" without taking a finger off the
 * movement button. */
typedef struct {
    int           gpio;
    unsigned char doomkey;
} btn_double_def_t;

static const btn_double_def_t BTN_DOUBLE_DEFS[] = {
    { 15, KEY_USE },        /* B double-click = use (primary) */
};
#define NUM_BTN_DOUBLES (sizeof(BTN_DOUBLE_DEFS) / sizeof(BTN_DOUBLE_DEFS[0]))

static void enqueue(int pressed, unsigned char dkey)
{
    if (!s_queue) return;
    key_event_t ev = { .pressed = pressed, .doomkey = dkey };
    /* Drop the event if the queue is full — DOOM's input pipeline is
     * tolerant of a missed key and we'd rather not block. */
    xQueueSend(s_queue, &ev, 0);
}

/* Synthetic key taps (long-press → ENTER, double-click → USE) need the
 * press and release to land in DIFFERENT DOOM tics, otherwise
 * D_ProcessEvents runs both in the same tick and gamekeydown[key] is
 * already false by the time G_BuildTiccmd checks it — meaning USE never
 * sets BT_USE, doors never open. We solve this by enqueuing the press
 * immediately and deferring the release ~50 ms (~2 tics @ 35 Hz) via a
 * one-shot esp_timer. The 50 ms feels instant to the player but spans
 * a tic boundary so DOOM observes the key as held for one full tic. */
static esp_timer_handle_t s_defer_release_timer;
static unsigned char       s_defer_release_key;

static void defer_release_cb(void *arg)
{
    (void)arg;
    enqueue(0, s_defer_release_key);
}

static void tap_with_defer(unsigned char dkey)
{
    if (!s_defer_release_timer) {
        const esp_timer_create_args_t args = {
            .callback = defer_release_cb,
            .name     = "btn_defer_rel",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_defer_release_timer));
    }
    esp_timer_stop(s_defer_release_timer);
    s_defer_release_key = dkey;
    enqueue(1, dkey);
    esp_timer_start_once(s_defer_release_timer, 50 * 1000);  /* 50 ms */
}

/* Defined in main.c — drives the vibration motor on for `ms`. */
extern void vibration_pulse_ms(int ms);

/* A+C chord state. ESCAPE fires once when both turn buttons go down
 * together; we latch s_chord_fired until both are released so a player
 * holding the combo doesn't open/close the menu repeatedly. The
 * individual LEFT/RIGHT key events still flow to DOOM during the chord
 * (so the player gets a brief turn-left-then-turn-right wiggle when
 * triggering ESCAPE), which is a fine cost for a chord that's
 * impossible to hit accidentally during normal play. */
#define GPIO_A 23
#define GPIO_C 10
static bool s_a_held;
static bool s_c_held;
static bool s_chord_fired;

static void on_press_down(void *arg, void *user)
{
    (void)arg;
    const btn_def_t *def = (const btn_def_t *)user;
    ESP_LOGD(TAG, "DOWN %s -> 0x%02X", def->name, def->doomkey);
    enqueue(1, def->doomkey);

    /* Track A/C held state and fire ESCAPE on chord entry. */
    if (def->gpio == GPIO_A) s_a_held = true;
    if (def->gpio == GPIO_C) s_c_held = true;
    if (s_a_held && s_c_held && !s_chord_fired) {
        s_chord_fired = true;
        ESP_LOGW(TAG, "CHORD A+C -> ESCAPE");
        tap_with_defer(KEY_ESCAPE);
    }

    /* Haptic feedback on every gun fire. 70 ms is short enough to feel
     * like a discrete tap; the motor's spin-up is ~20 ms so anything
     * shorter is barely noticeable. Re-firing extends the buzz. */
    if (def->doomkey == KEY_FIRE) {
        vibration_pulse_ms(70);
    }
}

static void on_press_up(void *arg, void *user)
{
    (void)arg;
    const btn_def_t *def = (const btn_def_t *)user;
    ESP_LOGD(TAG, "UP   %s", def->name);
    enqueue(0, def->doomkey);
    if (def->gpio == GPIO_A) s_a_held = false;
    if (def->gpio == GPIO_C) s_c_held = false;
    if (!s_a_held && !s_c_held) s_chord_fired = false;   /* re-arm */
}

/* Long-press fires both a press+release for the secondary doomkey so the
 * DOOM input handler treats it as a single tap. Tiny gap between events
 * stops the engine from coalescing them. */
static void on_long_press(void *arg, void *user)
{
    (void)arg;
    const btn_long_def_t *def = (const btn_long_def_t *)user;
    ESP_LOGW(TAG, "LONG GPIO %d -> 0x%02X", def->gpio, def->doomkey);
    tap_with_defer(def->doomkey);
}

/* Double-click handler. Uses tap_with_defer() so the release lands in
 * a later DOOM tic — without that delay G_BuildTiccmd never sees
 * gamekeydown[KEY_USE]==true and doors don't open. (Menu navigation
 * still worked with the old back-to-back enqueue because menu input
 * goes through M_Responder, which handles events as they arrive
 * instead of latching state at tic boundaries — that's why pause/menu
 * was fine but in-game USE was silently dropped.) */
static void on_double_click(void *arg, void *user)
{
    (void)arg;
    const btn_double_def_t *def = (const btn_double_def_t *)user;
    ESP_LOGW(TAG, "DOUBLE GPIO %d -> 0x%02X (USE)",
             def->gpio, def->doomkey);
    tap_with_defer(def->doomkey);
}

void buttons_init(void)
{
    s_queue = xQueueCreate(KEYQUEUE_LEN, sizeof(key_event_t));

    /* short_press_time governs both the click-vs-long-press boundary AND
     * the inter-tap window for double-click detection. Default 180 ms is
     * tight; 400 ms gives users a comfortable cadence to double-tap with
     * a gloved finger and still fires long-press cleanly within 1500 ms. */
    const button_config_t btn_cfg = { .short_press_time = 400 };

    for (size_t i = 0; i < NUM_BTNS; i++) {
        const btn_def_t *def = &BTN_DEFS[i];
        const button_gpio_config_t gpio_cfg = {
            .gpio_num     = def->gpio,
            .active_level = 0,
        };
        button_handle_t btn = NULL;
        ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
        ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL,
                                               on_press_down, (void *)def));
        ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL,
                                               on_press_up, (void *)def));
        /* Wire any matching long-press secondary. */
        for (size_t j = 0; j < NUM_BTN_LONGS; j++) {
            if (BTN_LONG_DEFS[j].gpio == def->gpio) {
                ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START,
                                                       NULL, on_long_press,
                                                       (void *)&BTN_LONG_DEFS[j]));
                break;
            }
        }
        /* Wire any matching double-click secondary. */
        for (size_t j = 0; j < NUM_BTN_DOUBLES; j++) {
            if (BTN_DOUBLE_DEFS[j].gpio == def->gpio) {
                ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK,
                                                       NULL, on_double_click,
                                                       (void *)&BTN_DOUBLE_DEFS[j]));
                break;
            }
        }
        ESP_LOGI(TAG, "Button %s on GPIO %d → doomkey 0x%02X",
                 def->name, def->gpio, def->doomkey);
    }
}

int buttons_pop_doomkey(int *pressed, unsigned char *doomKey)
{
    if (!s_queue) return 0;
    key_event_t ev;
    if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) return 0;
    *pressed = ev.pressed;
    *doomKey = ev.doomkey;
    return 1;
}
