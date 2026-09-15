/* herdr status HTTP client + poll task.
 *
 * Polls the PC-side bridge (bridge/herdr_status_bridge.py) every
 * CONFIG_HERDR_POLL_PERIOD_MS and publishes the parsed wire state into the
 * shared herdr_status_t that ui_companion.c reads from its LVGL timer.
 */

#include "herdr_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifi_sta.h"

static const char *TAG = "herdr";

/* Stack for `herdr_poll`, set in herdr_client_start. Measured on this board:
 * 4096 B overflowed the guard on the very first poll (stack protection fault
 * inside _malloc_r, reached from the JSON/HTTP path), so 8192 B it is.
 *   poll_ctx (stack local)                                  2056 B
 *     = char[2048] + size_t(4) + bool(1) + 3 B padding (ILP32)
 *   call frames: esp_http_client_perform + lwip + cJSON + esp_log  >2 KB
 * The body buffer dominates by design (no malloc in the poll path); a body that
 * would overflow it is flagged via poll_ctx.truncated and counted as a failed
 * poll, never silently truncated. The live bridge body measures ~224 B, so
 * 2048 B is ~9x headroom. Shrink the buffer rather than the timeout if this
 * ever gets tight again.
 */
#define HERDR_POLL_STACK 8192

static char s_url[128];

static herdr_status_t    s_state; /* guarded by s_lock */
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static bool              s_started;

static int      s_fail;         /* consecutive failed polls */
static uint32_t s_last_log_gen; /* gen reported by the last gen-level log line */
static bool     s_gen_logged;

/* Body collector for one poll; lives on the poll task's stack. */
struct poll_ctx {
    char   buf[2048];
    size_t len;
    bool   truncated;
};

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    struct poll_ctx *ctx = (struct poll_ctx *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != NULL && evt->data_len > 0) {
        size_t n = (size_t)evt->data_len;

        if (n >= sizeof ctx->buf - ctx->len) {
            n = sizeof ctx->buf - 1 - ctx->len; /* keep room for the NUL terminator */
            ctx->truncated = true;
        }
        memcpy(ctx->buf + ctx->len, evt->data, n);
        ctx->len += n;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

static herdr_agent_state_t state_from_json(const cJSON *item)
{
    const char *s = cJSON_GetStringValue(item); /* NULL for non-strings */

    if (s == NULL) {
        return HERDR_ST_UNKNOWN;
    }
    if (strcmp(s, "blocked") == 0) {
        return HERDR_ST_BLOCKED;
    }
    if (strcmp(s, "working") == 0) {
        return HERDR_ST_WORKING;
    }
    if (strcmp(s, "done") == 0) {
        return HERDR_ST_DONE;
    }
    if (strcmp(s, "idle") == 0) {
        return HERDR_ST_IDLE;
    }
    return HERDR_ST_UNKNOWN;
}

/* Parses one /state body. False for anything that counts as a failed poll:
 * unsupported wire version, malformed body, missing agents array. */
static bool parse_state(const char *text, herdr_status_t *out)
{
    memset(out, 0, sizeof *out);

    cJSON *root = cJSON_Parse(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "poll: body is not a JSON object");
        cJSON_Delete(root);
        return false;
    }

    cJSON *jv = cJSON_GetObjectItem(root, "v");
    if (!cJSON_IsNumber(jv) || jv->valueint != 1) {
        char vstr[32];

        if (jv == NULL) {
            snprintf(vstr, sizeof vstr, "missing");
        } else if (cJSON_IsString(jv)) {
            snprintf(vstr, sizeof vstr, "%s", cJSON_GetStringValue(jv));
        } else if (cJSON_IsNumber(jv)) {
            snprintf(vstr, sizeof vstr, "%d", jv->valueint);
        } else {
            snprintf(vstr, sizeof vstr, "json type %d", jv->type);
        }
        ESP_LOGW(TAG, "unsupported wire version %s", vstr);
        cJSON_Delete(root);
        return false;
    }

    cJSON *ja = cJSON_GetObjectItem(root, "agents");
    if (!cJSON_IsArray(ja)) {
        ESP_LOGW(TAG, "poll: agents is not an array");
        cJSON_Delete(root);
        return false;
    }

    cJSON *jg = cJSON_GetObjectItem(root, "gen");
    out->gen = cJSON_IsNumber(jg) ? (uint32_t)jg->valueint : 0;

    /* stale == false means the bridge is reaching herdr right now. */
    cJSON *js = cJSON_GetObjectItem(root, "stale");
    out->stale = cJSON_IsBool(js) && cJSON_IsTrue(js);
    out->online = !out->stale;

    int total = cJSON_GetArraySize(ja);
    int shown = 0;

    for (int i = 0; i < total && shown < HERDR_MAX_AGENTS; i++) {
        cJSON *je = cJSON_GetArrayItem(ja, i);

        if (!cJSON_IsObject(je)) {
            continue;
        }
        herdr_agent_t *a = &out->agents[shown];
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(je, "label"));
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(je, "kind"));

        /* The bridge already ASCII-sanitised and truncated both strings. */
        strlcpy(a->label, label ? label : "", sizeof a->label);
        strlcpy(a->kind, kind ? kind : "", sizeof a->kind);
        a->state = state_from_json(cJSON_GetObjectItem(je, "status"));
        a->focused = cJSON_IsTrue(cJSON_GetObjectItem(je, "focus"));
        shown++;
    }

    out->count = shown;
    out->overflow = total - (total < CONFIG_HERDR_UI_MAX_AGENTS ? total : CONFIG_HERDR_UI_MAX_AGENTS);

    cJSON_Delete(root);
    return true;
}

static void publish(const herdr_status_t *in)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *in;
    xSemaphoreGive(s_lock);
}

/* Bridge unreachable: keep the last agent rows so the list stays informative,
 * but tell the UI the data is no longer live. */
static void publish_offline(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.online = false;
    s_state.stale = true;
    xSemaphoreGive(s_lock);
}

static void log_gen_if_changed(const herdr_status_t *s)
{
    if (s_gen_logged && s->gen == s_last_log_gen) {
        return;
    }
    s_gen_logged = true;
    s_last_log_gen = s->gen;

    int blocked = 0, working = 0, done = 0, idle = 0;

    for (int i = 0; i < s->count; i++) {
        switch (s->agents[i].state) {
        case HERDR_ST_BLOCKED: blocked++; break;
        case HERDR_ST_WORKING: working++; break;
        case HERDR_ST_DONE:    done++; break;
        default:               idle++; break; /* IDLE + UNKNOWN: both read "idle" on the panel */
        }
    }
    ESP_LOGI(TAG, "gen=%u online=%d agents=%d blocked=%d working=%d done=%d idle=%d",
             (unsigned)s->gen, (int)s->online, s->count, blocked, working, done, idle);
}

static void poll_failed(void)
{
    s_fail++;
    ESP_LOGW(TAG, "poll failed (%d consecutive)", s_fail);
    if (s_fail >= 3) {
        publish_offline();
    }
}

static void poll_once(void)
{
    struct poll_ctx ctx = { 0 }; /* 2064 B: the biggest stack consumer (see budget above) */
    const esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 3000,
        .method = HTTP_METHOD_GET,
        .event_handler = http_evt,
        .user_data = &ctx,
        .disable_auto_redirect = true,
    };
    herdr_status_t next;

    /* Client per poll: at 1 Hz there is no persistent state worth getting wrong. */
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        poll_failed();
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "GET %s -> %d (%d bytes)", s_url, status, (int)ctx.len);

    if (err != ESP_OK || status != 200 || ctx.truncated) {
        poll_failed();
        return;
    }
    if (!parse_state(ctx.buf, &next)) {
        poll_failed();
        return;
    }

    s_fail = 0;
    publish(&next);
    log_gen_if_changed(&next);
}

static void herdr_poll(void *arg)
{
    (void)arg;

    for (;;) {
        while (!wifi_sta_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        poll_once();

        /* Sleep for the poll period, but let a screen tap cut it short: the
         * notification from herdr_client_poll_now() wakes this wait immediately,
         * so a tap forces a fresh poll instead of waiting out the period. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_HERDR_POLL_PERIOD_MS));
    }
}

void herdr_client_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;

    snprintf(s_url, sizeof s_url, "http://%s:%d/state", CONFIG_HERDR_BRIDGE_HOST, CONFIG_HERDR_BRIDGE_PORT);

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex allocation failed");
        s_started = false;
        return;
    }
    /* C6 has a single high-performance core; task affinity would be pointless. */
    if (xTaskCreate(herdr_poll, "herdr_poll", HERDR_POLL_STACK, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "herdr_poll task creation failed");
        s_task = NULL;
        s_started = false;
        return;
    }

    ESP_LOGI(TAG, "polling %s every %d ms", s_url, CONFIG_HERDR_POLL_PERIOD_MS);
}

bool herdr_client_get(herdr_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_state, sizeof *out);
    xSemaphoreGive(s_lock);
    return true;
}

void herdr_client_poll_now(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

#if CONFIG_HERDR_UI_MAX_AGENTS > HERDR_MAX_AGENTS
#error "CONFIG_HERDR_UI_MAX_AGENTS exceeds HERDR_MAX_AGENTS"
#endif