/* Wire/UI contract shared by the firmware and the host render harness.
 *
 * This header MUST stay free of esp_* includes: `tools/ui_host_test` compiles
 * `ui_companion.c` against it on the host (see AGENTS.md §9a).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Agents the wire parser will hold. */
#define HERDR_MAX_AGENTS 6
/* Label buffer: the bridge already truncates to 40 bytes of printable ASCII. */
#define HERDR_LABEL_LEN 40

typedef enum {
    HERDR_ST_UNKNOWN = 0,
    HERDR_ST_IDLE,
    HERDR_ST_WORKING,
    HERDR_ST_BLOCKED,
    HERDR_ST_DONE,
} herdr_agent_state_t;

typedef struct {
    char               id[24]; /* pane id, e.g. "w5:p1"; matches the bridge's /stats */
    char               label[HERDR_LABEL_LEN];
    char               kind[12];
    herdr_agent_state_t state;
    bool               focused;
} herdr_agent_t;

typedef struct {
    int           count;    /* agents parsed into this struct, <= HERDR_MAX_AGENTS */
    int           overflow; /* parsed_total - min(parsed_total, CONFIG_HERDR_UI_MAX_AGENTS) */
    bool          online;   /* last HTTP poll succeeded and the wire says stale == false */
    bool          stale;    /* wire staleness flag (bridge has not reached herdr recently) */
    uint32_t      gen;      /* wire generation counter; a change means new data */
    herdr_agent_t agents[HERDR_MAX_AGENTS];
} herdr_status_t;

/* Exact strings used by the list rows. */
static inline const char *herdr_state_name(herdr_agent_state_t s)
{
    switch (s) {
    case HERDR_ST_BLOCKED: return "blocked";
    case HERDR_ST_WORKING: return "working";
    case HERDR_ST_DONE:    return "done";
    case HERDR_ST_IDLE:    return "idle";
    default:               return "unknown";
    }
}

/* Logging hook for the UI. The device build compiles main/ with
 * -DUI_USE_ESP_LOG=1 (see main/CMakeLists.txt); the host harness leaves it
 * undefined so ui_companion.c stays free of esp_* headers. */
#ifdef UI_USE_ESP_LOG
#include "esp_log.h"
#define UI_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define UI_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#define UI_LOGI(tag, fmt, ...) ((void)0)
#define UI_LOGW(tag, fmt, ...) ((void)0)
#endif

/* Session statistics for the stats view, served by the bridge's GET /stats and
 * summed from the agent harness's own session logs. Zeroed until the first
 * successful fetch, which the UI renders as "no data yet". `per[]` is aligned
 * with herdr_status_t.agents[]. */
#define HERDR_MODEL_LEN 16

typedef struct {
    uint32_t tokens_in;
    uint32_t tokens_out;
    uint32_t messages;
    uint32_t tool_calls;
    uint32_t age_s; /* seconds since this session's last activity */
    char     model[HERDR_MODEL_LEN];
} herdr_session_t;

typedef struct {
    bool     valid;     /* a fetch has succeeded */
    uint32_t sessions;  /* how many the bridge described */
    uint32_t tokens_in;
    uint32_t tokens_out;
    uint32_t messages;
    uint32_t tool_calls;
    uint32_t age_s;     /* since the newest activity across sessions */
    herdr_session_t per[HERDR_MAX_AGENTS];
} herdr_sessions_t;

/* Filled by herdr_client.c from /stats; false before the first success. */
bool herdr_stats_get(herdr_sessions_t *out);

/* Implemented by herdr_client.c on the device, by a stub in the host harness.
 * `out` is left untouched and false is returned before herdr_client_start(). */
bool herdr_client_get(herdr_status_t *out);

/* Implemented by herdr_client.c; stubbed by the host harness. Non-blocking,
 * never touches LVGL. */
void herdr_client_poll_now(void);