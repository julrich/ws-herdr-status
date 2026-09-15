/* WiFi station bring-up for the herdr status companion.
 *
 * Standard ESP-IDF station flow: no vendor bsp_wifi exists in this repo, so
 * netif + event loop + station config live here. The reconnect loop never
 * gives up; the companion simply renders OFFLINE while the link is down.
 */

#include "wifi_sta.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

/* Written by the event loop task, read by the herdr_poll task. A single word
 * write cannot tear, so no lock is needed; a stale read costs one 500 ms poll
 * delay at worst. */
static volatile bool s_connected;

static bool              s_started;
static esp_ip4_addr_t    s_ip; /* logging only; esp_netif_get_ip_info is not needed */
static esp_timer_handle_t s_retry_timer;
static int               s_retry_step;

/* 1 s, 2 s, 4 s, 8 s, then a 10 s ceiling. */
static const uint32_t s_backoff_ms[] = { 1000, 2000, 4000, 8000, 10000 };
#define BACKOFF_STEPS (sizeof s_backoff_ms / sizeof s_backoff_ms[0])

/* esp_wifi_connect() must not be called from the event handler context, so the
 * retry runs from the esp_timer task instead of the WiFi event task. */
static void wifi_retry_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;

        const uint32_t delay_ms = s_backoff_ms[s_retry_step];
        if (s_retry_step < (int)BACKOFF_STEPS - 1) {
            s_retry_step++;
        }
        ESP_LOGW(TAG, "disconnected (reason %d), retrying in %d ms", (int)d->reason, (int)delay_ms);

        /* The timer is idle between retries; stopping first makes re-arming safe
         * even if a disconnect lands inside the previous retry's callback. */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        char buf[16];

        s_ip = e->ip_info.ip;
        s_connected = true;
        s_retry_step = 0;

        ESP_LOGI(TAG, "connected, ip=%s", esp_ip4addr_ntoa(&s_ip, buf, sizeof buf));
    }
}

void wifi_sta_start(void)
{
    if (s_started) {
        return;
    }
    if (CONFIG_HERDR_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "HERDR_WIFI_SSID is empty — set it in menuconfig");
        return;
    }
    s_started = true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        s_started = false;
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        s_started = false;
        return;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        s_started = false;
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&timer_args, &s_retry_timer) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed");
        s_started = false;
        return;
    }

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
        s_started = false;
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       wifi_event_handler, NULL, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, CONFIG_HERDR_WIFI_SSID, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, CONFIG_HERDR_WIFI_PASSWORD, sizeof sta.sta.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    /* No power save: this is a mains-powered desk companion, and modem sleep
     * against this AP swallowed the HTTP responses (TCP handshakes reached the
     * PC but its replies were never ACKed; every poll timed out after 3 s). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "station started (ssid=\"%s\")", CONFIG_HERDR_WIFI_SSID);
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}