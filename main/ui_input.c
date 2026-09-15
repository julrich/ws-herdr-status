/* Touch input: the one reader of the touch controller.
 *
 * Why this exists at all: the esp_lvgl_port touch indev asks for
 * esp_lcd_touch_get_coordinates(..., max_point_num = 1), so LVGL only ever sees
 * finger 0 — a two-finger tap never reaches it, and LVGL 8.4's own gesture
 * support is a one-finger direction on top of that. The controller reports two
 * points, so this task reads the handle itself, recognises the gesture in
 * main/touch_gesture.c and calls the companion's handlers directly.
 *
 * The port's indev is removed before the first read. It reads the same handle
 * and would compete for the same sample: the driver's get_coordinates() consumes
 * and clears the latched sample, so two readers would each see roughly half the
 * touches — and a half-seen touch is not a gesture, it is noise.
 *
 * Reads stay interrupt-gated inside the driver: its read_data() returns without
 * touching the bus unless the controller's INT line is asserted. Polling the
 * handle at 40 Hz therefore costs nothing while the panel is idle, which is what
 * keeps the IMU sharing that bus healthy (AGENTS.md §7, §11). Do not add a read
 * path that bypasses the gate.
 */

#include "ui_input.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_hw.h"
#include "esp_lvgl_port.h"
#include "touch_gesture.h"
#include "ui_companion.h"
#include "ui_stats.h"

static const char *TAG = "input";

/* 40 Hz. A deliberate swipe crosses the 172 px panel in roughly 200 ms, i.e.
 * 8 px per step — well above the recogniser's 12 px tap tolerance, so the step
 * size cannot blur a swipe into a tap. Reading faster would only add bus
 * traffic during a touch for no extra resolution. */
#define INPUT_TASK_PERIOD_MS 25
/* The recogniser keeps two slots and a few scalars, and the deepest frame is one
 * ESP_LOG; 3 KB is the same budget main/ui_rotation.c gives its task. */
#define INPUT_TASK_STACK 3072
#define INPUT_TASK_PRIO  3

/* Read by the diagnostics overlay from the LVGL task, written here. */
static herdr_input_stats_t s_stats;
static portMUX_TYPE        s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static bool s_started;

void ui_input_stats_get(herdr_input_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    /* A critical section rather than a mutex: the whole payload is a snapshot of
     * counters and the copy is a handful of instructions. */
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_publish(const herdr_input_stats_t *st)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats = *st;
    portEXIT_CRITICAL(&s_stats_lock);
}

/* One classified gesture, one companion action. The handlers build and animate
 * LVGL objects, so they run under the port lock, exactly as the calls in main.c
 * do; lvgl_port_lock(0) waits as long as it takes (0 means portMAX_DELAY). */
static void gest_handle(tg_event_t ev)
{
    lvgl_port_lock(0);

    switch (ev) {
    case TG_TAP:         ui_companion_on_tap(); break;
    case TG_TAP2:        ui_companion_on_toggle_overlay(); break;
    case TG_SWIPE_LEFT:  ui_companion_on_switch_view(-1); break;
    case TG_SWIPE_RIGHT: ui_companion_on_switch_view(+1); break;
    case TG_SWIPE_UP:    ui_companion_on_page(-1); break;
    case TG_SWIPE_DOWN:  ui_companion_on_page(+1); break;
    default:             break; /* never called with TG_NONE */
    }

    lvgl_port_unlock();
}

static void ui_input(void *arg)
{
    (void)arg;

    /* Take the touch path over from the port's indev. lvgl_port_remove_touch()
     * also frees the indev's memory, which the port's header is explicit that
     * nothing else does. */
    lv_indev_t *indev = app_hw_touch_indev();

    if (indev != NULL) {
        lvgl_port_lock(0);
        const esp_err_t err = lvgl_port_remove_touch(indev);
        lvgl_port_unlock();

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "touch indev removed — gestures are read here");
        } else {
            /* Keep going: a stale indev costs us samples, but dropping the
             * touch path entirely would cost us all of them. */
            ESP_LOGW(TAG, "lvgl_port_remove_touch failed: %s", esp_err_to_name(err));
        }
    }

    esp_lcd_touch_handle_t tp = app_hw_touch();

    tg_t tg;
    tg_init(&tg);

    herdr_input_stats_t st = { .running = true };
    stats_publish(&st); /* the overlay may already be up before the first sample */

    int64_t last_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_TASK_PERIOD_MS));

        uint16_t xs[2] = { 0, 0 };
        uint16_t ys[2] = { 0, 0 };
        uint8_t  n     = 0;

        if (esp_lcd_touch_read_data(tp) != ESP_OK) {
            /* A failed transaction is not a touch: handing the recogniser the
             * previous sample's coordinates would fabricate a press, and a press
             * that never ends is a session that never becomes a gesture. */
            st.read_errors++;
        } else {
            esp_lcd_touch_get_coordinates(tp, xs, ys, NULL, &n, 2);
        }

        /* Measure the step that actually passed, as the IMU loop does: the
         * delay is tick-quantised (25 ms lands on 20 or 30 ms at 100 Hz ticks)
         * and a slow read stretches the interval, so the recogniser's
         * millisecond budget gets real time rather than the nominal period. */
        const int64_t now_us = esp_timer_get_time();
        uint32_t      dt_ms  = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (dt_ms == 0) {
            dt_ms = 1; /* never hand the recogniser a zero-length step */
        }

        const bool       was_active = tg_active(&tg);
        const tg_event_t ev = tg_feed(&tg, n, xs[0], ys[0], xs[1], ys[1], dt_ms);

        st.polls++;

        switch (ev) {
        case TG_TAP:         st.taps++;   break;
        case TG_TAP2:        st.taps2++;  break;
        case TG_SWIPE_LEFT:
        case TG_SWIPE_RIGHT:
        case TG_SWIPE_UP:
        case TG_SWIPE_DOWN:  st.swipes++; break;
        default:             break;
        }

        if (ev != TG_NONE) {
            ESP_LOGI(TAG, "%s", tg_event_name(ev));
            gest_handle(ev);
        } else if (was_active && !tg_active(&tg)) {
            /* A session that ended without a gesture — a long press, a drag
             * below the swipe threshold, a two-finger drag — is one the
             * recogniser deliberately did not act on. A sample with no finger
             * down at all is not a gesture at all, so it is not counted. */
            st.ignored++;
        }

        st.touching = tg_active(&tg);
        stats_publish(&st);
    }
}

void ui_input_start(void)
{
    if (s_started) {
        return; /* two readers of one touch handle is exactly what this prevents */
    }
    if (app_hw_touch() == NULL) {
        /* No controller (harness, or bsp_touch_init() never ran): nothing to
         * read. Leaving running == false is what the overlay shows. */
        ESP_LOGW(TAG, "no touch controller — gestures off");
        return;
    }

    s_started = true;

    /* C6 has a single high-performance core; task affinity would be pointless. */
    if (xTaskCreate(ui_input, "ui_input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO, NULL) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "ui_input task creation failed");
    }
}
