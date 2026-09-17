#pragma once

#include <stdbool.h>

#include "herdr_status_types.h"
#include "ui_stats.h"

/* Starts the `herdr_poll` task: waits for WiFi, GETs CONFIG_HERDR_BRIDGE_HOST:
 * CONFIG_HERDR_BRIDGE_PORT/state every CONFIG_HERDR_POLL_PERIOD_MS, publishes
 * into the shared herdr_status_t. */
void herdr_client_start(void);

/* Link counters for the stats view's device page (main/ui_stats.h): `gen` and
 * `online` are the last published poll, the rest are running totals from the
 * poll task. Callable at any time, including before herdr_client_start(). */
void herdr_client_stats(herdr_link_stats_t *out);