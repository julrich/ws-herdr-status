/* ws-herdr-status — herdr agent-status companion.
 *
 * Board: ESP32-C6FH8, 8 MB flash, 172x320 IPS panel (JD9853 over 4-wire SPI),
 * capacitive touch (AXS5106L over I2C), WiFi station + HTTP poller.
 * See docs.waveshare.com/ESP32-C6-Touch-LCD-1.47
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lvgl_port.h"

#include "app_hw.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "bsp_touch.h"
#include "herdr_client.h"
#include "ui_companion.h"
#include "wifi_sta.h"

static const char *TAG = "app";

/* Native panel geometry; the companion runs it in portrait. */
#define LCD_H_RES       172
#define LCD_V_RES       320
#define LCD_ROTATION    0

/* 172 x 40 px x 2 B = 13.8 KB per buffer, doubled for the DMA pipeline. WiFi
 * and the poller share the heap with LVGL, and the panel is not the throughput
 * bottleneck: at 80 MHz SPI a 40-line chunk flushes in ~1.4 ms against the
 * 30 ms refresh period. */
#define LCD_DRAW_BUFF_LINES 40
#define LCD_DRAW_BUFF_DOUBLE 1

/* The JD9853 GRAM is wider than the 172 px visible window; the vendor BSP
 * leaves the horizontal gap at column 34 for the 0/180 rotations. */
#define LCD_GAP_X 34
#define LCD_GAP_Y 0

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c;
static lv_display_t *s_disp;

i2c_master_bus_handle_t app_hw_i2c(void) { return s_i2c; }
esp_lcd_panel_handle_t  app_hw_panel(void) { return s_panel; }
esp_lcd_touch_handle_t  app_hw_touch(void) { return s_touch; }
lv_display_t           *app_hw_disp(void) { return s_disp; }

/* The vendor's table for this panel (AGENTS.md §5) and its touch driver (§7).
 * Both are indexed by the angle the *device* has been turned by. */
/* The vendor's mapping for this panel (AGENTS.md §5), in the one orientation this
 * project uses: no swap, no mirror, and the 34 px GRAM gap. */
static void apply_panel(void)
{
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
}

/* Build the UI on a fresh screen, in the panel's one supported orientation. The old
 * screen is freed by the auto-delete (the host harness relies on the same behaviour).
 *
 * This used to be app_apply_rotation(), which re-pointed the panel, the touch
 * mapping and the display resolution whenever the IMU saw a quarter turn. That is
 * gone: the board sits upright with the face above the list, and the panel and touch
 * are left exactly as bsp_touch_init() and the vendor's table set them up. */
static void app_build_ui(void)
{
    if (s_disp == NULL || s_panel == NULL) {
        return;
    }

    if (!lvgl_port_lock(0)) {
        return;
    }

    lv_scr_load_anim(lv_obj_create(NULL), LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    ui_companion_create();

    lvgl_port_unlock();
}

static esp_err_t lvgl_start(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 1024 * 8,
        .task_affinity = -1,        /* no affinity: the C6 has a single HP core */
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFF_LINES,
        .double_buffer = LCD_DRAW_BUFF_DOUBLE,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            /* LVGL 8 took the panel's byte order from CONFIG_LV_COLOR_16_SWAP;
             * LVGL 9 dropped that option and the port passes it to the display
             * instead. Same JD9853, same big-endian RGB565 (AGENTS.md §6). */
            .swap_bytes = 1,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");
    s_disp = disp;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y), TAG, "set_gap failed");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch,
    };
    /* The port's own pointer indev is the whole input path: this panel reports a
     * single touch, so LVGL's click / long-press / gesture events cover the
     * companion's vocabulary (AGENTS.md §11). */
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg) != NULL, ESP_FAIL, TAG, "lvgl_port_add_touch failed");

    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "herdr status companion starting");

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_init();
    s_i2c = i2c_bus;
    bsp_spi_init();
    bsp_display_init(&s_io, &s_panel, LCD_H_RES * LCD_DRAW_BUFF_LINES);
    bsp_touch_init(&s_touch, i2c_bus, LCD_H_RES, LCD_V_RES, LCD_ROTATION);
    ESP_ERROR_CHECK(lvgl_start());

    bsp_display_brightness_init();

    wifi_sta_start();
    herdr_client_start();

    /* Build the UI. app_build_ui() creates the screen itself. */
    app_build_ui();

    /* Light the backlight only after the first frame has been pushed out, so
     * the uninitialised GRAM is never visible. */
    vTaskDelay(pdMS_TO_TICKS(200));
    bsp_display_set_brightness(100);

    ESP_LOGI(TAG, "companion running — free heap %u bytes", (unsigned)esp_get_free_heap_size());
}
