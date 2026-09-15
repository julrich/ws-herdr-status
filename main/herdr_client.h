#pragma once

#include <stdbool.h>

#include "herdr_status_types.h"

/* Starts the `herdr_poll` task: waits for WiFi, GETs CONFIG_HERDR_BRIDGE_HOST:
 * CONFIG_HERDR_BRIDGE_PORT/state every CONFIG_HERDR_POLL_PERIOD_MS, publishes
 * into the shared herdr_status_t. */
void herdr_client_start(void);