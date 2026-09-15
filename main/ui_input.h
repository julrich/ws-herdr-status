/* Touch input: the single consumer of the touch controller.
 *
 * The esp_lvgl_port indev is removed by this module at task start, because it
 * hands LVGL one point while the controller reports two, and the companion's
 * whole touch vocabulary is gestures (main/touch_gesture.c). This task reads the
 * handle itself and calls the ui_companion_on_*() handlers directly.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the `ui_input` task: polls the touch controller every 25 ms, recognises
 * gestures and runs them under the LVGL port lock. No-op when main.c never got a
 * touch controller to hand out (app_hw_touch() == NULL), in which case
 * ui_input_stats_get() keeps reporting running == false. */
void ui_input_start(void);

#ifdef __cplusplus
}
#endif
