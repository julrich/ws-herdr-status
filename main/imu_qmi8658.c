/* QMI8658A driver.
 *
 * Register map, init sequence and both scale factors are the vendor's, taken
 * from the factory demo's BSP (`components/esp_bsp/bsp_qmi8658.c` and the
 * `qmi8658_reg_t` enum in its header) — the only ground truth for this board,
 * whose IMU is unverified here (AGENTS.md §10), so nothing below is guessed.
 * The chip shares the touch controller's I2C bus and is driven with the IDF
 * `i2c_master` API, like the vendor BSP does.
 */

#include "imu_qmi8658.h"

#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

/* Registers (vendor enum values). */
#define QMI8658_I2C_ADDR 0x6B
#define QMI8658_WHO_AM_I 0x00
#define QMI8658_CTRL1    0x02 /* 0x40: register address auto-increment */
#define QMI8658_CTRL2    0x03 /* 0x95: accel ±4 g, 250 Hz */
#define QMI8658_CTRL3    0x04 /* 0xD5: gyro ±512 dps, 250 Hz */
#define QMI8658_CTRL7    0x08 /* 0x03: enable accelerometer + gyroscope */
#define QMI8658_STATUS0  0x2E /* bit 0 accel ready, bit 1 gyro ready */
#define QMI8658_AX_L     0x35 /* AX_L..GZ_H: six little-endian int16 */
#define QMI8658_RESET    0x60

#define QMI8658_WHO_AM_I_ID 0x05

/* Scales for the ±4 g / ±512 dps ranges the init sequence selects. */
#define ACCEL_G_PER_LSB  (4.0f / 32768.0f)
#define GYRO_DPS_PER_LSB (512.0f / 32768.0f)

/* The vendor's own driver uses a 1 s I2C timeout, and the shorter one tried
 * here first (100 ms) made this chip's reads fail en masse on this board: the
 * bus stretches occasionally, and giving up early leaves the chip's read
 * pointer mid-transfer, which cascades. Measured: with 100 ms the sample rate
 * collapsed to a handful per minute; with the vendor's 1000 ms it runs at the
 * poll rate. The detector tolerates a long stall (it integrates real time), so
 * the only cost is latency in the rare bad case. */
#define QMI8658_I2C_TIMEOUT_MS 1000

/* The chip shares its bus with the touch controller, whose driver (AGENTS.md §7)
 * ignores the result of its own register-address write, so a hiccup on this bus
 * is a matter of when, not if. Left alone, one bad transaction used to leave
 * every later read failing until the next boot: after this many consecutive
 * failures the vendor init sequence is replayed. Rate-limited so a chip that is
 * genuinely absent cannot spin on the bus. */
#define QMI8658_FAIL_BEFORE_REINIT 20
#define QMI8658_REINIT_MIN_GAP_MS  5000
/* The chip has no reset pin and its VDD is hard-wired to 3V3 (schematic: U3,
 * pins 5 and 8 straight to the rail, no load switch), so a chip that stops
 * acknowledging cannot be revived from firmware — only by unplugging the board.
 * After this many failed revival attempts the driver stops talking to it
 * entirely: the console stays readable and the bus stays quiet for the touch
 * controller, and the rotation feature is simply absent until the next power-up.
 */
#define QMI8658_REINIT_GIVE_UP 2

static i2c_master_dev_handle_t s_dev;
static uint32_t                s_fail_streak;
static int64_t                 s_last_reinit_us;
static bool                    s_warned;
static uint8_t                 s_reinit_attempts;
static bool                    s_give_up;

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(QMI8658_I2C_TIMEOUT_MS));
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof buf, pdMS_TO_TICKS(QMI8658_I2C_TIMEOUT_MS));
}

/* Vendor init sequence, in their order and with their values. Replayed on
 * recovery, so it must be safe to run at any time. */
static esp_err_t chip_configure(void)
{
    esp_err_t err = reg_write(QMI8658_RESET, 0xB0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    err = reg_write(QMI8658_CTRL1, 0x40);
    if (err != ESP_OK) {
        return err;
    }
    err = reg_write(QMI8658_CTRL7, 0x03);
    if (err != ESP_OK) {
        return err;
    }
    err = reg_write(QMI8658_CTRL2, 0x95);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write(QMI8658_CTRL3, 0xD5);
}

esp_err_t imu_qmi8658_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDR,
        /* The same 400 kHz as the touch controller on the same bus: running the
         * two at different rates made the IDF re-clock the bus on every switch
         * between them, and the touch controller stopped acknowledging within a
         * minute of that (measured). One uniform speed instead. */
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add_device failed");

    uint8_t id = 0;
    esp_err_t err = reg_read(QMI8658_WHO_AM_I, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    if (id != QMI8658_WHO_AM_I_ID) {
        ESP_LOGW(TAG, "not found: WHO_AM_I = 0x%02x, expected 0x%02x", id, QMI8658_WHO_AM_I_ID);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "found (WHO_AM_I = 0x%02x)", id);

    ESP_RETURN_ON_ERROR(chip_configure(), TAG, "configuration failed");
    s_last_reinit_us = esp_timer_get_time();

    return ESP_OK;
}

/* Little-endian int16 without a cast through an unaligned pointer. */
static int16_t le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Count consecutive failures and, if the chip has gone quiet, replay the init
 * sequence instead of failing until the next boot. */
static void note_failure(esp_err_t err)
{
    if (s_dev == NULL || s_give_up) {
        return;
    }
    s_fail_streak++;
    if (!s_warned) {
        s_warned = true;
        ESP_LOGW(TAG, "read failed (%s) — watching for recovery", esp_err_to_name(err));
    }
    if (s_fail_streak < QMI8658_FAIL_BEFORE_REINIT) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now - s_last_reinit_us < (int64_t)QMI8658_REINIT_MIN_GAP_MS * 1000) {
        return;
    }
    s_last_reinit_us = now;

    s_reinit_attempts++;
    const esp_err_t cfg = chip_configure();
    ESP_LOGW(TAG, "re-initialised after %u failed reads (%s)", (unsigned)s_fail_streak,
             cfg == ESP_OK ? "ok" : esp_err_to_name(cfg));
    if (cfg == ESP_OK) {
        s_fail_streak = 0;
        s_warned = false;
        s_reinit_attempts = 0;
        return;
    }

    if (s_reinit_attempts >= QMI8658_REINIT_GIVE_UP) {
        s_give_up = true;
        ESP_LOGE(TAG, "QMI8658 stopped responding and cannot be reset in software — "
                      "unplug the board to bring rotation back; the companion keeps running");
    }
}

bool imu_qmi8658_read(float *ax_g, float *ay_g, float *az_g,
                      float *gx_dps, float *gy_dps, float *gz_dps)
{
    if (s_give_up) {
        return false; /* silent: no bus traffic, no log flood */
    }

    uint8_t status = 0;
    esp_err_t err = reg_read(QMI8658_STATUS0, &status, 1);
    if (err != ESP_OK) {
        note_failure(err);
        return false;
    }
    if ((status & 0x03) == 0) {
        return false; /* no sample ready yet; not a failure */
    }

    /* CTRL1 auto-increment: one read yields all six little-endian int16. */
    uint8_t raw[12];
    err = reg_read(QMI8658_AX_L, raw, sizeof raw);
    if (err != ESP_OK) {
        note_failure(err);
        return false;
    }
    s_fail_streak = 0;
    s_warned      = false;

    *ax_g = (float)le16(raw + 0) * ACCEL_G_PER_LSB;
    *ay_g = (float)le16(raw + 2) * ACCEL_G_PER_LSB;
    *az_g = (float)le16(raw + 4) * ACCEL_G_PER_LSB;
    *gx_dps = (float)le16(raw + 6) * GYRO_DPS_PER_LSB;
    *gy_dps = (float)le16(raw + 8) * GYRO_DPS_PER_LSB;
    *gz_dps = (float)le16(raw + 10) * GYRO_DPS_PER_LSB;
    return true;
}
