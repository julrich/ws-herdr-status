#pragma once

#include <stdbool.h>

/* WiFi station bring-up. No-op when CONFIG_HERDR_WIFI_SSID is empty, so a
 * mis-built image still renders an offline companion. */
void wifi_sta_start(void);

/* True between IP_EVENT_STA_GOT_IP and the next disconnect. */
bool wifi_sta_is_connected(void);