/* QMI8658A accelerometer + gyroscope (the board's IMU, on the touch I2C bus).
 *
 * The header carries only the two IDF types its API needs (an I2C bus handle
 * and esp_err_t); esp_log/FreeRTOS and the register map stay in the .c, the
 * same way main/herdr_client.h keeps its esp_* includes in the .c.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Adds the chip to an already-initialised I2C master bus (main.c's
 * bsp_i2c_init()) at its 0x6B address, checks WHO_AM_I and runs the vendor's
 * init sequence. Returns ESP_ERR_NOT_FOUND when the ID register does not read
 * 0x05, so the caller can give up instead of polling a chip that is not there. */
esp_err_t imu_qmi8658_init(i2c_master_bus_handle_t bus);

/* One sample in the chip's own axes: acceleration in g and turn rate in deg/s.
 * False when STATUS0 reports no new data (or the bus read failed), in which
 * case the outputs are untouched; the next call picks the sample up. All six
 * pointers are required. */
bool imu_qmi8658_read(float *ax_g, float *ay_g, float *az_g,
                      float *gx_dps, float *gy_dps, float *gz_dps);
