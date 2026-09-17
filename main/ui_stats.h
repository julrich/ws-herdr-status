/* Data contract for the stats view's link and device page.
 *
 * Free of esp_* and LVGL on purpose: main/ui_companion.c reads these structs to
 * render it, and it is compiled on the host by tools/ui_host_test (AGENTS.md §9a),
 * which stubs the getters the way it already stubs herdr_client_get().
 *
 * Producers:
 *   - herdr_client_stats()   — the HTTP poller            (main/herdr_client.c)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bridge link, as the poller sees it. */
typedef struct {
    uint32_t gen;         /* last generation the bridge reported */
    uint32_t polls;       /* successful polls since boot */
    uint32_t failures;    /* consecutive failures right now */
    uint32_t fail_total;  /* failures since boot */
    uint32_t rtt_ms;      /* last successful round trip */
    bool     online;      /* last poll succeeded and the wire is not stale */
} herdr_link_stats_t;

/* Free heap in bytes. 0 where the platform does not provide it (the host harness). */
uint32_t ui_device_free_heap(void);

void herdr_client_stats(herdr_link_stats_t *out);
