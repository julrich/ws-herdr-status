/* Rotation: persist the panel orientation and follow the IMU after a turn. */
#pragma once

/* The orientation the device was last used in: 0, 90, 180 or 270; 0 when NVS
 * holds nothing or an unusable value. Reads only NVS, so it is safe to call
 * before the panel exists. Must be called after nvs_flash_init(). */
int ui_rotation_restore(void);

/* Starts the `imu_rot` task, which turns the panel through app_apply_rotation()
 * when the device is turned on the desk. Does nothing at all when
 * CONFIG_HERDR_IMU_ROTATE is off, so the panel stays in whatever orientation
 * ui_rotation_restore() asked for. */
void ui_rotation_start(void);
