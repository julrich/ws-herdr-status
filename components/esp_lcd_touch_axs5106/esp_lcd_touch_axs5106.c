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

    /* The report is still pending in the controller, and it will not assert the
     * line again for data it has already announced. Forgetting the level lets the
     * next poll retry, so one badly timed read does not cost the whole touch. */
    s_int_level = -1;

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

    /* Read once per interrupt assertion — the way the vendor's own Arduino driver
     * for this panel does it: an ISR on the falling edge sets a flag, and the
     * read consumes the flag before touching the bus.
     *
     * Reading whenever the line merely *is* asserted (what this driver did first)
     * hammers the controller for the whole duration of a touch, and it answers
     * with NACKs: the panel log showed read failures only ever at the moment of a
     * touch, two attempts each, over and over, with no sample ever obtained.
     * Waiting for a fresh assertion makes each read happen when the controller
     * says a report is ready, which is both gentler and correctly timed.
     *
     * Idle panels still cost nothing: with no finger down the line never asserts,
     * so the IMU on this shared bus gets the silence it needs (AGENTS.md §11). */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        const int  level  = gpio_get_level(tp->config.int_gpio_num);
        const int  active = tp->config.levels.interrupt ? 1 : 0;
        const bool fresh  = (level == active) && (s_int_level != active);

        s_int_level = (int8_t)level;
        if (!fresh) {
            return ESP_OK; /* no new report */
        }
    }

    err = axs5106_read_recovering(tp, TOUCH_AXS5106_TOUCH_POINTS_REG, data, 14);
    ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");
    points = data[1];
    points = points & 0x0F;

    if (points == 0)
    {
        return ESP_OK;
    }

    /* Number of touched points */
    points = (points > 2 ? 2 : points);

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
    /* One combined transaction (address phase, repeated start, read).
     *
     * The vendor's version issued i2c_master_transmit() and i2c_master_receive()
     * as two separate transactions and threw the transmit result away — this
     * driver is shared with the QMI8658A, which sits on the same bus, and an
     * aborted address phase followed by a blind read wedged it often enough to
     * knock the IMU's own reads out (see AGENTS.md §11). i2c_master_transmit_receive()
     * has no window in between for that to happen. */
    return i2c_master_transmit_receive(g_dev_handle, &reg, 1, data, len, 100);
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