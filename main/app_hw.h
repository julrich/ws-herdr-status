/* Handles and hardware actions the rest of the firmware may use.
 *
 * main.c owns the panel, the touch controller and the LVGL display; the rotation
 * module needs all three to switch orientation, so they are reachable through
 * these accessors instead of being duplicated.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
/* LVGL 8 has no lv_display_t; esp_lvgl_port's compatibility header aliases it to
 * lv_disp_t. Include it here so this header stands on its own. */
#include "esp_lvgl_port.h"

#ifdef __cplusplus
extern "C" {
#endif

i2c_master_bus_handle_t app_hw_i2c(void);
esp_lcd_panel_handle_t  app_hw_panel(void);
esp_lcd_touch_handle_t  app_hw_touch(void);
lv_display_t           *app_hw_disp(void);
/* The LVGL touch input device lvgl_start() created. main/ui_input.c removes it
 * (that task reads the controller's two points itself), which is also what frees
 * the indev's memory — see the note on lvgl_port_add_touch(). NULL until then. */
lv_indev_t             *app_hw_touch_indev(void);

/* Switch the whole stack to an orientation: `deg` is 0, 90, 180 or 270, i.e.
 * the angle the *device* has been turned by, so 90/270 mean landscape.
 *
 * Takes the LVGL port lock itself, applies the vendor's panel mapping and GRAM
 * gap for that angle (AGENTS.md §5), points the touch controller the same way
 * (§7), swaps the logical display resolution to 172x320 or 320x172 and rebuilds
 * the UI on a fresh screen. Safe to call before the UI exists and repeatedly,
 * but only from one task at a time. Returns false for an invalid angle or if the
 * hardware was never initialised.
 */
bool app_apply_rotation(int deg);

/* The orientation currently applied (0, 90, 180 or 270). */
int app_rotation(void);

#ifdef __cplusplus
}
#endif
