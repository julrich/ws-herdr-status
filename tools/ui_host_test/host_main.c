/* Host render harness for the herdr companion UI (AGENTS.md §9a).
 *
 * Compiles main/ui_companion.c *unmodified* against a host build of LVGL 8.4,
 * registers a 172x320 RGB565 display whose flush callback blits into an RGB888
 * framebuffer, then drives a fixed set of scenarios and asserts on pixels and
 * on the text the widgets actually carry. No OCR: labels are found by walking
 * the active screen's object tree and read with lv_label_get_text().
 *
 * The two functions the UI expects from the device (herdr_client_get /
 * herdr_client_poll_now) are stubbed here; everything else is LVGL.
 *
 * Build + run: tools/ui_host_test/run.sh (from the repo root).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "herdr_status_types.h"
#include "ui_stats.h"
#include "ui_companion.h"

/* ------------------------------------------------------------------ */
/* Device geometry. The same framebuffer size (172*320) covers both
 * orientations; the stride is per scenario, and the probe constants mirror the
 * two branches of ui_layout_init() in main/ui_companion.c. */
/* ------------------------------------------------------------------ */
#define FB_PIXELS (172 * 320)

/* portrait (matches ui_layout_init's !split branch) */
#define BODY_CX 86
#define BODY_CY 108       /* the face's box centre, as ui_companion.c places it */
#define BODY_D  120
#define LIST_Y0 190      /* portrait list geometry, from ui_layout_init() */
#define BAR_Y   186      /* the divider between the panel and the list (TINT_H) */
#define BAR_H   5
#define LIST_ROW_H 24
#define RING_D  138
#define MOUTH_CY 126

#define L_LIST_Y0  24
#define L_ROW_H    22
#define L_SUMMARY_Y 158

/* Frame-by-frame width/height of the scene being drawn. */
static int g_w = 172;
static int g_h = 320;

/* Palette given as RGB888, exactly as in the plan. The panel is RGB565 and
 * LVGL widens back to RGB888 with bit replication, so the expected value is
 * pushed through the same lv_color_hex()/lv_color_to32() round trip before it
 * is compared (see expect_rgb()). */
#define COL_BLOCKED 0xE0A33E
#define COL_WORKING 0x4EA8FF
#define COL_DONE    0x4ADE80
#define COL_IDLE    0xC9D4E2
#define COL_SLEEP   0x3A4657
#define COL_OFFLINE 0x2A3442
/* The face's own features (eyes, mouth, flat mouth) are painted in the screen
 * background colour, so they read as holes cut in the face disc. */
#define COL_DIM     0x6F7D90
#define COL_BG      0x0B0F14
#define COL_FACE    0x0B0F14

#define HEADLINE_Y_MIN 0
#define HEADLINE_Y_MAX 30
#define SCR_H_HALF  160  /* portrait half of the 320 px long side            */
#define STATS_Y0    30   /* portrait stats line 0: headline_y 10 + 20        */
#define STATS_ROW_H 14   /* ui_companion.c's per-line step                  */

#define SUMMARY_Y_MIN  295
#define SUMMARY_Y_MAX  320

/* The firmware gets this from Kconfig (sdkconfig.h); there is no sdkconfig on
 * the host, so pin the same default the plan uses. run.sh passes it too, which
 * also keeps ui_companion.c buildable here without a fallback of its own. */
#ifndef CONFIG_HERDR_UI_MAX_AGENTS
#define CONFIG_HERDR_UI_MAX_AGENTS 4
#endif

/* ------------------------------------------------------------------ */
/* Framebuffer + virtual clock                                         */
/* ------------------------------------------------------------------ */

static uint32_t   g_fb[FB_PIXELS];        /* 0xRRGGBB per pixel */
static lv_color_t g_draw_buf[FB_PIXELS];
uint32_t          g_tick;                 /* virtual clock, advanced by render() */

/* ------------------------------------------------------------------ */
/* Stubs for the device-side client                                    */
/* ------------------------------------------------------------------ */

static herdr_status_t g_status;
static int            g_poll_now_calls;

bool herdr_client_get(herdr_status_t *out)
{
    if(out == NULL) return false;
    *out = g_status;
    return true;
}

void herdr_client_poll_now(void)
{
    g_poll_now_calls++;
}

/* The diagnostics and session getters ui_companion.c reads. Their real
 * implementations live behind esp_* (main/herdr_client.c, ui_rotation.c,
 * ui_input.c, ui_device.c); here they are plain settable state. */
static herdr_link_stats_t  g_link;
static herdr_sessions_t    g_sessions;

void herdr_client_stats(herdr_link_stats_t *out)
{
    if(out != NULL) *out = g_link;
}

bool herdr_stats_get(herdr_sessions_t *out)
{
    if(out == NULL) return false;
    *out = g_sessions;
    return g_sessions.valid;
}

uint32_t ui_device_free_heap(void)
{
    return 180u * 1024u;
}

uint32_t ui_device_min_free_heap(void)
{
    return 150u * 1024u;
}

/* ------------------------------------------------------------------ */
/* Display + pointer input                                             */
/* ------------------------------------------------------------------ */

/* RGB565 -> the RGB888 the framebuffer holds. LVGL 9's lv_color_t is RGB888 while a
 * 16-bit display's pixels are RGB565, so every pixel makes this trip; the same
 * helper on the expectation side keeps the two in step. */
static uint32_t widen565(uint16_t c)
{
    const uint32_t r = (c >> 11) & 0x1Fu;
    const uint32_t g = (c >> 5)  & 0x3Fu;
    const uint32_t b = c         & 0x1Fu;

    return ((r << 3) << 16) | ((g << 2) << 8) | (b << 3);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* The pixels in the flush's byte map are in the display's native format — RGB565
     * at LV_COLOR_DEPTH 16, the same as the device's buffer. */
    const uint16_t *px = (const uint16_t *)px_map;
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);

    for(int32_t y = 0; y < h; y++) {
        for(int32_t x = 0; x < w; x++) {
            const uint32_t rgb = widen565(px[y * w + x]);
            const int32_t  fx  = area->x1 + x;
            const int32_t  fy  = area->y1 + y;
            if(fx >= 0 && fx < g_w && fy >= 0 && fy < g_h) {
                g_fb[fy * g_w + fx] = rgb;
            }
        }
    }
    lv_display_flush_ready(disp);
}

/* Scripted pointer input: no presses unless a scenario posts one. */
static lv_indev_state_t g_ptr_state = LV_INDEV_STATE_REL;
static lv_point_t       g_ptr_pos;

static void post_press(int x, int y)
{
    g_ptr_pos.x = (lv_coord_t)x;
    g_ptr_pos.y = (lv_coord_t)y;
    g_ptr_state = LV_INDEV_STATE_PR;
}

static void post_release(void)
{
    g_ptr_state = LV_INDEV_STATE_REL;
}

static void render(int iterations);

/* A click at (x, y) through the scripted pointer: press, one tick, release, one
 * tick. A tick is 30 ms of the virtual clock. */
/* A click through the scripted pointer. LVGL 9 gives every indev its own read timer
 * (~60 ms in this build) rather than reading at each lv_timer_handler() as v8 did, so
 * the press has to be held across a read or it is simply never seen. */
static void click_at(int x, int y)
{
    post_press(x, y);
    render(3);                       /* 90 ms: at least one read lands on the press */
    post_release();
    render(2);
}


/* Two clicks in the same half, 30 ms apart — inside the UI's double window. */
static void double_click_at(int x, int y)
{
    click_at(x, y);
    click_at(x, y);
}

/* A double tap as a finger does it: two taps in the same half, but not on the same
 * pixel. The harness used to only ever tap one point, which hid a device bug: LVGL
 * resets its double-click detection when the taps are further apart than the indev's
 * scroll_limit, so a real double never registered and the stats view was unreachable
 * on the panel. */
static void double_click_apart(int x, int y)
{
    click_at(x, y);
    click_at(x + 18, y + 22);
}

/* The two halves: the face keeps the first half along the long axis, the list the
 * second. Portrait splits at y=160, landscape at x=160 (ui_companion.c's
 * screen_event_cb). */
#define FACE_HALF_X  BODY_CX
#define FACE_HALF_Y  BODY_CY                 /* 108: above the split */
#define LIST_HALF_X  BODY_CX
#define LIST_HALF_Y  (SCR_H_HALF + 20)       /* 180: below it */

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = g_ptr_pos;
    data->state = g_ptr_state;
}

static void harness_init(void)
{
    lv_init();

    /* LVGL 9 dropped the driver structs: a display is created from its size and
     * configured with setters, and its draw buffer is attached separately. */
    lv_display_t *disp = lv_display_create(g_w, g_h);
    if(disp == NULL) {
        fprintf(stderr, "host: display creation failed\n");
        exit(2);
    }
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_draw_buf, NULL, FB_PIXELS * (int)sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    if(indev == NULL) {
        fprintf(stderr, "host: indev creation failed\n");
        exit(2);
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    /* The same two thresholds main.c sets on the device's indev (see the comment
     * there): without them a double tap only counts when it lands in the same
     * 10 px, which is why the device could not open the stats view. */
    lv_indev_set_scroll_limit(indev, 40);
    lv_indev_set_long_press_time(indev, 600);
}

/* Deterministic virtual time: 30 ms per LVGL tick, never wall-clock. */
static void render(int iterations)
{
    if(iterations <= 0) iterations = 20;
    for(int i = 0; i < iterations; i++) {
        g_tick += 30;
        lv_tick_inc(30);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

/* ------------------------------------------------------------------ */
/* Scenario plumbing                                                   */
/* ------------------------------------------------------------------ */

/* ui_companion.c deletes its own tick timer when it rebuilds the UI
 * (LVGL 9 hides the timer struct, and reaching into it was never right). */


static void reset_scene(void)
{
    lv_scr_load_anim(lv_obj_create(NULL), LV_SCR_LOAD_ANIM_NONE, 0, 0, true);

    /* Sentinel: no expected palette colour is 0x000000, so an unpainted pixel
     * fails the assertions instead of accidentally matching an old frame. */
    memset(g_fb, 0, sizeof g_fb);

    g_poll_now_calls = 0;
    post_release();
}

static uint32_t s_gen;

static void set_agent(int i, const char *label, const char *kind, herdr_agent_state_t state, bool focused)
{
    herdr_agent_t *a = &g_status.agents[i];
    memset(a, 0, sizeof *a);
    /* The wire carries a pane id; the stats endpoint matches rows by it. */
    snprintf(a->id, sizeof a->id, "w1:p%d", i + 1);
    snprintf(a->label, sizeof a->label, "%s", label);
    snprintf(a->kind, sizeof a->kind, "%s", kind);
    a->state   = state;
    a->focused = focused;
}

/* Mirrors the wire shape the bridge emits, written straight into the struct. */
static void load_scenario(const char *name)
{
    memset(&g_status, 0, sizeof g_status);
    memset(&g_link, 0, sizeof g_link);
    memset(&g_sessions, 0, sizeof g_sessions);

    g_link.gen      = 17285;
    g_link.polls    = 400;
    g_link.rtt_ms   = 12;
    g_link.online   = true;
    g_status.online = true;
    g_status.stale  = false;
    g_status.gen    = ++s_gen;

    if(strcmp(name, "blocked") == 0 || strcmp(name, "burst") == 0 || strcmp(name, "sweat") == 0
            || strcmp(name, "land_blocked") == 0 || strcmp(name, "land_burst") == 0) {
        set_agent(0, "PoC", "omp", HERDR_ST_BLOCKED, true);
        g_status.count = 1;
    }
    else if(strcmp(name, "working") == 0 || strcmp(name, "alive") == 0 || strcmp(name, "glint") == 0
            || strcmp(name, "motes") == 0
            || strcmp(name, "land_working") == 0
            || strcmp(name, "view_switch") == 0 || strcmp(name, "overlay") == 0
            || strcmp(name, "flourish") == 0) {
        set_agent(0, "PoC", "omp", HERDR_ST_WORKING, true);
        set_agent(1, "Docs pass", "claude", HERDR_ST_IDLE, false);
        g_status.count = 2;
    }
    else if(strcmp(name, "done") == 0 || strcmp(name, "party") == 0) {
        set_agent(0, "Finished review", "omp", HERDR_ST_DONE, false);
        g_status.count = 1;
    }
    else if(strcmp(name, "stats") == 0 || strcmp(name, "live_rows") == 0) {
        /* The desk's real agents and their real /stats numbers, which is where the
         * formatting questions came from (four sessions, 1.1G input tokens between
         * them, one session quiet overnight). */
        set_agent(0, "Waveshare", "omp",  HERDR_ST_WORKING, true);
        set_agent(1, "rm Upgrade", "omp", HERDR_ST_IDLE,    false);
        set_agent(2, "Racer", "omp",      HERDR_ST_IDLE,    false);
        set_agent(3, "tsnm", "omp",       HERDR_ST_IDLE,    false);
        g_status.count = 4;

        static const struct { uint32_t in, out, calls, msgs, age, cost; } live[4] = {
            { 322361294u, 723439u,  804,  790,     6, 4564666u },   /* $4.56 */
            { 522287761u, 990343u, 1280, 1142,  9602, 5376479u },   /* $5.38 */
            {  14171410u, 159329u,  173,  101,  8057,  302797u },   /* $0.30 */
            { 239267051u, 463330u,  735,  538, 103149, 2358504u },  /* $2.36 */
        };

        g_sessions.valid      = true;
        g_sessions.sessions   = 4;
        g_sessions.tokens_in  = 1097192927u;
        g_sessions.tokens_out = 2330905u;
        g_sessions.messages   = 2567;
        g_sessions.tool_calls = 2988;
        g_sessions.age_s      = 11;
        g_sessions.cost_micro = 12745891u;   /* $12.75 across the four */

        for(int i = 0; i < 4; i++) {
            g_sessions.per[i].tokens_in  = live[i].in;
            g_sessions.per[i].tokens_out = live[i].out;
            g_sessions.per[i].tool_calls = live[i].calls;
            g_sessions.per[i].messages   = live[i].msgs;
            g_sessions.per[i].age_s      = live[i].age;
            g_sessions.per[i].cost_micro = live[i].cost;
            g_sessions.per[i].tokens_per_s = 85;   /* what the bridge reports */
            snprintf(g_sessions.per[i].model, sizeof g_sessions.per[i].model, "deepseek-v4.1-f");
        }
    }
    else if(strcmp(name, "idle") == 0 || strcmp(name, "blush") == 0) {
        set_agent(0, "Waiting", "omp", HERDR_ST_IDLE, false);
        g_status.count = 1;
    }
    else if(strcmp(name, "empty") == 0 || strcmp(name, "sleep_z") == 0 || strcmp(name, "land_empty") == 0) {
        g_status.count = 0;
    }
    else if(strcmp(name, "offline") == 0) {
        set_agent(0, "PoC", "omp", HERDR_ST_WORKING, true);
        set_agent(1, "Docs pass", "claude", HERDR_ST_IDLE, false);
        g_status.count  = 2;
        g_status.online = false;
    }
    else if(strcmp(name, "overflow") == 0 || strcmp(name, "paging") == 0) {
        static const char *labels[HERDR_MAX_AGENTS] = {
            "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"
        };
        for(int i = 0; i < HERDR_MAX_AGENTS; i++) {
            set_agent(i, labels[i], "omp", HERDR_ST_WORKING, i == 0);
        }
        g_status.count    = HERDR_MAX_AGENTS;
        g_status.overflow = HERDR_MAX_AGENTS - CONFIG_HERDR_UI_MAX_AGENTS;
    }
    else if(strcmp(name, "tap") == 0) {
        set_agent(0, "PoC", "omp", HERDR_ST_WORKING, true);
        set_agent(1, "Docs pass", "claude", HERDR_ST_IDLE, false);
        g_status.count = 2;
    }
    else {
        fprintf(stderr, "host: unknown scenario \"%s\"\n", name);
        exit(2);
    }
}

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int  failed;
    char detail[256];
} result_t;

static void failf(result_t *r, const char *fmt, ...)
{
    va_list ap;
    if(r->failed) return; /* keep the first failure */
    r->failed = 1;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);
}

#define EXPECT(r, cond, ...)                                                   \
    do {                                                                       \
        if(!(cond)) failf((r), __VA_ARGS__);                                   \
    } while(0)

/* rgb888 -> the exact RGB888 value LVGL stores after going through RGB565. */
static uint32_t expect_rgb(uint32_t rgb888)
{
    /* The display renders RGB565, so an expectation makes the same round trip the
     * pixel does before it reaches the framebuffer. */
    return widen565(lv_color_to_u16(lv_color_hex(rgb888)));
}

static uint32_t pixel_at(int x, int y)
{
    if(x < 0 || x >= g_w || y < 0 || y >= g_h) return 0xFFFFFFFFu;
    return g_fb[y * g_w + x];
}

static bool assert_pixel(int x, int y, uint32_t rgb888)
{
    return pixel_at(x, y) == expect_rgb(rgb888);
}

static int count_in_band(int y0, int y1, uint32_t want)
{
    int n = 0;
    if(y0 < 0) y0 = 0;
    if(y1 >= g_h) y1 = g_h - 1;
    for(int y = y0; y <= y1; y++) {
        for(int x = 0; x < g_w; x++) {
            if(g_fb[y * g_w + x] == want) n++;
        }
    }
    return n;
}

static bool assert_pixel_count(int y0, int y1, uint32_t rgb888, int min_count)
{
    return count_in_band(y0, y1, expect_rgb(rgb888)) >= min_count;
}

/* A label counts only if it and every ancestor are visible: hidden list rows
 * keep whatever coordinates they last had, which can collide with a text band. */
static bool obj_is_visible(const lv_obj_t *obj)
{
    while(obj != NULL) {
        if(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
        obj = lv_obj_get_parent(obj);
    }
    return true;
}

/* Find the first visible label whose top edge sits in [y_min, y_max] of the
 * active screen, in screen-absolute coordinates. */
static lv_obj_t *find_label_rec(lv_obj_t *obj, int y_min, int y_max)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if(child == NULL) continue;
        if(lv_obj_check_type(child, &lv_label_class) && obj_is_visible(child)) {
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if(a.y1 >= y_min && a.y1 <= y_max) return child;
        }
        lv_obj_t *found = find_label_rec(child, y_min, y_max);
        if(found != NULL) return found;
    }
    return NULL;
}

static lv_obj_t *find_label_at(int y_min, int y_max)
{
    lv_obj_t *scr = lv_screen_active();
    if(scr == NULL) return NULL;
    return find_label_rec(scr, y_min, y_max);
}

/* Exact-string comparison of the label living in a y band; fills `got`. */
/* Any label in the band carrying exactly this text. A stats row holds two labels (a
 * left column and a right-aligned figure), so matching the band's first label would
 * depend on which happened to be created first. */
/* Ink inside the face's box. The kawaii face draws strokes (lids, eyes, mouth,
 * blush) over the background, so "the face is drawn" means pixels that are not the
 * background. Callers let the mood-change burst settle first, or its wash would
 * count. */
static int face_ink(void)
{
    const int      x0 = BODY_CX - BODY_D / 2, x1 = BODY_CX + BODY_D / 2;
    const int      y0 = BODY_CY - BODY_D / 2, y1 = BODY_CY + BODY_D / 2;
    /* The box's own corner is the mood panel behind the face, whatever colour that
     * is this mood: ink is what differs from it. */
    const uint32_t bg = pixel_at(x0, y0);
    int            n  = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            if(x >= 0 && x < g_w && y >= 0 && y < g_h && g_fb[y * g_w + x] != bg) n++;
        }
    }
    return n;
}

static bool assert_face_drawn(int min_px, char *why, size_t why_sz)
{
    const int n = face_ink();

    snprintf(why, why_sz, "%d ink pixels in the face's box", n);
    return n >= min_px;
}

static bool assert_text_in(lv_obj_t *obj, int y_min, int y_max, const char *expected)
{
    uint32_t n = lv_obj_get_child_cnt(obj);

    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if(child == NULL) continue;
        if(lv_obj_check_type(child, &lv_label_class) && obj_is_visible(child)) {
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if(a.y1 >= y_min && a.y1 <= y_max && strcmp(lv_label_get_text(child), expected) == 0) {
                return true;
            }
        }
        if(assert_text_in(child, y_min, y_max, expected)) return true;
    }
    return false;
}

/* Collects the label boxes whose top edge falls in the band. A stats row has two:
 * the left column and the right-aligned figure. */
static void band_boxes(lv_obj_t *obj, int y_min, int y_max, lv_area_t *box, int *n, int max)
{
    uint32_t cnt = lv_obj_get_child_cnt(obj);

    for(uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if(child == NULL) continue;

        if(lv_obj_check_type(child, &lv_label_class) && obj_is_visible(child)) {
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if(a.y1 >= y_min && a.y1 <= y_max && *n < max) {
                box[(*n)++] = a;
            }
        }
        band_boxes(child, y_min, y_max, box, n, max);
    }
}

/* The two columns of a stats row must not run into each other, and the right one has
 * to stay on screen. LVGL does not clip a label, so an over-long string silently
 * overlaps its neighbour: comparing the boxes is the only way to catch it. Reports
 * the gap and the right edge, so a failure says how much has to be trimmed. */
static bool assert_columns(int y_min, int y_max, int min_gap, char *why, size_t why_sz)
{
    lv_obj_t  *scr = lv_screen_active();
    lv_area_t  box[4];
    int        n = 0, gap = -1, edge = 0;

    if(scr == NULL) {
        snprintf(why, why_sz, "no screen");
        return false;
    }

    band_boxes(scr, y_min, y_max, box, &n, 4);

    if(n >= 2) {
        /* Leftmost is the label column, rightmost the figure column. */
        lv_area_t left = box[0], right = box[0];
        for(int i = 1; i < n; i++) {
            if(box[i].x1 < left.x1)   left  = box[i];
            if(box[i].x2 > right.x2)  right = box[i];
        }
        gap  = right.x1 - left.x2;
        edge = right.x2;
    }

    snprintf(why, why_sz, "gap=%d px, right edge=%d (screen %d)", gap, edge, g_w);
    return gap >= min_gap && edge <= g_w - 4;
}

static bool assert_text(int y_min, int y_max, const char *expected, char *got, size_t got_sz)
{
    lv_obj_t *scr = lv_screen_active();

    if(scr != NULL && assert_text_in(scr, y_min, y_max, expected)) {
        snprintf(got, got_sz, "%s", expected);
        return true;
    }

    lv_obj_t   *lbl = find_label_at(y_min, y_max);
    const char *txt = (lbl != NULL) ? lv_label_get_text(lbl) : NULL;
    if(txt == NULL) txt = "(no label)";
    snprintf(got, got_sz, "%s", txt);
    return false;
}

/* The colour a label is actually drawn in, found by the text it carries. The row's
 * figures are specified colours, so this is a behaviour, not an implementation detail —
 * and it is the only way to catch a later style write undoing one (which happened). */
static bool label_colour_in(lv_obj_t *obj, const char *text, uint32_t rgb888)
{
    uint32_t n = lv_obj_get_child_cnt(obj);

    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if(child == NULL) continue;

        if(lv_obj_check_type(child, &lv_label_class) && obj_is_visible(child) &&
           strcmp(lv_label_get_text(child), text) == 0) {
            const lv_color_t c = lv_obj_get_style_text_color(child, LV_PART_MAIN);
            return lv_color_to_int(c) == (int)(rgb888 & 0xFFFFFFu);
        }
        if(label_colour_in(child, text, rgb888)) return true;
    }
    return false;
}

/* Suffix match over every label in the band: a row holds both its name and its status
 * figure, and which of them comes first is not the check's business. */
static bool assert_text_suffix_in(lv_obj_t *obj, int y_min, int y_max, const char *suffix)
{
    uint32_t n = lv_obj_get_child_cnt(obj);

    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if(child == NULL) continue;

        if(lv_obj_check_type(child, &lv_label_class) && obj_is_visible(child)) {
            lv_area_t   a;
            const char *t;
            size_t      tl, sl;

            lv_obj_get_coords(child, &a);
            t  = lv_label_get_text(child);
            tl = strlen(t);
            sl = strlen(suffix);
            if(a.y1 >= y_min && a.y1 <= y_max && tl >= sl && strcmp(t + tl - sl, suffix) == 0) {
                return true;
            }
        }
        if(assert_text_suffix_in(child, y_min, y_max, suffix)) return true;
    }
    return false;
}

static bool assert_text_suffix(int y_min, int y_max, const char *suffix, char *got, size_t got_sz)
{
    lv_obj_t   *lbl = find_label_at(y_min, y_max);
    const char *txt = (lbl != NULL) ? lv_label_get_text(lbl) : NULL;
    size_t      tl, sl;
    if(txt == NULL) txt = "(no label)";
    snprintf(got, got_sz, "%s", txt);
    tl = strlen(txt);
    sl = strlen(suffix);
    return tl >= sl && strcmp(txt + tl - sl, suffix) == 0;
}

/* ------------------------------------------------------------------ */
/* Idle-animation probes                                               */
/*                                                                     */
/* The face, the eyes and the mouth are all painted in COL_FACE, which  */
/* through RGB565 is the same value as the background: they are "holes" */
/* in the face disc. So these probes look for COL_FACE pixels *inside*  */
/* the disc (which is body-coloured) rather than for a distinct colour, */
/* and every probe stays off the mood-change burst: ripple pixels are    */
/* blended at RIPPLE_OPA 220, so they never equal an exact palette       */
/* value, and the background tint only ever moves the background.        */
/* ------------------------------------------------------------------ */

/* Widest row of the face, as an observable: the maximum first..last span of
 * body-coloured pixels over all rows. Independent of the bob (a whole-row
 * translation) and of the eyes/mouth holes (they are inside the span), and the
 * x window excludes the ring/sweep at 17..155 so only the face can contribute;
 * ripple pixels are blended, never an exact palette value. */
static int body_max_width(uint32_t mood_rgb)
{
    uint32_t want = expect_rgb(mood_rgb);
    int      best = 0;

    for(int y = 44; y <= 172; y++) {
        int first = -1, last = -1;
        for(int x = 24; x <= 148; x++) {
            if(pixel_at(x, y) == want) {
                if(first < 0) first = x;
                last = x;
            }
        }
        if(first >= 0 && last - first + 1 > best) best = last - first + 1;
    }
    return best;
}

/* Face-coloured pixels in the mouth band, restricted to the face disc
 * (radius 54 keeps the probe clear of the disc's own softly rasterised edge).
 * x 56..116 spans the whole smile: the eyes sit above y 110. */
static int mouth_pixels(void)
{
    int n = 0;

    for(int y = 110; y <= 168; y++) {
        for(int x = 56; x <= 116; x++) {
            int dx = x - BODY_CX, dy = y - BODY_CY;
            if(dx * dx + dy * dy <= 54 * 54 && pixel_at(x, y) == expect_rgb(COL_FACE)) n++;
        }
    }
    return n;
}

/* Centre x of the left eye (-1 when no eye pixel is visible). x 40..80 holds the
 * left eye at every glance offset; the right eye starts at x 99. */
static int left_eye_centre_x(void)
{
    long sum = 0;
    int  n   = 0;

    for(int y = 89; y <= 107; y++) {
        for(int x = 40; x <= 80; x++) {
            if(pixel_at(x, y) == expect_rgb(COL_FACE)) { sum += x; n++; }
        }
    }
    return n ? (int)(sum / n) : -1;
}

/* Brightest channel of a pixel, 0..255: used to watch the ring's opacity. */
static int pixel_luma_max(int x, int y)
{
    uint32_t c = pixel_at(x, y);
    int      r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);

    return (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
}

/* Any ink in a band: a pixel that is not the settled background. */
static bool band_has_ink(int x0, int x1, int y0, int y1)
{
    uint32_t bg = expect_rgb(COL_BG);

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            uint32_t c = pixel_at(x, y);
            if(c == bg) continue;
            for(int shift = 0; shift <= 16; shift += 8) {
                int d = (int)((c >> shift) & 0xFF) - (int)((bg >> shift) & 0xFF);
                if(d > 12 || d < -12) return true;
            }
        }
    }
    return false;
}

/* Bright pixels in the strip below the face: from just under its box (the face is
 * 100 px across and centred at BODY_CY, so its bottom edge is ~146) down to the mood
 * panel's edge at 186, clear of the agent rows that start at 190. At rest this strip
 * shows the panel and nothing else, and only a burst ring can light it up — the mood
 * in this scenario has no motes, which are the other thing that lives down here. */
static int bright_pixels_below_face(int threshold)
{
    /* The strip is inside the mood panel, which is itself bright in this mood's
     * colour, so the baseline is what the strip looks like without a ring — sampled
     * at its corner, away from where the washer crosses: the strip sits between the
     * face's box (which ends at ~161) and the activity bar at 186. */
    const uint32_t base = pixel_at(20, 164);
    int            n    = 0;

    for(int y = 164; y <= 180; y++) {
        for(int x = 20; x <= 152; x++) {
            const uint32_t c = pixel_at(x, y);

            if(c == base) continue;

            int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
            if(r > threshold || g > threshold || b > threshold) n++;
        }
    }
    return n;
}


/* An exact palette match, counted inside a box. */
static int count_exact(uint32_t rgb888, int x0, int x1, int y0, int y1)
{
    uint32_t want = expect_rgb(rgb888);
    int      n    = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            if(pixel_at(x, y) == want) n++;
        }
    }
    return n;
}

/* The divider between the panel and the list. It is drawn as a solid track in the dim
 * colour, so "drawn" is a pixel count and "not drawn" is none at all — an absence no
 * blend of the burst's ripples can fake, since those never land on an exact colour. */
static bool assert_bar(bool want, char *why, size_t why_sz)
{
    /* The middle three rows of the track: clear of the list's first row below it. */
    const int n = count_exact(COL_DIM, 0, g_w - 1, BAR_Y + 1, BAR_Y + BAR_H - 2);

    if((n > 0) != want) {
        snprintf(why, why_sz, "the divider is %s (%d track px)", n > 0 ? "drawn" : "missing", n);
        return false;
    }
    return true;
}

/* Pixels leaning warm by at least delta on the red channel (cheek blush). */
static int count_warmer(int x0, int x1, int y0, int y1, int delta)
{
    int n = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            uint32_t c = pixel_at(x, y);
            int      r = (int)((c >> 16) & 0xFF), b = (int)(c & 0xFF);
            if(r - b >= delta) n++;
        }
    }
    return n;
}

/* Pixels leaning cool by at least delta on the blue channel (sweat drop). */
static int count_bluer(int x0, int x1, int y0, int y1, int delta)
{
    int n = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            uint32_t c = pixel_at(x, y);
            int      r = (int)((c >> 16) & 0xFF), b = (int)(c & 0xFF);
            if(b - r >= delta) n++;
        }
    }
    return n;
}

/* Cool-white pixels (sparkles/dust): bright in all channels and blue-leaning.
 * The mood colours are either too dark on red or too warm to match, and the
 * headline in the top band is the mood colour, so this isolates COL_SPARKLE. */
static int count_cool_white(int x0, int x1, int y0, int y1)
{
    int n = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            uint32_t c = pixel_at(x, y);
            int      r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
            if(r > 150 && g > 150 && b >= r) n++;
        }
    }
    return n;
}

/* Pale-gold confetti: nothing else in the palette is this bright and this warm. */
/* Where the face lives, so a check can tell its art from the mood view's own
 * trimmings: the kawaii face uses pale colours that would otherwise read as
 * confetti. */
static bool in_face_box(int x, int y)
{
    return x >= BODY_CX - BODY_D / 2 && x <= BODY_CX + BODY_D / 2 &&
           y >= BODY_CY - BODY_D / 2 && y <= BODY_CY + BODY_D / 2;
}

static int count_confetti(void)
{
    int n = 0;

    for(int y = 0; y < g_h; y++) {
        for(int x = 0; x < g_w; x++) {
            if(in_face_box(x, y)) continue;   /* the face draws in these colours */
            uint32_t c = pixel_at(x, y);
            int      r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
            if(r >= 240 && g >= 230 && b <= 215) n++;
        }
    }
    return n;
}

/* Pixels above the background by delta on any channel (dim dust motes). */
static int count_above_bg(int x0, int x1, int y0, int y1, int delta)
{
    uint32_t bg = expect_rgb(COL_BG);
    int      n  = 0;

    for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
            uint32_t c = pixel_at(x, y);
            for(int shift = 0; shift <= 16; shift += 8) {
                if((int)((c >> shift) & 0xFF) - (int)((bg >> shift) & 0xFF) > delta) { n++; break; }
            }
        }
    }
    return n;
}

/* Sample a probe frame by frame and report its range to the caller. */
typedef int (*probe_fn)(void);

static void sample_probe(int iterations, probe_fn probe, int *lo, int *hi)
{
    int mn = 1 << 20, mx = 0;

    for(int i = 0; i < iterations; i++) {
        render(1);
        int v = probe();
        if(v < mn) mn = v;
        if(v > mx) mx = v;
    }
    *lo = mn;
    *hi = mx;
}

/* ------------------------------------------------------------------ */
/* Per-scenario assertions                                             */
/* ------------------------------------------------------------------ */

static void check_blocked(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_pixel_count(5, 28, COL_BLOCKED, 40),
           "headline band y5..28 has %d px of #%06X (want >=40)",
           count_in_band(5, 28, expect_rgb(COL_BLOCKED)), (unsigned)expect_rgb(COL_BLOCKED));
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "NEEDS YOU", got, sizeof got),
           "headline want \"NEEDS YOU\" got \"%s\"", got);
}

static void check_working(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "WORKING", got, sizeof got),
           "headline want \"WORKING\" got \"%s\"", got);

    /* An agent is running, so the divider between the panel and the list is drawn. */
    {
        char why[64];
        EXPECT(r, assert_bar(true, why, sizeof why), "%s", why);
    }
}

static void check_done(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "DONE", got, sizeof got),
           "headline want \"DONE\" got \"%s\"", got);
}

static void check_idle(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "IDLE", got, sizeof got),
           "headline want \"IDLE\" got \"%s\"", got);
}

static void check_empty(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "NO AGENTS", got, sizeof got),
           "headline want \"NO AGENTS\" got \"%s\"", got);
    EXPECT(r, assert_text(SUMMARY_Y_MIN, SUMMARY_Y_MAX, "no agents", got, sizeof got),
           "summary want \"no agents\" got \"%s\"", got);

    /* No agents means nothing for a bar to be about, so the divider goes with them. The
     * burst the mood change started has to be over first: its ripples cross this strip. */
    render(20);
    {
        char why[64];
        EXPECT(r, assert_bar(false, why, sizeof why), "%s", why);
    }
}

static void check_offline(result_t *r)
{
    char got[64];
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(HEADLINE_Y_MIN, HEADLINE_Y_MAX, "OFFLINE", got, sizeof got),
           "headline want \"OFFLINE\" got \"%s\"", got);
    EXPECT(r, assert_text(SUMMARY_Y_MIN, SUMMARY_Y_MAX, "no link", got, sizeof got),
           "summary want \"no link\" got \"%s\"", got);

    render(20);   /* past the mood-change burst, for the reason in check_empty */
    {
        char why[64];
        EXPECT(r, assert_bar(false, why, sizeof why), "%s", why);
    }
}

/* Six agents, four rows. The summary used to tack a "+2" on; paging reports the
 * same overflow with the page marker, so that is what this now checks. */
static void check_overflow(result_t *r)
{
    char got[64];
    EXPECT(r, assert_text_suffix(SUMMARY_Y_MIN, SUMMARY_Y_MAX, " p1/2", got, sizeof got),
           "summary want a page marker \" p1/2\" got \"%s\"", got);
}

static void check_tap(result_t *r)
{
    /* Press and release through the scripted pointer: LVGL's own click handling
     * is the only path a tap takes on the device, so it is the path under test. */
    g_poll_now_calls = 0;
    post_press(BODY_CX, BODY_CY);
    render(3);
    post_release();
    render(4);

    EXPECT(r, g_poll_now_calls == 1,
           "herdr_client_poll_now calls = %d (want 1)", g_poll_now_calls);
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
}

/* A tap must visibly react: the flourish ring expands past the face. Sampled
 * just above the body, where the resting screen is pure background. */
static void check_flourish(result_t *r)
{
    /* Left of the face at its own height, inside the mood panel and clear of the
     * face itself: the expanding ring crosses this spot. Compared against what is
     * here at rest rather than against the background, because the panel is not the
     * background any more. */
    const int probe_x = 22;
    const int probe_y = BODY_CY;

    /* Let the mood-change burst finish first: its rings sweep this exact spot. */
    render(60);
    const uint32_t rest = pixel_at(probe_x, probe_y);

    ui_companion_on_tap();
    render(6); /* 180 ms: the ring has grown past the probe */

    EXPECT(r, pixel_at(probe_x, probe_y) != rest,
           "no flourish ring beside the face after a tap (#%06X)", (unsigned)pixel_at(probe_x, probe_y));

    render(80); /* long after: everything must be back to rest */
    EXPECT(r, pixel_at(probe_x, probe_y) == rest,
           "the flourish never cleared (#%06X, want #%06X)",
           (unsigned)pixel_at(probe_x, probe_y), (unsigned)rest);
}

/* Two views, swiped between. Leaving the mood view must hide the face; coming
 * back must restore it, and the stats view has to be the other one. */
static void check_view_switch(result_t *r)
{
    char got[32];

    EXPECT(r, ui_companion_view() == UI_VIEW_MOOD, "started in view %s", ui_view_name(ui_companion_view()));
    EXPECT(r, assert_text(0, 30, "WORKING", got, sizeof got),
           "mood view headline want \"WORKING\" got \"%s\"", got);

    g_poll_now_calls = 0;
    double_click_at(FACE_HALF_X, FACE_HALF_Y);   /* double tap the face's half */
    /* The tap that opened the double also played the mood's flourish, which paints
     * over the screen for a while: let it finish before judging the new view. */
    render(30);

    EXPECT(r, ui_companion_view() == UI_VIEW_STATS, "double tap gave view %s",
           ui_view_name(ui_companion_view()));
    /* A double tap contains a tap, so exactly one refresh is expected: the first
     * click of the pair. Nothing waits to find out whether a double is coming. */
    EXPECT(r, g_poll_now_calls == 1,
           "a double tap on the face wanted 1 bridge poll, got %d", g_poll_now_calls);
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
    EXPECT(r, assert_text(0, 30, "STATS  1/2", got, sizeof got),
           "stats view title want \"STATS  1/2\" (it marks the page) got \"%s\"", got);

    double_click_at(FACE_HALF_X, FACE_HALF_Y);   /* two views: the next double wraps back */
    render(30);                                  /*  as above: settle the flourish */
    EXPECT(r, ui_companion_view() == UI_VIEW_MOOD, "wrapping gave view %s", ui_view_name(ui_companion_view()));
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }

    double_click_at(FACE_HALF_X, FACE_HALF_Y);
    render(30);
    EXPECT(r, ui_companion_view() == UI_VIEW_STATS, "double tap from mood gave view %s",
           ui_view_name(ui_companion_view()));

    /* Back to the mood view, then open the stats with two taps that are *not* on the
     * same pixel: the way a finger does it, and the case the device failed. */
    double_click_apart(FACE_HALF_X, FACE_HALF_Y - 20);
    render(30);
    EXPECT(r, ui_companion_view() == UI_VIEW_MOOD, "a travelling double tap gave view %s",
           ui_view_name(ui_companion_view()));

    double_click_apart(FACE_HALF_X, FACE_HALF_Y - 20);
    render(30);
    EXPECT(r, ui_companion_view() == UI_VIEW_STATS,
           "two taps 28 px apart should open the stats view, got %s",
           ui_view_name(ui_companion_view()));
}

/* The stats view: the only place the session numbers appear, and until this
 * scenario existed nothing asserted it at all. The numbers are the desk's own, at
 * the magnitudes that made the view unreadable before the G tier and the h/d
 * ages: a billion tokens printed as "1097.1M", an overnight session as "9602s". */
/* The list's status column says what an agent is doing, not just that it is
 * "working": a busy one reports its token rate, an idle one what its session has
 * cost. Driven by moving the fixture's output totals, which is what a /stats refresh
 * looks like to the device. */
static void check_live_rows(result_t *r)
{
    char got[32];

    reset_scene();
    load_scenario("stats");     /* one working agent, three idle, all with costs */
    ui_companion_create();
    render(20);

    /* An idle agent shows its session spend instead of the word "idle". */
    EXPECT(r, assert_text(LIST_Y0 + LIST_ROW_H + 2, LIST_Y0 + LIST_ROW_H + 15, "$5.38", got, sizeof got),
           "the second row (idle) want its spend \"$5.38\", got \"%s\"", got);

    /* The working agent shows the rate the bridge reports — omp's own figure, not one
     * computed here — so it is there with the first /stats the device reads. */
    {
        lv_obj_t *scr = lv_screen_active();

        EXPECT(r, scr != NULL && assert_text_suffix_in(scr, LIST_Y0 + 2, LIST_Y0 + 15, "/s"),
               "the working row want a token rate (\".../s\"), got \"%s\"", got);
    }
    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "working", got, sizeof got) == false,
           "the working row still shows the state word (\"%s\")", got);

    /* ...and it follows the data rather than being set once. */
    g_sessions.per[0].tokens_per_s = 4321;
    g_status.gen++;
    render(20);
    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "4.3k/s", got, sizeof got),
           "the working row did not follow /stats to \"4.3k/s\", got \"%s\"", got);

    /* ...and in the colours asked for: #5CD8FF for the rate, #FFC844 for the money. */
    {
        lv_obj_t *scr = lv_screen_active();

        EXPECT(r, scr != NULL && label_colour_in(scr, "4.3k/s", 0x5CD8FFu),
               "the rate is not drawn in #5CD8FF");
        EXPECT(r, scr != NULL && label_colour_in(scr, "$5.38", 0xFFC844u),
               "the spend is not drawn in #FFC844");
    }
}

/* The stats view: the only place the session figures appear, and now a two-column
 * table rather than one long line per row. The numbers are the desk's own, at the
 * magnitudes that made the view unreadable before the G tier and the h/d ages.
 *
 * Page one is the sessions (tokens and money are what the view is opened for) and
 * page two is the link and the device; a tap on the list's half pages between them. */
static void check_stats(result_t *r)
{
    char got[64];

    double_click_at(FACE_HALF_X, FACE_HALF_Y);   /* double tap into the stats view */
    render(30);                                  /* and let its flourish finish */
    EXPECT(r, ui_companion_view() == UI_VIEW_STATS, "double tap gave view %s",
           ui_view_name(ui_companion_view()));

    EXPECT(r, assert_text(0, 30, "STATS  1/2", got, sizeof got),
           "page marker want \"STATS  1/2\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0, STATS_Y0 + 6, "SESSIONS", got, sizeof got),
           "first page section header want \"SESSIONS\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0, STATS_Y0 + 6, "4", got, sizeof got),
           "the session count want \"4\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + STATS_ROW_H, STATS_Y0 + STATS_ROW_H + 6,
                          "1.0G in  2.3M out", got, sizeof got),
           "tokens row want \"1.0G in  2.3M out\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 2 * STATS_ROW_H, STATS_Y0 + 2 * STATS_ROW_H + 6,
                          "2988 calls  2567 msg", got, sizeof got),
           "activity row want \"2988 calls  2567 msg\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 3 * STATS_ROW_H, STATS_Y0 + 3 * STATS_ROW_H + 6,
                          "$12.75", got, sizeof got),
           "the spend figure want \"$12.75\" — rounded, not truncated — got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 4 * STATS_ROW_H, STATS_Y0 + 4 * STATS_ROW_H + 6,
                          "AGENTS", got, sizeof got),
           "the rows' header want \"AGENTS\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 5 * STATS_ROW_H, STATS_Y0 + 5 * STATS_ROW_H + 6,
                          "Waveshare 322.3M", got, sizeof got),
           "first row want \"Waveshare 322.3M\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 5 * STATS_ROW_H, STATS_Y0 + 5 * STATS_ROW_H + 6,
                          "$4.56", got, sizeof got),
           "first row's cost want \"$4.56\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 6 * STATS_ROW_H, STATS_Y0 + 6 * STATS_ROW_H + 6,
                          "rm Upgrad 522.2M", got, sizeof got),
           "second row (9-char label) want \"rm Upgrad 522.2M\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + 6 * STATS_ROW_H, STATS_Y0 + 6 * STATS_ROW_H + 6,
                          "$5.38", got, sizeof got),
           "second row's cost want \"$5.38\" got \"%s\"", got);

    /* No column may run into its neighbour: the longest left string against the
     * widest figure is what decides the row width. */
    EXPECT(r, assert_columns(STATS_Y0 + 5 * STATS_ROW_H, STATS_Y0 + 5 * STATS_ROW_H + 6, 6, got, sizeof got),
           "the widest agent row's columns collide (%s)", got);
    EXPECT(r, assert_columns(STATS_Y0 + STATS_ROW_H, STATS_Y0 + STATS_ROW_H + 6, 6, got, sizeof got),
           "the tokens row's columns collide (%s)", got);
    EXPECT(r, assert_columns(STATS_Y0 + 2 * STATS_ROW_H, STATS_Y0 + 2 * STATS_ROW_H + 6, 6, got, sizeof got),
           "the activity row's columns collide (%s)", got);

    /* Four agents fill four row lines; the rest must be clear rather than carrying
     * whatever the page before had there. That leak is what this guards. */
    EXPECT(r, assert_text(STATS_Y0 + 9 * STATS_ROW_H, STATS_Y0 + 9 * STATS_ROW_H + 6, "", got, sizeof got),
           "line 10 of the sessions page should be empty, got \"%s\"", got);

    /* Page two: the link and the device, one tap on the list's half away — deferred
     * by the double window, like every list tap. */
    click_at(LIST_HALF_X, LIST_HALF_Y);
    render(20);

    EXPECT(r, assert_text(0, 30, "STATS  2/2", got, sizeof got),
           "second page marker want \"STATS  2/2\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0, STATS_Y0 + 6, "LINK", got, sizeof got),
           "second page section header want \"LINK\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + STATS_ROW_H, STATS_Y0 + STATS_ROW_H + 6, "gen", got, sizeof got),
           "link row label want \"gen\" got \"%s\"", got);
    EXPECT(r, assert_text(STATS_Y0 + STATS_ROW_H, STATS_Y0 + STATS_ROW_H + 6, "17285", got, sizeof got),
           "the link row's figure want \"17285\" got \"%s\"", got);
    EXPECT(r, assert_columns(STATS_Y0 + 9 * STATS_ROW_H, STATS_Y0 + 9 * STATS_ROW_H + 6, 6, got, sizeof got),
           "the widest device row's columns collide (%s)", got);

    /* ...and the session rows must not still be drawn underneath it. */
    EXPECT(r, assert_text(STATS_Y0 + 5 * STATS_ROW_H, STATS_Y0 + 5 * STATS_ROW_H + 6,
                          "Waveshare 322.3M", got, sizeof got) == false,
           "the sessions rows are still on the link page (\"%s\")", got);

    /* Back to the sessions page: the check leaves the frame it writes showing the
     * page someone actually opens the view for. */
    click_at(LIST_HALF_X, LIST_HALF_Y);
    render(20);
    EXPECT(r, assert_text(STATS_Y0 + 3 * STATS_ROW_H, STATS_Y0 + 3 * STATS_ROW_H + 6,
                          "$12.75", got, sizeof got),
           "paging back lost the spend figure (got \"%s\")", got);
}

/* Six agents, four rows: a vertical swipe has to page the list. */
static void check_paging(result_t *r)
{
    char got[32];

    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "> Alpha", got, sizeof got),
           "page 1 row 1 want \"> Alpha\" (the focused agent) got \"%s\"", got);
    EXPECT(r, assert_text(LIST_Y0 + 3 * LIST_ROW_H + 2, LIST_Y0 + 3 * LIST_ROW_H + 15, "Delta", got, sizeof got),
           "page 1 row 4 want \"Delta\" got \"%s\"", got);

    click_at(LIST_HALF_X, LIST_HALF_Y);   /* tap the list's half: forward a page */
    render(20);   /* the page waits out the double window (~350 ms) before it moves */

    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "Echo", got, sizeof got),
           "page 2 row 1 want \"Echo\" got \"%s\"", got);
    EXPECT(r, assert_text(LIST_Y0 + LIST_ROW_H + 2, LIST_Y0 + LIST_ROW_H + 15, "Foxtrot", got, sizeof got),
           "page 2 row 2 want \"Foxtrot\" got \"%s\"", got);
    EXPECT(r, assert_text_suffix(SUMMARY_Y_MIN, SUMMARY_Y_MAX, " p2/2", got, sizeof got),
           "summary want a page marker \" p2/2\" got \"%s\"", got);

    click_at(LIST_HALF_X, LIST_HALF_Y);   /* wraps back to page 1 */
    render(20);
    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "> Alpha", got, sizeof got),
           "wrapping the list gave row 1 \"%s\"", got);

    /* The list's double tap is the overlay's other door, and the list must not move:
     * the tap that opened the double only scheduled a page, and the double cancels
     * it. Rendered well past the double window, so a page that was only deferred
     * would have landed by now. */
    double_click_at(LIST_HALF_X, LIST_HALF_Y);
    render(20);
    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "> Alpha", got, sizeof got),
           "the list's double moved the page: want \"> Alpha\" got \"%s\"", got);
    EXPECT(r, pixel_at(BODY_CX, BODY_CY) < 0x202020u,
           "the list's double did not raise the overlay (#%06X)",
           (unsigned)pixel_at(BODY_CX, BODY_CY));

    double_click_at(LIST_HALF_X, LIST_HALF_Y);   /* again: it goes back down */
    render(20);
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }

    /* And the single does page, once the window has passed. */
    click_at(LIST_HALF_X, LIST_HALF_Y);
    render(20);
    EXPECT(r, assert_text(LIST_Y0 + 2, LIST_Y0 + 15, "Echo", got, sizeof got),
           "a single tap on the list's half did not page: want \"Echo\" got \"%s\"", got);
}

/* The overlay covers the screen while it is on, and leaves nothing behind. */
static void check_overlay(result_t *r)
{
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }

    /* Hold a finger down well past the long-press time main.c sets (600 ms), then
     * let go: a hold that lands exactly on the threshold is classified either way. */
    g_poll_now_calls = 0;
    post_press(BODY_CX, BODY_CY);
    render(30);   /* 900 ms */
    post_release();
    render(4);

    /* The panel is a 94%-opaque wash of COL_BG, so the face must be gone even
     * though the exact pixel is no longer the bare background colour. */
    EXPECT(r, pixel_at(BODY_CX, BODY_CY) != expect_rgb(COL_WORKING),
           "long press did not raise the overlay (#%06X)", (unsigned)pixel_at(BODY_CX, BODY_CY));
    EXPECT(r, pixel_at(BODY_CX, BODY_CY) < 0x202020u,
           "the overlay is not dark (#%06X)", (unsigned)pixel_at(BODY_CX, BODY_CY));

    /* LVGL sends CLICKED on release "regardless to long press" (lv_event.h), and
     * a click would refresh from the bridge. It must not: the press was a long
     * press, and only a real tap is a tap. */
    EXPECT(r, g_poll_now_calls == 0,
           "a long press also read as a tap (%d bridge polls)", g_poll_now_calls);

    post_press(BODY_CX, BODY_CY);   /* a second long press drops it again */
    render(20);
    post_release();
    render(4);
    render(20);   /* let the mood-change burst settle: its wash is not the face */
    {
        char why[64];
        EXPECT(r, assert_face_drawn(60, why, sizeof why), "the face is not drawn (%s)", why);
    }
}

/* The idle face must actually move. WORKING carries all four of the new idle
 * animations (breathe, ring glow, mouth talk, eye glances), so one 5.1 s sample
 * window covers them; each probe must observe at least two distinct values. */
static void check_alive(result_t *r)
{
    char why[64];

    /* The face is a widget with its own animation timer (blinks every 3 s, and its
     * pupils wander), so over 5 s the pixels inside its box have to change. Sampled
     * as a whole-box count rather than one probe: what moves is the component's
     * business, not the harness's. */
    render(20);
    int seen[8], n = 0;

    for(int i = 0; i < 34; i++) {          /* ~1 s of virtual time per sample */
        render(34);
        const int ink = face_ink();
        bool      dup = false;
        for(int j = 0; j < n; j++) if(seen[j] == ink) dup = true;
        if(!dup && n < 8) seen[n++] = ink;
    }

    snprintf(why, sizeof why, "%d distinct ink counts", n);
    EXPECT(r, n >= 3, "the face never moved (%s)", why);
}

/* Mood change must paint the background and wash rings out of the face, then
 * put everything back. Sampled at the burst, not after it. */
static void check_burst(result_t *r)
{
    uint32_t bg_rest = expect_rgb(COL_BG);

    reset_scene();
    load_scenario("burst");
    ui_companion_create();

    /* The mood (and so the burst) is applied by the first 200 ms ui_tick. The ring is
     * sampled across the whole burst rather than at one instant: the smaller face means
     * RIPPLE_D1 is smaller, so when it crosses the strip below depends on the geometry. */
    int ring_mid = 0;

    render(10); /* 300 ms: the tint is at its peak */
    uint32_t bg_hot = pixel_at(4, 300); /* background: below the mood panel */

    for (int i = 0; i < 40; i++) {   /* 1.2 s of samples */
        render(1);
        const int n = bright_pixels_below_face(110);
        if (n > ring_mid) ring_mid = n;
    }

    render(80); /* 2.9 s: burst long over */
    int      ring_end = bright_pixels_below_face(110);
    uint32_t bg_end   = pixel_at(4, 300);

    EXPECT(r, bg_hot != bg_rest,
           "background #%06X did not take the mood colour at the change", (unsigned)bg_hot);
    EXPECT(r, bg_end == bg_rest,
           "background still tinted #%06X long after the burst (want #%06X)",
           (unsigned)bg_end, (unsigned)bg_rest);
    EXPECT(r, ring_mid >= 20,
           "no ring reached the strip below the face (%d bright px)", ring_mid);
    EXPECT(r, ring_end == 0, "rings still drawing %d px after the burst ended", ring_end);
}

/* Sleep floats two "z"s a half cycle apart, so each of the two x bands must see
 * ink at some point in a 5 s window (the headline sits above y 26). The same
 * window also covers the slow dust motes below the face, and confirms that a
 * shut-eyed mood shows no glints at all. */

/* ------------------------------------------------------------------ */
/* Decoration checks                                                   */
/* ------------------------------------------------------------------ */



/* Blush: the cheeks get warmer than the grey IDLE face, then fade back. */
static int probe_blush(void)
{
    return count_warmer(44, 68, 111, 125, 20) + count_warmer(104, 128, 111, 125, 20);
}




/* Sparkles: cool-white motes twinkling in the band above the face. */
static int probe_motes(void)
{
    return count_cool_white(0, g_w - 1, 20, 45);   /* below the activity bar */
}

static void check_motes(result_t *r)
{
    int lo, hi;

    sample_probe(170, probe_motes, &lo, &hi);
    EXPECT(r, hi >= 5, "no sparkles appeared above the face (max %d px)", hi);
    EXPECT(r, lo == 0, "sparkles never dimmed out (min %d px)", lo);
}

/* Confetti: a DONE transition sprays pale-gold squares, then they are gone. */
static void check_party(result_t *r)
{
    int early = 0, late;

    reset_scene();
    load_scenario("party");
    ui_companion_create();

    /* The mood change (and the confetti) lands on the first 200 ms ui_tick. */
    for(int i = 0; i < 40; i++) { /* 1.2 s of samples */
        render(1);
        int n = count_confetti();
        if(n > early) early = n;
    }
    render(60); /* +1.8 s */
    late = count_confetti();

    EXPECT(r, early > 0, "no confetti on the DONE transition");
    EXPECT(r, late == 0, "%d confetti pixels still on screen after the burst", late);
}

/* ------------------------------------------------------------------ */
/* Landscape (320x172)                                                 */
/*                                                                     */
/* The IMU turns the device, app_apply_rotation() re-lays the UI out at  */
/* 320x172, and ui_companion.c's split branch puts the face on the left  */
/* and the agent list on the right. These check that, on the pixels.     */
/* ------------------------------------------------------------------ */





/* ------------------------------------------------------------------ */
/* Frame dump + driver                                                 */
/* ------------------------------------------------------------------ */

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if(f == NULL) {
        fprintf(stderr, "host: cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for(int i = 0; i < g_w * g_h; i++) {
        const unsigned char rgb[3] = {
            (unsigned char)((g_fb[i] >> 16) & 0xFF),
            (unsigned char)((g_fb[i] >> 8) & 0xFF),
            (unsigned char)(g_fb[i] & 0xFF),
        };
        fwrite(rgb, 1, sizeof rgb, f);
    }
    fclose(f);
}

typedef struct {
    const char *name;
    void (*check)(result_t *r);
} scenario_t;

static bool run_scenario(const scenario_t *sc)
{
    result_t res;
    char     path[256];

    memset(&res, 0, sizeof res);

    reset_scene();
    load_scenario(sc->name);
    ui_companion_create();
    render(20); /* 600 ms of virtual time: the UI's 200 ms timer fires 3x */

    sc->check(&res);

    /* Written relative to the repo root (run.sh chdirs there). */
    snprintf(path, sizeof path, "tools/ui_host_test/frame_%s.ppm", sc->name);
    dump_ppm(path);

    if(res.failed) {
        printf("FAIL %s %s\n", sc->name, res.detail);
        return false;
    }
    printf("PASS %s\n", sc->name);
    return true;
}

int main(void)
{
    static const scenario_t scenarios[] = {
        { "blocked",  check_blocked  },
        { "working",  check_working  },
        { "done",     check_done     },
        { "idle",     check_idle     },
        { "empty",    check_empty    },
        { "offline",  check_offline  },
        { "overflow", check_overflow },
        { "tap",      check_tap      },
        { "alive",    check_alive    },
        { "burst",    check_burst    },
        { "motes",    check_motes    },
        { "party",    check_party    },
        { "view_switch", check_view_switch },
        { "paging",      check_paging },
        { "stats",       check_stats  },
        { "live_rows",   check_live_rows },
        { "overlay",     check_overlay },
        { "flourish",    check_flourish },
    };
    const size_t n      = sizeof scenarios / sizeof scenarios[0];
    int          failed = 0;

    harness_init();

    for(size_t i = 0; i < n; i++) {
        if(!run_scenario(&scenarios[i])) failed++;
    }

    printf("%zu scenarios, %d failed\n", n, failed);
    return failed > 0 ? 1 : 0;
}