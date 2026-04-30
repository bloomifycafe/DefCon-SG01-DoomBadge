#pragma once
/*
 * led_feedback.h — drives the 16-LED WS2812B ring on GPIO 8 to reflect
 * what's happening in DOOM. Hooked from DG_DrawFrame: every frame we
 * read player[0]'s health/armor/ammo/damagecount and emit a coloured
 * pulse on the ring whenever something changes.
 */
#include <stdint.h>

void led_feedback_init(void);

/* Called once per DOOM tick from DG_DrawFrame. Reads doomgeneric's
 * `players[0]` global, diffs against the previous frame, and triggers
 * coloured pulses on the LED ring:
 *   - took damage (health dropped)        → quick RED flash
 *   - picked up health                    → soft GREEN pulse
 *   - picked up armor                     → soft BLUE pulse
 *   - fired weapon (ammo dropped)         → fast WHITE flash
 *   - died (health <= 0)                  → solid dim red breathing
 *   - level complete (intermission scrn)  → bright rainbow chase
 *   - default (alive, idle)               → ring colour scaled by health
 *                                           (green→yellow→red as you bleed)
 */
void led_feedback_tick(void);
