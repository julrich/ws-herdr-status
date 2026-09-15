#include <stdio.h>
#include "esp_lcd_touch_axs5106.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

static const char *TAG = "esp_lcd_touch_axs5106";

/* The controller can latch an interrupt that it never gets to clear: the first
 * read after a touch fails, INT stays asserted, and every later read then fails
 * too. Measured on this board as bursts of eight to ten NACKs per touch and a
 * single recognised gesture in a whole session of tapping. Unlike the IMU on the
 * same bus, this part has a reset pin, so the driver recovers by resetting it.
 *
 * The first failure is the one worth surviving: a transaction issued while the
 * controller is still assembling a report is NACKed, and a retry a few
 * milliseconds later succeeds. */
#define AXS5106_FAILS_BEFORE_RESET 8
#define AXS5106_READ_ATTEMPTS 2
#define AXS5106_RETRY_GAP_MS 3

#define TOUCH_AXS5106_TOUCH_POINTS_REG (0X01)
/* Identification register, per the Arduino driver shipped with this panel. */
#define AXS5106_ID_REG (0x08)

/* The interrupt level seen by the previous read, so a sample is taken once per
 * assertion rather than for as long as the line stays asserted. */
static int8_t s_int_level = -1;

/* Whether the last report that got through said a finger was down. This has to be
 * the driver's own flag: esp_lcd_touch_get_coordinates() consumes the sample (it
 * zeroes tp->data.points), so the handle cannot answer this question. It is also
 * what makes the release observable at all — see the gate in read_data(). */
static bool s_finger_down;
#define TOUCH_AXS5106_TOUCH_P1_XH_REG (0x03)
#define TOUCH_AXS5106_TOUCH_P1_XL_REG (0x04)
#define TOUCH_AXS5106_TOUCH_P1_YH_REG (0x05)
#define TOUCH_AXS5106_TOUCH_P1_YL_REG (0x06)

#define TOUCH_AXS5106_TOUCH_P2_XH_REG (0x09)
#define TOUCH_AXS5106_TOUCH_P2_XL_REG (0x0A)
#define TOUCH_AXS5106_TOUCH_P2_YH_REG (0x0B)
#define TOUCH_AXS5106_TOUCH_P2_YL_REG (0x0C)

i2c_master_dev_handle_t g_dev_handle;

/*******************************************************************************
 * Function definitions
 *******************************************************************************/
static esp_err_t esp_lcd_touch_axs5106_read_data(esp_lcd_touch_handle_t tp);
static bool esp_lcd_touch_axs5106_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t esp_lcd_touch_axs5106_del(esp_lcd_touch_handle_t tp);

/* I2C read */
static esp_err_t touch_axs5106_i2c_read(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t *data, uint8_t len);

/* AXS5106 init */
static esp_err_t touch_axs5106_init(esp_lcd_touch_handle_t tp);
/* AXS5106 reset */
static esp_err_t touch_axs5106_reset(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_axs5106(i2c_master_dev_handle_t dev_handle, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch)
{
    esp_err_t ret = ESP_OK;
    g_dev_handle = dev_handle;
    assert(config != NULL);
    assert(out_touch != NULL);

    /* Prepare main structure */
    esp_lcd_touch_handle_t esp_lcd_touch_axs5106 = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE(esp_lcd_touch_axs5106, ESP_ERR_NO_MEM, err, TAG, "no mem for AXS5106 controller");

    /* Communication interface */
    // esp_lcd_touch_axs5106->io = io;

    /* Only supported callbacks are set */
    esp_lcd_touch_axs5106->read_data = esp_lcd_touch_axs5106_read_data;
    esp_lcd_touch_axs5106->get_xy = esp_lcd_touch_axs5106_get_xy;
    esp_lcd_touch_axs5106->del = esp_lcd_touch_axs5106_del;

    /* Mutex */
    esp_lcd_touch_axs5106->data.lock.owner = portMUX_FREE_VAL;

    /* Save config */
    memcpy(&esp_lcd_touch_axs5106->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (esp_lcd_touch_axs5106->config.int_gpio_num != GPIO_NUM_NC)
    {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (esp_lcd_touch_axs5106->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(esp_lcd_touch_axs5106->config.int_gpio_num)};
        ret = gpio_config(&int_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed");

        /* Register interrupt callback */
        if (esp_lcd_touch_axs5106->config.interrupt_callback)
        {
            esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_axs5106, esp_lcd_touch_axs5106->config.interrupt_callback);
        }
    }

    /* Prepare pin for touch controller reset */
    if (esp_lcd_touch_axs5106->config.rst_gpio_num != GPIO_NUM_NC)
    {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(esp_lcd_touch_axs5106->config.rst_gpio_num)};
        ret = gpio_config(&rst_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed");
    }

    /* Reset controller */
    ret = touch_axs5106_reset(esp_lcd_touch_axs5106);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "AXS5106 reset failed");

    /* The Arduino driver for this panel reads this register at init and only
     * prints it when non-zero, so 0x00 is expected — what matters is that the
     * controller answered at all. The ESP-IDF driver never checked, which is one
     * reason a half-initialised part went unnoticed as long as it did. */
    {
        uint8_t id = 0;
        if (touch_axs5106_i2c_read(esp_lcd_touch_axs5106, AXS5106_ID_REG, &id, 1) == ESP_OK) {
            ESP_LOGI(TAG, "controller id register reads 0x%02x", id);
            /* Which read discipline is running, so the console says so. Twice in
             * one session a flashed image was mistaken for the previous one; this
             * line settles it without reading the source. */
            ESP_LOGI(TAG, "driver: two-step read, INT-gated, 8-byte window, "
                          "release on empty read");
        } else {
            ESP_LOGW(TAG, "no answer from the controller after reset");
        }
    }

    /* Init controller */
    ret = touch_axs5106_init(esp_lcd_touch_axs5106);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "AXS5106 init failed");

err:
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (0x%x)! Touch controller AXS5106 initialization failed!", ret);
        if (esp_lcd_touch_axs5106)
        {
            esp_lcd_touch_axs5106_del(esp_lcd_touch_axs5106);
        }
    }

    *out_touch = esp_lcd_touch_axs5106;

    return ret;
}

/* Reads with a short retry, and resets the controller when it has stopped
 * answering altogether. Never returns the stale previous sample: the caller
 * treats a failure as "no touch", which is what a stuck controller means. */
static esp_err_t axs5106_read_recovering(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t *data, uint8_t len)
{
    static uint8_t fail_streak;

    for (int attempt = 0; attempt < AXS5106_READ_ATTEMPTS; attempt++) {
        if (touch_axs5106_i2c_read(tp, reg, data, len) == ESP_OK) {
            fail_streak = 0;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(AXS5106_RETRY_GAP_MS));
    }

    if (++fail_streak >= AXS5106_FAILS_BEFORE_RESET) {
        fail_streak = 0;
        ESP_LOGW(TAG, "controller stopped answering: resetting it");
        ESP_RETURN_ON_ERROR(touch_axs5106_reset(tp), TAG, "reset failed");
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t esp_lcd_touch_axs5106_read_data(esp_lcd_touch_handle_t tp)
{
    esp_err_t err;
    uint8_t data[30] = {0};
    uint8_t points;
    size_t i = 0;

    assert(tp != NULL);

    /* This controller answers only when it has a report to give, and INT is how
     * it says so. Two measurements on this unit shape the rule:
     *
     *  - Reading on every poll (the vendor's own IDF driver, and what this driver
     *    did first) is refused every single time: at idle, every read NACKs at the
     *    50 ms poll rate, the fail-streak reset fires every ~8 attempts, and the
     *    controller does not recover. An idle panel must not be polled for data.
     *  - Reading only on the *asserting* edge loses the release. A release that is
     *    not announced as another report leaves the last finger-down sample in
     *    tp->data, and nothing else clears it: esp_lcd_touch_read_data() only
     *    forwards to the driver, while esp_lcd_touch_get_coordinates() consumes
     *    the sample by zeroing tp->data.points. LVGL then holds a press that never
     *    comes up, so no tap, long press or swipe ever fires — a dead-looking
     *    panel with a healthy driver and a clean log.
     *
     * So: read when the controller announces a report, and also while the last
     * report still says a finger is down. If that second read finds nothing
     * pending, the finger has stopped reporting, which is the release. */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        const int  level  = gpio_get_level(tp->config.int_gpio_num);
        const int  active = tp->config.levels.interrupt ? 1 : 0;
        const bool fresh  = (level == active) && (s_int_level != active);

        s_int_level = (int8_t)level;
        if (!fresh && !s_finger_down) {
            return ESP_OK; /* nothing announced, nothing outstanding */
        }
    }

    /* Count + finger 1 only, registers 0x01..0x08: this panel is single-touch, so
     * this is the whole report. The vendor's 14 bytes reach into 0x09..0x0E, the
     * second finger's window, which is the leading suspect for the data-phase
     * NACKs this board produced during touches (AGENTS.md §7). */
    err = axs5106_read_recovering(tp, TOUCH_AXS5106_TOUCH_POINTS_REG, data, 8);
    if (err != ESP_OK) {
        /* Nothing pending. With a finger down that is the release, and it has to
         * be reported as one: the alternative is a press that stands forever.
         * A driver failure means "no touch" to the caller, which is why this
         * clears the sample and returns success rather than the error. */
        if (s_finger_down) {
            s_finger_down = false;
            portENTER_CRITICAL(&tp->data.lock);
            tp->data.points = 0;
            portEXIT_CRITICAL(&tp->data.lock);
            ESP_LOGI(TAG, "finger up (no report pending)");
        }
        return ESP_OK;
    }
    points = data[1];
    points = points & 0x0F;

    const bool was_down = s_finger_down;
    s_finger_down = (points > 0);

    if (points == 0)
    {
        if (was_down) {
            ESP_LOGI(TAG, "finger up");
        }
        return ESP_OK;
    }

    /* One finger is all this panel reports, and all an 8-byte read carries. */
    points = (points > 1 ? 1 : points);

    // err = touch_axs5106_i2c_read(tp, TOUCH_AXS5106_TOUCH_P1_XH_REG, data, 6 * points);
    // ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");

    portENTER_CRITICAL(&tp->data.lock);

    /* Number of touched points */
    tp->data.points = points;

    /* Fill all coordinates */
    for (i = 0; i < points; i++)
    {
        tp->data.coords[i].y = (((uint16_t)(data[4 + i * 6] & 0x0f)) << 8);
        tp->data.coords[i].y |= data[5 + i * 6];
        tp->data.coords[i].x = ((uint16_t)(data[2 + i * 6] & 0x0f)) << 8;
        tp->data.coords[i].x |= data[3 + i * 6];
    }
    portEXIT_CRITICAL(&tp->data.lock);

    /* Transition only: two lines per touch, and the down line carries the raw
     * coordinates, which is how the panel's mapping is confirmed on hardware
     * without eyes on the screen. */
    if (!was_down) {
        ESP_LOGI(TAG, "finger down (%u,%u)", (unsigned)tp->data.coords[0].x,
                 (unsigned)tp->data.coords[0].y);
    }

    return ESP_OK;
}

static bool esp_lcd_touch_axs5106_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    assert(tp != NULL);
    assert(x != NULL);
    assert(y != NULL);
    assert(point_num != NULL);
    assert(max_point_num > 0);

    portENTER_CRITICAL(&tp->data.lock);

    /* Count of points */
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);

    for (size_t i = 0; i < *point_num; i++)
    {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength)
        {
            strength[i] = tp->data.coords[i].strength;
        }
    }

    /* Invalidate */
    tp->data.points = 0;

    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t esp_lcd_touch_axs5106_del(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);

    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC)
    {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback)
        {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }

    /* Reset GPIO pin settings */
    if (tp->config.rst_gpio_num != GPIO_NUM_NC)
    {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }

    free(tp);

    return ESP_OK;
}


static esp_err_t touch_axs5106_i2c_read(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t *data, uint8_t len)
{
    assert(tp != NULL);
    assert(data != NULL);
    /* Two transactions, the way the vendor's driver does it: write the register
     * address, then read it in a separate transaction, with a STOP in between.
     *
     * Measured on this unit with a probe that tried all four shapes over one
     * touch (AGENTS.md §7):
     *
     *   transmit + receive          14 bytes -> ESP_OK   (block 00 01 80 5d 00 c5 40 10)
     *   transmit + receive           8 bytes -> ESP_OK
     *   transmit_receive (RESTART)   14 and 8 -> NACK, ESP_ERR_INVALID_STATE
     *
     * The AXS5106 does not answer a read joined to the address phase by a
     * repeated start; it wants the STOP. That is why the vendor's driver and the
     * working template use this shape, and why every touch read in this project
     * failed while the QMI8658A on the same bus read fine through the same
     * helper: the IMU accepts a repeated start, this controller does not. The
     * length was never the problem.
     *
     * The vendor threw the transmit result away. This does not: an unacknowledged
     * address phase leaves the controller unaddressed, and a blind read after
     * that is what knocked the QMI8658A's own reads out (AGENTS.md §11). */
    esp_err_t err = i2c_master_transmit(g_dev_handle, &reg, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(g_dev_handle, data, len, 100);
}

static esp_err_t touch_axs5106_init(esp_lcd_touch_handle_t tp)
{
    return ESP_OK;
}

/* Reset timing is the vendor's own, from the Arduino driver that ships with this
 * panel (Arduino/libraries/esp_lcd_touch_axs5106l): 200 ms asserted, then 300 ms
 * of settle time. The ESP-IDF driver's 10 ms pulse is far too short — the part
 * comes back up enough to ACK and report a point count while leaving the
 * coordinate registers at zero, which is precisely the "raw n=1 p0=(172,0)" the
 * panel produced, and it is not something any read strategy can recover from. */
#define AXS5106_RESET_ASSERT_MS 200
#define AXS5106_RESET_SETTLE_MS 300

static esp_err_t touch_axs5106_reset(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);

    if (tp->config.rst_gpio_num != GPIO_NUM_NC)
    {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(AXS5106_RESET_ASSERT_MS));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(AXS5106_RESET_SETTLE_MS));
    }

    return ESP_OK;
}