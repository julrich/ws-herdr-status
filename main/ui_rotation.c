/* Rotation driver: reads the IMU and turns the panel when the device is turned.
 *
 * The decision itself lives in rotation_logic.c (pure C, host-tested); this file
 * only does the device-side plumbing: NVS persistence, the poll loop, and the
 * hand-off to app_apply_rotation(), which owns the panel, the touch controller
 * and LVGL — nothing here touches any of those directly.
 */

#include "ui_rotation.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_hw.h"
#include "imu_qmi8658.h"
#include "rotation_logic.h"

/* Orientation lives in the same namespace as the rest of the companion's
 * settings, under a key holding the quarter turns applied so far (0..3), not the
 * angle: 270 does not fit in the byte nvs_set_u8() stores, and quarter turns are
 * what the detector commits anyway. */
#define ROT_NVS_NAMESPACE "herdr"
#define ROT_NVS_KEY "rot"
#define ROT_QUARTER_DEG 90

/* Read by the diagnostics overlay from the LVGL task, written by imu_rot. */
static herdr_imu_stats_t s_stats;
static portMUX_TYPE      s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

int ui_rotation_restore(void)
{
#if !CONFIG_HERDR_IMU_ROTATE
    /* Rotation switched off: start in portrait, as the Kconfig option promises,
     * instead of restoring an orientation nothing can then change. */
    return 0;
#else
    nvs_handle_t h;
    if (nvs_open(ROT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0; /* nothing saved yet */
    }

    uint8_t quarters = 0;
    const esp_err_t err = nvs_get_u8(h, ROT_NVS_KEY, &quarters);
    nvs_close(h);

    if (err != ESP_OK || quarters > 3) {
        return 0; /* unset, or written by something else: portrait */
    }
    return (int)quarters * ROT_QUARTER_DEG;
#endif
}

#if CONFIG_HERDR_IMU_ROTATE

static const char *TAG = "imu";

/* The chip samples at 250 Hz, but the detector only needs enough samples to
 * integrate a hand turn: at 10 Hz a 0.4 s quarter turn still yields four gyro
 * readings, and 150 dps over that gesture integrates to ~60 deg, past the 55 deg
 * commit. The rate is deliberately low because this board's I2C link to the IMU
 * degrades with traffic — measured as growing ESP_ERR_TIMEOUT rates after a
 * minute or two of continuous polling, independent of timeout length and of the
 * touch controller's traffic (AGENTS.md §11). Fewer transactions means a longer
 * healthy stretch. The loop measures the real step anyway, so a slow or skipped
 * sample costs accuracy, never correctness. */
#define IMU_TASK_PERIOD_MS 100
#define IMU_TASK_STACK     3072
#define IMU_TASK_PRIO      3
/* 40 samples x ~25 ms is about a second. */
#define IMU_LOG_EVERY 40

static void imu_stats_publish(const herdr_imu_stats_t *st)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats = *st;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void persist_rotation(int deg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ROT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, ROT_NVS_KEY, (uint8_t)(deg / ROT_QUARTER_DEG));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving rotation %d failed: %s", deg, esp_err_to_name(err));
    }
}

static void imu_rot(void *arg)
{
    (void)arg;

    rot_detector_t det;
    rot_detector_init(&det);

    const esp_err_t err = imu_qmi8658_init(app_hw_i2c());
    if (err != ESP_OK) {
        /* One warning and out: a chip that is not on the bus will not appear
         * later, and retrying would only flood the console. The panel keeps the
         * orientation it was restored with. */
        ESP_LOGW(TAG, "QMI8658 unavailable (%s) — rotation stays off", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    int64_t last_us = esp_timer_get_time();
#if CONFIG_HERDR_IMU_LOG_RAW
    int logged = 0;
#endif

    herdr_imu_stats_t st = { .running = true, .present = true };
    uint32_t          window_samples = 0;
    int64_t           window_us      = esp_timer_get_time();
    imu_stats_publish(&st);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(IMU_TASK_PERIOD_MS));

        float ax, ay, az, gx, gy, gz;
        if (!imu_qmi8658_read(&ax, &ay, &az, &gx, &gy, &gz)) {
            /* No sample ready; the next pass picks it up, and the step measured
             * then spans the wait as well. The reader cannot tell "nothing new"
             * from a failed transaction, and both are a read that did not
             * happen, which is what the overlay's error count reports. */
            st.errors++;
            imu_stats_publish(&st);
            continue;
        }

        /* Measure the time that actually passed instead of trusting the delay:
         * pdMS_TO_TICKS(25) is two ticks, i.e. 20 ms, and the loop also waits
         * out any sample that was not ready. The detector's thresholds are in
         * milliseconds, so it needs real time, not a sample count. */
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (dt_ms == 0) {
            dt_ms = 1; /* never hand the integrator a zero-length step */
        }

        const int delta = rot_detector_feed(&det, ax, ay, az, gx, gy, gz, dt_ms);
        if (delta != 0) {
            const int rot = (app_rotation() + delta + 360) % 360;
            if (app_apply_rotation(rot)) {
                persist_rotation(rot);
                ESP_LOGI(TAG, "turned, rot=%d", rot);
            }
        }

        /* Diagnostics for the overlay (main/ui_stats.h). The rate is measured
         * over a one-second window rather than derived from the configured
         * period: pdMS_TO_TICKS() rounds the delay and a slow read stretches a
         * step, so 100 ms is an intention, not the truth. Dividing once per
         * second keeps the number stable to the Hz. */
        st.samples++;
        st.axis       = (int8_t)rot_detector_normal_axis(&det);
        st.sign       = (int8_t)rot_detector_normal_sign(&det);
        st.calibrated = rot_detector_calibrated(&det);

        window_samples++;
        if (now_us - window_us >= 1000000) {
            st.rate_hz = (uint32_t)((int64_t)window_samples * 1000000 / (now_us - window_us));
            window_samples = 0;
            window_us      = now_us;
        }
        imu_stats_publish(&st);

#if CONFIG_HERDR_IMU_LOG_RAW
        if (++logged >= IMU_LOG_EVERY) {
            logged = 0;
            ESP_LOGI(TAG, "a=(%.2f,%.2f,%.2f) g w=(%.0f,%.0f,%.0f) dps axis=%d sign=%d rot=%d",
                     ax, ay, az, gx, gy, gz, rot_detector_normal_axis(&det),
                     rot_detector_normal_sign(&det), app_rotation());
        }
#endif
    }
}

#endif /* CONFIG_HERDR_IMU_ROTATE */

void ui_rotation_start(void)
{
#if CONFIG_HERDR_IMU_ROTATE
    /* C6 has a single high-performance core; task affinity would be pointless. */
    if (xTaskCreate(imu_rot, "imu_rot", IMU_TASK_STACK, NULL, IMU_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "imu_rot task creation failed");
    }
#endif
    /* CONFIG_HERDR_IMU_ROTATE=n: nothing to start; the panel keeps the
     * orientation ui_rotation_restore() returned. */
}

void ui_rotation_stats_get(herdr_imu_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}
