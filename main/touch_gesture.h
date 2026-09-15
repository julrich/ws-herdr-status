/* Touch gesture recognition.
 *
 * Pure C by design — no esp_*, no LVGL — so tools/gesture_test compiles this file
 * unmodified on the host (AGENTS.md §9a) and the recognition rules can be
 * exercised without a finger.
 *
 * Why not use LVGL's own gesture support: LVGL 8.4 only offers a one-finger
 * swipe *direction* (`lv_indev_get_gesture_dir`), and the esp_lvgl_port touch
 * indev feeds LVGL a single point (`data->point.x = touchpad_x[0]`), so two
 * fingers never reach it. The touch controller reports two points and
 * esp_lcd_touch will hand them over, so the device reads the touch handle itself
 * (main/ui_input.c) and recognises gestures here.
 *
 * The model is one "touch session": it starts when a finger arrives, tracks how
 * long it lasts, how far each finger *slot* travels from where that slot first
 * appeared, and the most fingers seen at once; on release it classifies the
 * session as a tap, a swipe or nothing.
 *
 * Movement is per slot, not per mean position, because fingers do not lift
 * together: on a two-finger tap one usually leaves a few tens of milliseconds
 * before the other, and the midpoint of the two jumps by half the finger
 * separation the moment that happens — enough to make every two-finger tap look
 * like a 40 px drag. Each slot keeps its own origin.
 *
 * Slot identity is not stable across a change in the reported finger count
 * either: when one of two fingers lifts, the survivor is reported in slot 0 even
 * if it started in slot 1 (measured with the synthetic case in
 * tools/gesture_test). So whenever the count changes, the origins of the slots
 * still down are re-latched to where they are now, while the travel accumulated
 * so far is kept. A one-finger swipe never changes count, so its travel is
 * unaffected; a staggered two-finger tap stops looking like a drag.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TG_NONE = 0,
    TG_TAP,        /* one finger, brief, little travel */
    TG_TAP2,       /* two fingers, brief — toggles the diagnostics overlay */
    TG_SWIPE_LEFT, /* one finger, dominant horizontal travel — next view */
    TG_SWIPE_RIGHT,
    TG_SWIPE_UP,   /* one finger, dominant vertical travel — page the agent list */
    TG_SWIPE_DOWN,
} tg_event_t;

typedef struct {
    uint32_t tap_max_ms;   /* longest press still a tap */
    int32_t  tap_max_px;   /* most travel still a tap */
    int32_t  swipe_min_px; /* least travel that counts as a swipe */
} tg_cfg_t;

typedef struct {
    tg_cfg_t cfg;

    bool     active;      /* a finger is (or was) down */
    uint8_t  max_points;  /* most fingers seen in this session */
    uint8_t  last_points; /* count reported by the previous sample */
    uint32_t held_ms;     /* how long the session lasted */

    /* Per finger slot: where it first appeared and how far it has travelled. */
    struct {
        bool    present;
        int32_t start_x, start_y;
        int32_t last_x, last_y;
        int32_t moved_px;
    } slot[2];
} tg_t;

/* The default thresholds: a tap is under 600 ms and 12 px, which leaves room for
 * a deliberate, unhurried press; a swipe needs 30 px, which is well past the
 * wobble of a tap on a 172x320 panel. 600/12/30 were checked against the same
 * synthetic cases the host test uses. */
void tg_init(tg_t *t);

/* Feed one touch sample and get an event when a gesture completes.
 *
 * `points` is 0, 1 or 2; `x1`/`y1` are only read when two fingers are down.
 * Coordinates are in screen pixels, `dt_ms` is the time since the previous
 * sample (clamped internally to at least 1 ms). Returns TG_NONE unless the
 * release that ends a session is classified as a gesture.
 */
tg_event_t tg_feed(tg_t *t, int points, int x0, int y0, int x1, int y1, uint32_t dt_ms);

/* "none"/"tap"/"tap2"/"swipe_left"/… — for logs and test output. */
const char *tg_event_name(tg_event_t e);

/* True while a finger is down (the caller may want a faster poll for tracking). */
bool tg_active(const tg_t *t);
