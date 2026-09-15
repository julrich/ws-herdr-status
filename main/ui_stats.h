/* Data contract for the diagnostics overlay and the stats view.
 *
 * Free of esp_* and LVGL on purpose: main/ui_companion.c reads these structs to
 * render both screens, and it is compiled on the host by tools/ui_host_test
 * (AGENTS.md §9a), which stubs the three getters the way it already stubs
 * herdr_client_get().
 *
 * Producers:
 *   - herdr_client_stats()   — the HTTP poller            (main/herdr_client.c)
 *   - ui_rotation_stats_get() — the IMU/rotation task      (main/ui_rotation.c)
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

/* IMU and rotation, as the rotation task sees it. */
typedef struct {
    bool     running;     /* the IMU task is alive */
    bool     present;     /* the chip answered at init */
    uint32_t rate_hz;     /* measured sample rate, whole Hz */
    uint32_t samples;     /* samples accepted since boot */
    uint32_t errors;      /* failed reads since boot */
    int8_t   axis;        /* calibrated screen normal: 0=x, 1=y, 2=z */
    int8_t   sign;        /* +1/-1; meaningless before `calibrated` */
    bool     calibrated;
} herdr_imu_stats_t;

/* Free heap in bytes, and its low-water mark since boot. 0 where the platform
 * does not provide them (the host harness), which the overlay renders as n/a. */
uint32_t ui_device_free_heap(void);
uint32_t ui_device_min_free_heap(void);

void herdr_client_stats(herdr_link_stats_t *out);
void ui_rotation_stats_get(herdr_imu_stats_t *out);
