/* Blob companion for the herdr agent-status display.
 *
 * Expression, colour and motion follow the aggregate mood of the agents in the
 * last status poll; a compact per-agent list sits underneath.
 *
 * LVGL-only by design: this file is also compiled on the host by
 * tools/ui_host_test/run.sh, which is how the layout is verified without eyes
 * on the panel (AGENTS.md §9a). No esp_* includes — the only hook back into the
 * firmware is the UI_LOGI macro in herdr_status_types.h.
 */

#include "ui_companion.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "herdr_status_types.h"
#include "ui_stats.h"

/* Real value comes from main/Kconfig.projbuild (range 1..6); the host harness
 * has no sdkconfig, so fall back to the same default. */
#ifndef CONFIG_HERDR_UI_MAX_AGENTS
#define CONFIG_HERDR_UI_MAX_AGENTS 4
#endif

#define UI_ROWS CONFIG_HERDR_UI_MAX_AGENTS

/* ---- layout ------------------------------------------------------------- */
/*
 * Everything below is resolved by ui_companion_create() for the screen it is
 * handed, so the same UI serves 172x320 portrait and 320x172 landscape (the
 * device rotates itself with the IMU, see ui_rotation.c). Relative sizes (eye
 * spacing, mouth offset, decorations) are derived from the face diameter, so
 * landscape just scales the face down and moves the agent list to the right.
 *
 * The names are macros over the resolved struct: the rest of the file reads
 * like the old portrait-only version and animates correctly in both.
 */
typedef struct {
    lv_coord_t scr_w, scr_h;
    lv_coord_t body_cx, body_cy, body_d, ring_d;
    lv_coord_t eye_dx, eye_cy, eye_w, eye_h;
    lv_coord_t mouth_cy, mouth_d;
    lv_coord_t blush_dx, blush_cy;
    lv_coord_t alert_x, alert_y;
    lv_coord_t z_cx, z_base_y;
    lv_coord_t sweat_x0, sweat_y0, sweat_x1, sweat_y1;
    lv_coord_t ripple_d1;
    lv_coord_t mote_top0, mote_top1, mote_low0, mote_low1;
    lv_coord_t headline_x, headline_y;   /* portrait: centred, x ignored */
    lv_coord_t list_x, list_y0, list_row_h, list_w;
    lv_coord_t summary_x, summary_y;     /* portrait: centred, x ignored */
    bool       split;                    /* list beside the face instead of below */
} lay_t;

static lay_t L;

#define SCR_W      (L.scr_w)
#define SCR_H      (L.scr_h)
#define BODY_CX    (L.body_cx)
#define BODY_CY    (L.body_cy)
#define BODY_D     (L.body_d)
#define RING_D     (L.ring_d)
#define EYE_DX     (L.eye_dx)
#define EYE_CY     (L.eye_cy)
#define EYE_W      (L.eye_w)
#define EYE_H      (L.eye_h)
#define MOUTH_CX   (L.body_cx)
#define MOUTH_CY   (L.mouth_cy)
#define MOUTH_D    (L.mouth_d)
#define BLUSH_DX   (L.blush_dx)
#define BLUSH_CY   (L.blush_cy)
#define ALERT_X    (L.alert_x)
#define ALERT_Y    (L.alert_y)
#define Z_CX       (L.z_cx)
#define Z_BASE_Y   (L.z_base_y)
#define SWEAT_X0   (L.sweat_x0)
#define SWEAT_Y0   (L.sweat_y0)
#define SWEAT_X1   (L.sweat_x1)
#define SWEAT_Y1   (L.sweat_y1)
#define RIPPLE_D1  (L.ripple_d1)
#define PART_TOP0  (L.mote_top0)
#define PART_TOP1  (L.mote_top1)
#define PART_LOW0  (L.mote_low0)
#define PART_LOW1  (L.mote_low1)
#define HEADLINE_X (L.headline_x)
#define HEADLINE_Y (L.headline_y)
#define LIST_X     (L.list_x)
#define LIST_Y0    (L.list_y0)
#define LIST_ROW_H (L.list_row_h)
#define LIST_W     (L.list_w)
#define SUMMARY_X  (L.summary_x)
#define SUMMARY_Y  (L.summary_y)
#define SPLIT      (L.split)

/* Sizes that do not scale with the face. */
#define BODY_SQUASH_D   8   /* how much the tap squash takes off the diameter */
#define EYE_SHUT_H      3
#define MOUTH_W         5
#define MOUTH_FLAT_W    24
#define MOUTH_FLAT_H    3
#define RING_W          4
#define SWEEP_SPAN      96
#define Z_RISE          34
#define Z2_DX           14 /* the second sleeping "z", one phase behind the first */
#define DOT_D           10
#define ROW_LABEL_DX    16 /* row label offset from the row's left edge */
#define ROW_TEXT_H      12 /* montserrat_12 line height, for vertical centring */

/* Mood-change burst: two rings expanding out of the face, plus a short tint of
 * the whole background in the new mood's colour. */
#define RIPPLE_N        2
#define RIPPLE_W        3
#define RIPPLE_MS       620
#define RIPPLE_STAGGER  170
#define RIPPLE_OPA      220
#define BG_TINT_MAX     56  /* 0..255 weight of the mood colour in the background */
#define BG_TINT_MS      140
#define BG_TINT_FADE_MS 420

/* ---- decorative extras (see the mood table further down) ---------------- */

/* Eye glint: a child of each eye, so it follows a glance and is clipped away as
 * the lid closes — no animation touches it. */
#define GLINT_D         5
#define GLINT_OX        3
#define GLINT_OY        3

/* Cheek blush: two soft ellipses that fade in and out under the eyes, sized to
 * sit clear of both the eyes and the widest smile. */
#define BLUSH_W         20
#define BLUSH_H         10
#define BLUSH_OPA       110

/* Sweat drop (BLOCKED): slides down the right cheek and fades at the chin. */
#define SWEAT_W         7
#define SWEAT_H         9
#define SWEAT_OPA       200
#define SWEAT_MOVE      500 /* first half of the cycle slides; the rest is parked */

/* One particle pool serves both the ambient motes (sparkles in WORKING, dust in
 * SLEEP) and the one-shot DONE confetti. */
#define PART_N          8
#define PART_D          5
#define CONF_MS         1200
#define CONF_OPA        255 /* opaque: confetti is paper, and it keeps the probe exact */

/* Tap flourish: a short, mood-specific reaction to a single tap, on top of the
 * body squash. Ring growth and duration, the background wash strength, and how
 * many particles to throw (0 where the mood's own motes own the particle pool).
 * Kept short — this is an acknowledgement, not a scene change. */
typedef struct {
    uint32_t ring_ms;
    int32_t  ring_grow;
    uint8_t  wash;    /* 0..255 weight of the mood colour behind everything */
    uint8_t  sparks;  /* particles thrown, 0 = none */
    int32_t  rise;    /* how far those particles travel */
    uint32_t hold_ms; /* how long the wash lingers */
} flourish_cfg_t;

/* Views. Two of them, swiped between: the companion itself, and a page of
 * numbers about the link, the device and the agents' sessions. */
#define STATS_LINES 11
#define OVERLAY_LINES 12

/* Both UI timers run at this period, which is also the glance countdown unit.
 * The host harness identifies UI timers by this period when it tears a screen
 * down (tools/ui_host_test/host_main.c: drop_ui_timers), so it is load-bearing. */
#define UI_LOOK_TICK    200

/* Fill in the layout for the screen we are drawing on. Portrait stacks the list
 * under the face; landscape puts the face on the left and the list on the right
 * (172 px of height is not enough for both). */
static void ui_layout_init(lv_coord_t w, lv_coord_t h)
{
    L.scr_w = w;
    L.scr_h = h;
    L.split = (w > h);

    if (!L.split) {
        L.body_cx  = w / 2;
        L.body_cy  = 108;
        L.body_d   = 120;
        L.ring_d   = 138;
        L.eye_dx   = 26;
        L.eye_cy   = 98;
        L.eye_w    = 18;
        L.eye_h    = 18;
        L.mouth_cy = 126; /* see ui_companion.c history: the smile used to sit on the chin */
        L.mouth_d  = 56;
        L.blush_dx = 30;
        L.blush_cy = 118;
        L.alert_x  = L.body_cx + 52;
        L.alert_y  = 40;
        L.z_cx     = L.body_cx + 54;
        L.z_base_y = 60;
        L.ripple_d1 = L.body_d + 80;
        L.headline_x = 0;
        L.headline_y = 10;
        L.list_x   = 8;
        L.list_y0  = 190;
        L.list_row_h = 24;
        L.list_w   = w - 16;
        L.summary_x = 0;
        L.summary_y = 300;
        L.mote_top0 = 6;
        L.mote_top1 = 42;
        L.mote_low0 = 172;
        L.mote_low1 = 186;
    } else {
        L.body_cx  = 78;
        L.body_cy  = h / 2 + 6; /* 92 at 320x172 */
        L.body_d   = 108;
        L.ring_d   = 124;
        L.eye_dx   = 24;
        L.eye_cy   = L.body_cy - 9;
        L.eye_w    = 16;
        L.eye_h    = 16;
        L.mouth_cy = L.body_cy + 16;
        L.mouth_d  = 50;
        L.blush_dx = 27;
        L.blush_cy = L.body_cy + 9;
        L.alert_x  = L.body_cx + 47;
        L.alert_y  = 26;
        L.z_cx     = L.body_cx + 49;
        L.z_base_y = 40;
        L.ripple_d1 = L.body_d + 80;
        L.headline_x = 148;
        L.headline_y = 4;
        L.list_x   = 148;
        L.list_y0  = 24;
        L.list_row_h = 22;
        L.list_w   = w - 148 - 8;
        L.summary_x = 148;
        L.summary_y = 158;
        L.mote_top0 = 2;
        L.mote_top1 = 34;
        L.mote_low0 = 160;
        L.mote_low1 = 168;
    }

    /* The sweat drop rides the right cheek at the same proportions as portrait
     * (x +34/+14, y -38/+42 at a 120 px face). */
    const lv_coord_t u = L.body_d / 120;
    L.sweat_x0 = L.body_cx + 34 * u;
    L.sweat_y0 = L.body_cy - 38 * u;
    L.sweat_x1 = L.body_cx + 48 * u;
    L.sweat_y1 = L.body_cy + 42 * u;
}

/* ---- palette ------------------------------------------------------------ */

#define COL_BG          0x0B0F14
#define COL_TRACK       0x1A2330
#define COL_BLOCKED     0xE0A33E
#define COL_WORKING     0x4EA8FF
#define COL_DONE        0x4ADE80
#define COL_IDLE        0xC9D4E2
#define COL_SLEEP       0x3A4657
#define COL_OFFLINE     0x2A3442
#define COL_TEXT        0xC9D4E2
#define COL_DIM         0x6F7D90
#define COL_FACE        0x0B0F14
#define COL_GLINT       0xFFFFFF /* eye highlight: opaque, so pixels match exactly */
#define COL_SPARKLE     0xDCE7F5 /* ambient motes */
#define COL_CONFETTI    0xFFF3C4 /* DONE confetti, second colour is COL_SPARKLE */
#define COL_DROP        0x9FD8FF /* sweat */
#define COL_BLUSH       0xD9736F /* cheek blush */

/* ---- moods -------------------------------------------------------------- */

typedef enum {
    MOOD_BLOCKED = 0,
    MOOD_WORKING,
    MOOD_DONE,
    MOOD_IDLE,
    MOOD_SLEEP,
    MOOD_OFFLINE,
    MOOD_N,
} mood_t;

typedef struct {
    uint32_t    body;       /* body + headline colour */
    const char *headline;
    uint16_t    mouth_start, mouth_end;
    int32_t     mouth_w;
    uint32_t    blink_ms;   /* 0 == eyes held shut (sleep/offline) */
    uint32_t    bob_ms;     /* full bob cycle */
    int32_t     bob_amp;
    int32_t     breathe_amp;/* extra body width at the top of the breath; 0 == none */
    uint32_t    look_ms_min, look_ms_max; /* eye-glance interval window */
    int32_t     look_px;    /* sideways eye travel; 0 == eyes do not look around */
    bool        ring;       /* ring + sweep visible */
    uint32_t    ring_ms;
    uint32_t    ring_glow_ms; /* ring/sweep opacity breathing; 0 == none */
    uint32_t    talk_ms;    /* mouth open/close cycle; 0 == none */
    uint32_t    alert_ms;   /* 0 == alert marker hidden */
    uint32_t    z_ms;       /* 0 == "z" hidden */
    bool        flat_mouth;
    /* decorations */
    bool        glint;      /* eye highlights */
    uint32_t    blush_ms;   /* cheek blush cycle; 0 == none */
    uint32_t    sweat_ms;   /* sweat-drop cycle; 0 == none */
    uint32_t    mote_ms;    /* ambient mote cycle; 0 == none */
    lv_opa_t    mote_opa;   /* peak mote opacity */
    int32_t     mote_rise;  /* mote travel, px */
    bool        party;      /* confetti on a mood change */
} mood_cfg_t;

/* One animation per property per mood — every property below (y, width, x,
 * height, angles, arc width, opa, border opa, bg opa) is written by at most one
 * running animation, so nothing fights over a value. See ui_apply_mood(). */
static const mood_cfg_t s_moods[MOOD_N] = {
    [MOOD_BLOCKED] = {
        .body = COL_BLOCKED, .headline = "NEEDS YOU",
        .mouth_start = 45, .mouth_end = 135, .mouth_w = 6,
        .blink_ms = 1200, .bob_ms = 1000, .bob_amp = 5, .breathe_amp = 4,
        .look_ms_min = 1200, .look_ms_max = 2400, .look_px = 2,
        .alert_ms = 700,
        .glint = true, .sweat_ms = 2200,
    },
    [MOOD_WORKING] = {
        .body = COL_WORKING, .headline = "WORKING",
        .mouth_start = 30, .mouth_end = 150, .mouth_w = 5,
        .blink_ms = 2600, .bob_ms = 600, .bob_amp = 4, .breathe_amp = 3,
        .look_ms_min = 1600, .look_ms_max = 3200, .look_px = 4,
        .ring = true, .ring_ms = 1400, .ring_glow_ms = 700,
        .talk_ms = 450,
        .glint = true, .mote_ms = 800, .mote_opa = 255, .mote_rise = 16,
    },
    [MOOD_DONE] = {
        .body = COL_DONE, .headline = "DONE",
        .mouth_start = 10, .mouth_end = 170, .mouth_w = 5,
        .blink_ms = 1600, .bob_ms = 700, .bob_amp = 6, .breathe_amp = 6,
        .look_ms_min = 1800, .look_ms_max = 3600, .look_px = 4,
        .glint = true, .blush_ms = 1500, .party = true,
    },
    [MOOD_IDLE] = {
        .body = COL_IDLE, .headline = "IDLE",
        .mouth_start = 20, .mouth_end = 160, .mouth_w = 5,
        .blink_ms = 3400, .bob_ms = 1800, .bob_amp = 3, .breathe_amp = 3,
        .look_ms_min = 2600, .look_ms_max = 5200, .look_px = 5,
        .glint = true, .blush_ms = 2200,
    },
    [MOOD_SLEEP] = {
        .body = COL_SLEEP, .headline = "NO AGENTS",
        .blink_ms = 0, .bob_ms = 2600, .bob_amp = 3, .breathe_amp = 3,
        .z_ms = 2600, .flat_mouth = true,
        .mote_ms = 3200, .mote_opa = 110, .mote_rise = 26,
    },
    [MOOD_OFFLINE] = {
        .body = COL_OFFLINE, .headline = "OFFLINE",
        .blink_ms = 0, .bob_ms = 3200, .bob_amp = 2, .breathe_amp = 2,
        .flat_mouth = true,
    },
};

static const char *s_mood_names[MOOD_N] = {
    "blocked", "working", "done", "idle", "sleep", "offline",
};

/* ---- state -------------------------------------------------------------- */

static const char *TAG = "ui";

static lv_obj_t *s_scr; /* the active screen: background tint target */
static lv_obj_t *s_headline;
static lv_obj_t *s_ring;
static lv_obj_t *s_sweep;
static lv_obj_t *s_body;
static lv_obj_t *s_eye[2];
static lv_obj_t *s_glint[2];
static lv_obj_t *s_blush[2];
static lv_obj_t *s_sweat;
static lv_obj_t *s_part[PART_N];
static lv_obj_t *s_mouth;
static lv_obj_t *s_mouth_flat;
static lv_obj_t *s_ripple[RIPPLE_N];
/* Each ring's own diameter range. The fade is driven by the animation's progress
 * rather than by the current diameter: a size-tied fade (v - BODY_D over the
 * burst's RIPPLE_D1) stops part-way when a shorter animation — a tap flourish —
 * ends early, which left a faint ring parked around the face. */
static struct {
    int32_t d0, d1;
} s_ripple_geom[RIPPLE_N];
static lv_obj_t *s_alert;
static lv_obj_t *s_z;
static lv_obj_t *s_z2;
static lv_obj_t *s_summary;
static lv_obj_t *s_row[UI_ROWS];
static lv_obj_t *s_dot[UI_ROWS];
static lv_obj_t *s_row_label[UI_ROWS];
static lv_obj_t *s_row_status[UI_ROWS];

/* Views and interaction state. */
static ui_view_t s_view;
static int       s_page;          /* which slice of the agent list is shown */
static lv_obj_t *s_mood_cont;     /* holds the whole companion view */
static lv_obj_t *s_stats_cont;    /* holds the stats view */
static lv_obj_t *s_overlay;       /* diagnostics panel, created on demand */
static lv_obj_t *s_overlay_text;
static lv_obj_t *s_stats_title;
static lv_obj_t *s_stats_txt[STATS_LINES];
static uint32_t  s_taps;      /* interaction counters, shown by the overlay */
static uint32_t  s_longs;
static uint32_t  s_doubles;
static uint32_t  s_flourish_count;
static int       s_stats_page;    /* the stats view has two pages of its own */
static int       s_stats_pages;

/* Per-mood tap reactions. BLOCKED and OFFLINE get sparks because the particle
 * pool is idle in those moods; WORKING and SLEEP leave it to their motes. */
static const flourish_cfg_t s_flourish[MOOD_N] = {
    [MOOD_BLOCKED] = { .ring_ms = 260, .ring_grow = 40, .wash = 40, .sparks = 3, .rise = 18, .hold_ms = 200 },
    [MOOD_WORKING] = { .ring_ms = 220, .ring_grow = 46, .wash = 30, .sparks = 0, .rise = 0,  .hold_ms = 160 },
    [MOOD_DONE]    = { .ring_ms = 240, .ring_grow = 52, .wash = 44, .sparks = 8, .rise = -1, .hold_ms = 220 },
    [MOOD_IDLE]    = { .ring_ms = 340, .ring_grow = 34, .wash = 18, .sparks = 0, .rise = 0,  .hold_ms = 260 },
    [MOOD_SLEEP]   = { .ring_ms = 380, .ring_grow = 30, .wash = 0,  .sparks = 0, .rise = 0,  .hold_ms = 0 },
    [MOOD_OFFLINE] = { .ring_ms = 200, .ring_grow = 44, .wash = 0,  .sparks = 4, .rise = 14, .hold_ms = 0 },
};

static mood_t   s_mood;
static bool     s_bob_running;
static bool     s_squash_running;
static uint32_t s_last_gen;
static bool     s_last_online;
static mood_t   s_last_mood;
static uint32_t s_burst_colour;  /* mood colour of the ripple/tint burst in flight */
static int32_t  s_eye_dx;        /* current sideways glance offset, px */
static uint32_t s_look_wait;     /* glance countdown, in UI_LOOK_TICK units */

/* Per-particle parameters, filled in when a mote/confetti run starts. Motes use
 * x0/y0 as the resting spot and y1 as the travel; confetti uses x0,y0 -> x1,y1. */
typedef struct {
    int32_t  x0, y0, x1, y1;
    uint32_t ms;
    lv_opa_t opa;
} part_cfg_t;

static part_cfg_t s_part_cfg[PART_N];

/* ---- animation callbacks ------------------------------------------------ */

static void anim_body_bob(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, BODY_CY - BODY_D / 2 - v);
}

/* Tap feedback: squash towards the centre, then LVGL plays it back. */
static void anim_body_squash(void *var, int32_t v)
{
    lv_obj_t *body = var;
    lv_obj_set_height(body, (lv_coord_t)v);
    lv_obj_set_y(body, BODY_CY - (lv_coord_t)(v / 2));
}

static void anim_eye_blink(void *var, int32_t v)
{
    lv_obj_t *eye = var;
    lv_obj_set_height(eye, (lv_coord_t)v);
    lv_obj_set_y(eye, EYE_CY - (lv_coord_t)(v / 2));
}

/* Breathing: the face gets a little wider and back, always about its centre.
 * Width and x only — y belongs to the bob, height to the tap squash. */
static void anim_body_breathe(void *var, int32_t v)
{
    lv_obj_t *body = var;
    lv_coord_t w = BODY_D + (lv_coord_t)v;

    lv_obj_set_width(body, w);
    lv_obj_set_x(body, BODY_CX - w / 2);
}

/* Both eyes glance sideways together; their spacing never changes. */
static void anim_eye_look(void *var, int32_t v)
{
    LV_UNUSED(var);
    s_eye_dx = v;
    lv_obj_set_x(s_eye[0], BODY_CX - EYE_DX - EYE_W / 2 + (lv_coord_t)v);
    lv_obj_set_x(s_eye[1], BODY_CX + EYE_DX - EYE_W / 2 + (lv_coord_t)v);
}

/* Ring + sweep glow breathing. The sweep's rotation is a different animation on
 * a different property, so the two coexist. */
static void anim_ring_glow(void *var, int32_t v)
{
    LV_UNUSED(var);
    lv_obj_set_style_border_opa(s_ring, (lv_opa_t)v, 0);
    lv_obj_set_style_arc_opa(s_sweep, (lv_opa_t)v, LV_PART_INDICATOR);
}

/* Working face mutters to itself: the smile thickens and thins. Animating the
 * arc's angles instead moved each corner by ~2 px — invisible. */
static void anim_mouth_talk(void *var, int32_t v)
{
    LV_UNUSED(var);
    lv_obj_set_style_arc_width(s_mouth, MOUTH_W + v, LV_PART_INDICATOR);
}

static void anim_sweep_rotation(void *var, int32_t v)
{
    lv_arc_set_rotation((lv_obj_t *)var, (uint16_t)v);
}

static void anim_alert_blink(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* Single callback: the "z" drifts up and fades out over one span. */
static void anim_z_float(void *var, int32_t v)
{
    lv_obj_t *z = var;
    lv_obj_set_y(z, Z_BASE_Y - v);
    lv_obj_set_style_opa(z, (lv_opa_t)(255 - v * 255 / Z_RISE), 0);
}

/* Mood-change burst ring: expands from the face's own size out past the panel
 * edge while fading out. border_opa (not opa) so the fade is real: style `opa`
 * on a plain object is an all-or-nothing cutoff in LVGL 8 (lv_obj_draw.c). */
static int ripple_index_of(const lv_obj_t *ring)
{
    for (int i = 0; i < RIPPLE_N; i++) {
        if (s_ripple[i] == ring) {
            return i;
        }
    }
    return 0;
}

/* v is progress 0..1000, not a diameter, so the ring always fades to nothing
 * however far its own animation was asked to grow. */
static void anim_ripple(void *var, int32_t v)
{
    lv_obj_t          *ring = var;
    const int32_t      d = s_ripple_geom[ripple_index_of(ring)].d0 +
                           (s_ripple_geom[ripple_index_of(ring)].d1 -
                            s_ripple_geom[ripple_index_of(ring)].d0) * v / 1000;
    const int32_t      opa = RIPPLE_OPA - RIPPLE_OPA * v / 1000;

    lv_obj_set_size(ring, (lv_coord_t)d, (lv_coord_t)d);
    lv_obj_set_pos(ring, BODY_CX - d / 2, BODY_CY - d / 2);
    lv_obj_set_style_border_opa(ring, (lv_opa_t)opa, 0);
}

/* Park the ring out of the way once its animation is over: an invisible ring is
 * still an invalidated area on every refresh. */
static void ripple_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

/* Background wash in the new mood's colour: up fast, back down slowly. */
static void anim_bg_tint(void *var, int32_t v)
{
    lv_obj_set_style_bg_color((lv_obj_t *)var,
                              lv_color_mix(lv_color_hex(s_burst_colour), lv_color_hex(COL_BG), (uint8_t)v), 0);
}

/* Filled decorations fade through bg_opa: the object-level `opa` style is only
 * an all-or-nothing cutoff in LVGL 8 (lv_obj_draw.c:41-48), so it cannot fade
 * a fill. */
static void anim_blush(void *var, int32_t v)
{
    LV_UNUSED(var);
    lv_obj_set_style_bg_opa(s_blush[0], (lv_opa_t)v, 0);
    lv_obj_set_style_bg_opa(s_blush[1], (lv_opa_t)v, 0);
}

/* Sweat: the first half of the cycle slides and fades; the rest is parked
 * invisible, which is how a one-value animation gets a pause. */
static void anim_sweat(void *var, int32_t v)
{
    if (v >= SWEAT_MOVE) {
        lv_obj_set_style_bg_opa((lv_obj_t *)var, LV_OPA_TRANSP, 0);
        return;
    }
    const int32_t t = v * 1000 / SWEAT_MOVE; /* 0..1000 across the slide */
    int32_t       opa;

    if (t < 150) {
        opa = SWEAT_OPA * t / 150;
    } else if (t > 750) {
        opa = SWEAT_OPA * (1000 - t) / 250;
    } else {
        opa = SWEAT_OPA;
    }
    lv_obj_set_pos((lv_obj_t *)var, SWEAT_X0 + (SWEAT_X1 - SWEAT_X0) * t / 1000,
                   SWEAT_Y0 + (SWEAT_Y1 - SWEAT_Y0) * t / 1000);
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)opa, 0);
}

static int part_index_of(const lv_obj_t *p)
{
    for (int i = 0; i < PART_N; i++) {
        if (s_part[i] == p) return i;
    }
    return 0;
}

/* Motes twinkle for the first third of their own cycle and then park. The fade
 * is a trapezoid, not a triangle: with only ~26 value steps per cycle a
 * triangular ramp never gets near full opacity (measured: it peaked at 44%),
 * and the flat top is what makes a sparkle read as a glint rather than a
 * smudge. Each particle gets its own cycle length, so they drift out of phase
 * instead of pulsing in lockstep. */
static void anim_mote(void *var, int32_t v)
{
    lv_obj_t         *p = var;
    const part_cfg_t *c = &s_part_cfg[part_index_of(p)];
    int32_t           fade;

    if (v >= 350) {
        lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
        lv_obj_set_y(p, c->y0);
        return;
    }
    const int32_t t = v * 1000 / 350; /* 0..1000 across the twinkle */
    if (t < 200) {
        fade = t * 1000 / 200;
    } else if (t < 600) {
        fade = 1000;
    } else {
        fade = (1000 - t) * 1000 / 400;
    }

    lv_obj_set_y(p, c->y0 - c->y1 * t / 1000);
    lv_obj_set_style_bg_opa(p, (lv_opa_t)(c->opa * fade / 1000), 0);
}

/* Confetti: outward at a constant speed, falling under "gravity" (quadratic),
 * opaque for the whole flight and fading only at the end (a value-stepped ramp
 * peaked at 96% instead of 100%, which also made it hard to observe). */
static void anim_confetti(void *var, int32_t v)
{
    lv_obj_t         *p = var;
    const part_cfg_t *c = &s_part_cfg[part_index_of(p)];
    int32_t           opa;

    if (v < 100) {
        opa = c->opa * v / 100;
    } else if (v < 800) {
        opa = c->opa;
    } else {
        opa = c->opa * (1000 - v) / 200;
    }

    lv_obj_set_pos(p, c->x0 + (c->x1 - c->x0) * v / 1000,
                   c->y0 + (c->y1 - c->y0) * v * v / 1000000);
    lv_obj_set_style_bg_opa(p, (lv_opa_t)opa, 0);
}

/* ---- construction helpers ---------------------------------------------- */

/* Decorative widgets must not swallow the screen's clicks or scroll. */
static void make_passive(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_ADV_HITTEST);
}

/* Plain object with every default-theme decoration (card bg, border, pad)
 * switched off, sized and centred on the given centre point. */
static lv_obj_t *create_blob(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t cx, lv_coord_t cy)
{
    lv_obj_t *obj = lv_obj_create(parent);
    make_passive(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

/* An arc that draws no track of its own -- a separate object is the track. */
static lv_obj_t *create_arc(lv_obj_t *parent, lv_coord_t d, lv_coord_t cx, lv_coord_t cy, int32_t width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    make_passive(arc);
    lv_obj_set_size(arc, d, d);
    lv_obj_set_pos(arc, cx - d / 2, cy - d / 2);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(arc, 0, 0);
    return arc;
}

static void start_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                       uint32_t time, uint32_t playback, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time);
    lv_anim_set_playback_time(&a, playback);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

/* Time and playback time are halves of the cycle for every looping animation,
 * so the table's period is the wall-clock period. */
static void ui_start_bob(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];
    lv_anim_del(s_body, anim_body_bob);
    start_anim(s_body, anim_body_bob, 0, m->bob_amp,
               m->bob_ms / 2, m->bob_ms - m->bob_ms / 2, lv_anim_path_ease_in_out);
    s_bob_running = true;
}

static void ui_start_blink(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];
    for (int i = 0; i < 2; i++) {
        lv_anim_del(s_eye[i], anim_eye_blink);
        if (m->blink_ms == 0) {
            lv_obj_set_height(s_eye[i], EYE_SHUT_H);
            lv_obj_set_y(s_eye[i], EYE_CY - EYE_SHUT_H / 2);
        } else {
            lv_obj_set_height(s_eye[i], EYE_H);
            lv_obj_set_y(s_eye[i], EYE_CY - EYE_H / 2);
            start_anim(s_eye[i], anim_eye_blink, EYE_H, 2,
                       m->blink_ms / 2, m->blink_ms - m->blink_ms / 2, lv_anim_path_linear);
        }
    }
}

static void squash_ready_cb(lv_anim_t *a)
{
    LV_UNUSED(a);
    s_squash_running = false;
}

static void ui_start_breathe(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_body, anim_body_breathe);
    if (m->breathe_amp == 0) {
        anim_body_breathe(s_body, 0);
        return;
    }

    /* A quarter cycle behind the bob: in phase, the widening was cancelled by
     * the vertical offset and the breath was invisible. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_body);
    lv_anim_set_exec_cb(&a, anim_body_breathe);
    lv_anim_set_values(&a, 0, m->breathe_amp);
    lv_anim_set_delay(&a, m->bob_ms / 4);
    lv_anim_set_time(&a, m->bob_ms / 2);
    lv_anim_set_playback_time(&a, m->bob_ms - m->bob_ms / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* The ring only exists in the WORKING mood, so the glow follows it. */
static void ui_start_glow(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_ring, anim_ring_glow);
    if (m->ring_glow_ms == 0) {
        lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
        lv_obj_set_style_arc_opa(s_sweep, LV_OPA_COVER, LV_PART_INDICATOR);
        return;
    }
    start_anim(s_ring, anim_ring_glow, LV_OPA_COVER, 120,
               m->ring_glow_ms / 2, m->ring_glow_ms - m->ring_glow_ms / 2, lv_anim_path_ease_in_out);
}

static void ui_start_talk(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_mouth, anim_mouth_talk);
    if (m->talk_ms == 0) {
        return;
    }
    start_anim(s_mouth, anim_mouth_talk, 0, 4,
               m->talk_ms / 2, m->talk_ms - m->talk_ms / 2, lv_anim_path_ease_in_out);
}

/* Two "z"s, the second half a cycle behind the first. */
static void ui_start_z(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_z, anim_z_float);
    lv_anim_del(s_z2, anim_z_float);
    if (m->z_ms == 0) {
        return;
    }

    start_anim(s_z, anim_z_float, 0, Z_RISE, m->z_ms, 0, lv_anim_path_linear);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_z2);
    lv_anim_set_exec_cb(&a, anim_z_float);
    lv_anim_set_values(&a, 0, Z_RISE);
    lv_anim_set_delay(&a, m->z_ms / 2);
    lv_anim_set_time(&a, m->z_ms);
    lv_anim_set_playback_time(&a, 0);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/* Cheek blush fades in and out on its own slow cycle. */
static void ui_start_blush(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_blush[0], anim_blush);
    if (m->blush_ms == 0) {
        anim_blush(NULL, LV_OPA_TRANSP);
        return;
    }
    start_anim(s_blush[0], anim_blush, LV_OPA_TRANSP, BLUSH_OPA,
               m->blush_ms / 2, m->blush_ms - m->blush_ms / 2, lv_anim_path_ease_in_out);
}

static void ui_start_sweat(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    lv_anim_del(s_sweat, anim_sweat);
    if (m->sweat_ms == 0) {
        anim_sweat(s_sweat, SWEAT_MOVE); /* parks it invisibly */
        return;
    }
    start_anim(s_sweat, anim_sweat, 0, 1000, m->sweat_ms, 0, lv_anim_path_linear);
}

/* Hide every particle and stop whatever was driving it. */
static void ui_stop_particles(void)
{
    for (int i = 0; i < PART_N; i++) {
        lv_anim_del(s_part[i], anim_mote);
        lv_anim_del(s_part[i], anim_confetti);
        lv_obj_set_style_bg_opa(s_part[i], LV_OPA_TRANSP, 0);
    }
}

/* Ambient motes: sparkles in WORKING, slow dust in SLEEP. */
static void ui_start_motes(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];

    if (m->mote_ms == 0) {
        return;
    }
    for (int i = 0; i < PART_N; i++) {
        part_cfg_t *c = &s_part_cfg[i];
        const bool  lower = (i % 3) == 2; /* a third of them below the face */
        const int32_t band_h = lower ? (PART_LOW1 - PART_LOW0) : (PART_TOP1 - PART_TOP0);
        /* Never ask for more rise than the band is tall: lv_rand(min, max) with
         * max < min wraps and spawns the mote off-screen. */
        const int32_t rise = (m->mote_rise < band_h) ? m->mote_rise : band_h;

        c->x0  = (int32_t)lv_rand(4, SCR_W - 12);
        c->y0  = lower ? (int32_t)lv_rand(PART_LOW0 + rise, PART_LOW1)
                       : (int32_t)lv_rand(PART_TOP0 + rise, PART_TOP1);
        c->x1  = 0;
        c->y1  = rise;
        c->ms  = m->mote_ms * (70 + lv_rand(0, 60)) / 100; /* +-30%: never in lockstep */
        c->opa = m->mote_opa;

        lv_obj_set_pos(s_part[i], c->x0, c->y0);
        lv_obj_set_style_bg_color(s_part[i], lv_color_hex(COL_SPARKLE), 0);
        lv_obj_set_style_radius(s_part[i], LV_RADIUS_CIRCLE, 0);
        anim_mote(s_part[i], 0);
        start_anim(s_part[i], anim_mote, 0, 1000, c->ms, 0, lv_anim_path_linear);
    }
}

/* One-shot burst of confetti out of the face, staggered so it sprays rather
 * than appearing all at once. */
static void ui_start_confetti(void)
{
    for (int i = 0; i < PART_N; i++) {
        part_cfg_t *c = &s_part_cfg[i];

        /* Launch from 40% of the way to the landing point, i.e. just inside the
         * blob's rim: started at the centre, the whole flight happened behind
         * the face and only the last frames were visible. */
        c->x1  = (int32_t)lv_rand(4, SCR_W - 4);
        c->y1  = (int32_t)lv_rand(SCR_H / 2, SCR_H - 20);
        c->x0  = BODY_CX + (c->x1 - BODY_CX) * 40 / 100;
        c->y0  = BODY_CY + (c->y1 - BODY_CY) * 40 / 100;
        c->ms  = CONF_MS;
        c->opa = CONF_OPA;

        lv_obj_set_style_bg_color(s_part[i], lv_color_hex((i & 1) ? COL_SPARKLE : COL_CONFETTI), 0);
        lv_obj_set_style_radius(s_part[i], 1, 0); /* little squares, not dots */
        anim_confetti(s_part[i], 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_part[i]);
        lv_anim_set_exec_cb(&a, anim_confetti);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_delay(&a, (uint32_t)i * 60);
        lv_anim_set_time(&a, CONF_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    }
}

/* Occasional glance: the eyes hold a position for a while, then move together.
 * The timer runs at a FIXED period — the random interval is a countdown in ticks
 * — so a screen rebuild can identify and drop it by period alone (the host
 * harness drops period-200 timers; a re-rolled period would slip through and
 * touch freed widgets). */
static void ui_look_fire(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    const mood_cfg_t *m = &s_moods[s_mood];

    if (s_look_wait > 0) {
        s_look_wait--;
        return;
    }
    if (m->look_px <= 0) {
        return; /* shut eyes (sleep/offline) never glance */
    }
    s_look_wait = (m->look_ms_min + lv_rand(0, m->look_ms_max - m->look_ms_min)) / UI_LOOK_TICK;

    int32_t target;
    switch (lv_rand(0, 2)) {
    case 0:  target = -m->look_px; break;
    case 1:  target = 0;           break;
    default: target = m->look_px;  break;
    }
    if (target == s_eye_dx) {
        /* Always move: rest if we were off-centre, glance otherwise. */
        target = (s_eye_dx == 0) ? -m->look_px : 0;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_eye[0]);
    lv_anim_set_exec_cb(&a, anim_eye_look);
    lv_anim_set_values(&a, s_eye_dx, target);
    lv_anim_set_time(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Mood change: two rings wash out of the face and the background takes the new
 * colour for a moment. Restarting is safe — the callbacks rewrite both values
 * from their first frame. */
static void ui_start_burst(void)
{
    const mood_cfg_t *m = &s_moods[s_mood];
    s_burst_colour = m->body;

    for (int i = 0; i < RIPPLE_N; i++) {
        s_ripple_geom[i].d0 = BODY_D;
        s_ripple_geom[i].d1 = RIPPLE_D1;

        lv_anim_del(s_ripple[i], anim_ripple);
        lv_obj_set_style_border_color(s_ripple[i], lv_color_hex(m->body), 0);
        lv_obj_clear_flag(s_ripple[i], LV_OBJ_FLAG_HIDDEN);
        anim_ripple(s_ripple[i], 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ripple[i]);
        lv_anim_set_exec_cb(&a, anim_ripple);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_delay(&a, (uint32_t)i * RIPPLE_STAGGER);
        lv_anim_set_time(&a, RIPPLE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&a, ripple_ready_cb);
        lv_anim_start(&a);
    }

    lv_anim_del(s_scr, anim_bg_tint);
    anim_bg_tint(s_scr, 0);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_scr);
    lv_anim_set_exec_cb(&b, anim_bg_tint);
    lv_anim_set_values(&b, 0, BG_TINT_MAX);
    lv_anim_set_time(&b, BG_TINT_MS);
    lv_anim_set_playback_time(&b, BG_TINT_FADE_MS);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);

    /* DONE gets confetti on top of the rings. */
    if (m->party) {
        ui_start_confetti();
    }
}

/* ---- views ------------------------------------------------------------- */

static void ui_stats_render(void);
static void ui_overlay_render(void);
static void ui_render_list(const herdr_status_t *s);
static void ui_render_summary(const herdr_status_t *s);

const char *ui_view_name(ui_view_t view)
{
    return (view == UI_VIEW_STATS) ? "stats" : "mood";
}

ui_view_t ui_companion_view(void)
{
    return s_view;
}

/* One flag decides which containers are on screen. The stats view is refreshed on
 * the way in so it never shows numbers from the last time it was looked at. */
static void ui_apply_view(void)
{
    if (s_view == UI_VIEW_STATS) {
        lv_obj_add_flag(s_mood_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_stats_cont, LV_OBJ_FLAG_HIDDEN);
        ui_stats_render();
    } else {
        lv_obj_add_flag(s_stats_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_mood_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_companion_on_switch_view(int dir)
{
    if (dir == 0) {
        return;
    }
    int v = (int)s_view + ((dir > 0) ? 1 : -1);
    if (v < 0) {
        v = UI_VIEW_COUNT - 1;
    } else if (v >= UI_VIEW_COUNT) {
        v = 0;
    }
    s_view = (ui_view_t)v;
    ui_apply_view();
    UI_LOGI(TAG, "view=%s", ui_view_name(s_view));
}

/* Vertical swipe: pages whichever view is showing. The mood view pages the agent
 * list (only useful when the bridge reports more agents than there are rows) and
 * the stats view pages its two screens of numbers. */
void ui_companion_on_page(int dir)
{
    if (dir == 0) {
        return;
    }

    herdr_status_t s;
    if (!herdr_client_get(&s)) {
        return;
    }

    if (s_view == UI_VIEW_STATS) {
        if (s_stats_pages <= 1) {
            return;
        }
        s_stats_page += (dir > 0) ? 1 : -1;
        if (s_stats_page < 0) {
            s_stats_page = s_stats_pages - 1;
        } else if (s_stats_page >= s_stats_pages) {
            s_stats_page = 0;
        }
        ui_stats_render();
        return;
    }

    const int pages = (s.count + UI_ROWS - 1) / UI_ROWS;
    if (pages <= 1) {
        return; /* everything already fits */
    }
    s_page += (dir > 0) ? 1 : -1;
    if (s_page < 0) {
        s_page = pages - 1;
    } else if (s_page >= pages) {
        s_page = 0;
    }
    ui_render_list(&s);
    ui_render_summary(&s);
    UI_LOGI(TAG, "list page %d/%d", s_page + 1, pages);
}

/* ---- diagnostics overlay ------------------------------------------------- */

void ui_companion_on_toggle_overlay(void)
{
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay); /* takes the label with it */
        s_overlay      = NULL;
        s_overlay_text = NULL;
        UI_LOGI(TAG, "overlay off");
        return;
    }

    s_overlay = lv_obj_create(s_scr);
    make_passive(s_overlay);
    lv_obj_set_size(s_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, 240, 0);
    lv_obj_set_style_border_width(s_overlay, 1, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_border_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 5, 0);

    s_overlay_text = lv_label_create(s_overlay);
    make_passive(s_overlay_text);
    lv_obj_set_style_text_font(s_overlay_text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_overlay_text, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(s_overlay_text, 0, 0);
    lv_label_set_long_mode(s_overlay_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_overlay_text, SCR_W - 12);

    ui_overlay_render();
    UI_LOGI(TAG, "overlay on");
}

/* Everything an overlay line can say that the firmware can answer about itself.
 * The whole point is that this is readable when something is wrong, so it is
 * deliberately dense and uses short labels. */
static void ui_overlay_render(void)
{
    if (s_overlay_text == NULL) {
        return;
    }

    herdr_link_stats_t link = { 0 };
    herdr_imu_stats_t  imu  = { 0 };
    herdr_status_t     s    = { 0 };

    herdr_client_stats(&link);
    ui_rotation_stats_get(&imu);
    const bool have = herdr_client_get(&s);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    const uint32_t up = lv_tick_get() / 1000;
    char           buf[512];

    snprintf(buf, sizeof buf,
             "up      %02u:%02u:%02u\n"
             "heap    %u K low %u K\n"
             "lvgl    %u%% frag %u%%\n"
             "bridge  gen %u  %s\n"
             "poll    %u ok %u err\n"
             "rtt     %u ms  fail %u\n"
             "agents  %d  ovf %d\n"
             "imu     %u Hz  err %u\n"
             "axis    %d %s%c  %s\n"
             "rot     %d  %dx%d\n"
             "input   tap %u  long %u\n"
             "        dbl %u  view %s",
             (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60),
             (unsigned)(ui_device_free_heap() / 1024), (unsigned)(ui_device_min_free_heap() / 1024),
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct,
             (unsigned)link.gen, link.online ? "online" : "offline",
             (unsigned)link.polls, (unsigned)link.fail_total,
             (unsigned)link.rtt_ms, (unsigned)link.failures,
             have ? s.count : 0, have ? s.overflow : 0,
             (unsigned)imu.rate_hz, (unsigned)imu.errors,
             (int)imu.axis, imu.calibrated ? "cal" : "raw",
             (imu.sign > 0) ? '+' : '-', imu.present ? "ok" : "gone",
             SPLIT ? 1 : 0, SCR_W, SCR_H,
             (unsigned)s_taps, (unsigned)s_longs,
             (unsigned)s_doubles, ui_view_name(s_view));

    lv_label_set_text(s_overlay_text, buf);
}

/* ---- stats view ---------------------------------------------------------- */

/* Formats a token count so it fits a 12 px font on a 172 px wide screen. */
static void fmt_tokens(char *buf, size_t n, uint32_t tokens)
{
    /* uint32_t is `unsigned long` on the C6 but `unsigned int` on the host, so
     * every argument is cast: -Werror=format catches the difference on one of the
     * two builds otherwise. */
    if (tokens >= 1000000000u) {
        /* Live sessions reach into the billions: a long agent session re-sends its
         * whole context every call, so a single session's input count passes 1e9
         * on its own (measured: 322M, 522M, 239M for three sessions on the desk). */
        snprintf(buf, n, "%u.%uG", (unsigned)(tokens / 1000000000u),
                 (unsigned)((tokens / 100000000u) % 10u));
    } else if (tokens >= 1000000u) {
        snprintf(buf, n, "%u.%uM", (unsigned)(tokens / 1000000u), (unsigned)((tokens / 100000u) % 10u));
    } else if (tokens >= 1000u) {
        snprintf(buf, n, "%u.%uk", (unsigned)(tokens / 1000u), (unsigned)((tokens / 100u) % 10u));
    } else {
        snprintf(buf, n, "%u", (unsigned)tokens);
    }
}

/* Ages as a human reads them: seconds under a minute, then minutes, hours and
 * days. "last 9602s" is what the raw field looks like (a session left overnight),
 * and it says nothing to anyone. */
static void fmt_age(char *buf, size_t n, uint32_t secs)
{
    if (secs < 60u) {
        snprintf(buf, n, "%us", (unsigned)secs);
    } else if (secs < 3600u) {
        snprintf(buf, n, "%um", (unsigned)(secs / 60u));
    } else if (secs < 86400u) {
        snprintf(buf, n, "%uh%02um", (unsigned)(secs / 3600u), (unsigned)((secs / 60u) % 60u));
    } else {
        snprintf(buf, n, "%ud%uh", (unsigned)(secs / 86400u), (unsigned)((secs / 3600u) % 24u));
    }
}

/* One line of the stats view. The view is 11 lines on both orientations, and
 * vertical swipes page between the link/device page and the sessions page. */
static void stats_line(int idx, const char *fmt, ...)
{
    if (idx < 0 || idx >= STATS_LINES || s_stats_txt[idx] == NULL) {
        return;
    }
    va_list ap;
    char    buf[48];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lv_label_set_text(s_stats_txt[idx], buf);
}

static void ui_stats_render(void)
{
    herdr_link_stats_t link = { 0 };
    herdr_imu_stats_t  imu  = { 0 };

    herdr_client_stats(&link);
    ui_rotation_stats_get(&imu);

    herdr_status_t   s = { 0 };
    const bool       have = herdr_client_get(&s);

    if (s_stats_page == 0) {
        /* Link, device and the things that go wrong. */
        stats_line(0, "LINK");
        stats_line(1, " gen %u  %s", (unsigned)link.gen, link.online ? "online" : "offline");
        stats_line(2, " poll %u ok  %u err", (unsigned)link.polls, (unsigned)link.fail_total);
        stats_line(3, " rtt %u ms  fail %u", (unsigned)link.rtt_ms, (unsigned)link.failures);
        stats_line(4, "DEVICE");
        stats_line(5, " up %uh%02um  heap %uK", (unsigned)(lv_tick_get() / 3600000u),
                   (unsigned)((lv_tick_get() / 60000u) % 60u), (unsigned)(ui_device_free_heap() / 1024));
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        stats_line(6, " lvgl %u%% frag %u%%", (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
        stats_line(7, " imu %u Hz  %s", (unsigned)imu.rate_hz, imu.calibrated ? "calibrated" : "raw");
        stats_line(8, " ax %d %c  err %u", (int)imu.axis, (imu.sign > 0) ? '+' : '-', (unsigned)imu.errors);
        stats_line(9, " rot %s  %dx%d", SPLIT ? "land" : "port", SCR_W, SCR_H);
        stats_line(10, " in %u tap  %u dbl", (unsigned)s_taps, (unsigned)s_doubles);
        s_stats_pages = 2;
        return;
    }

    /* Sessions page: what the agents have actually been doing, from the bridge's
     * read of their session logs. */
    herdr_sessions_t sess = { 0 };
    const bool       have_sessions = herdr_stats_get(&sess) && sess.valid;

    if (!have_sessions) {
        stats_line(0, "SESSIONS");
        stats_line(1, " no data yet — the bridge");
        stats_line(2, " has not answered /stats");
        for (int i = 3; i < STATS_LINES; i++) {
            stats_line(i, "");
        }
        return;
    }

    char tin[16], tout[16];
    fmt_tokens(tin, sizeof tin, sess.tokens_in);
    fmt_tokens(tout, sizeof tout, sess.tokens_out);
    char age[12];
    fmt_age(age, sizeof age, sess.age_s);

    stats_line(0, "SESS %u   %s in", (unsigned)sess.sessions, tin);
    stats_line(1, "out %s  msg %u", tout, (unsigned)sess.messages);
    stats_line(2, "calls %u  last %s", (unsigned)sess.tool_calls, age);

    const int shown = (have && s.count < HERDR_MAX_AGENTS) ? s.count : HERDR_MAX_AGENTS;
    const int rows  = (shown < STATS_LINES - 3) ? shown : STATS_LINES - 3;

    for (int i = 0; i < rows; i++) {
        fmt_tokens(tin, sizeof tin, sess.per[i].tokens_in);
        stats_line(3 + i, "%-9.9s %s %uc", have ? s.agents[i].label : "?", tin, sess.per[i].tool_calls);
    }

    /* The link/device page fills all eleven lines and this one fills seven, so
     * without this the device page's tail (the IMU line, the rotation line, the
     * input counters) stays on screen under the session rows. */
    for (int i = 3 + rows; i < STATS_LINES; i++) {
        stats_line(i, "");
    }
}

/* A tap's reaction: a short ring, an optional wash of background colour, and —
 * in the moods whose particle pool is idle — a fistful of sparks. Everything
 * reuses what the mood-change burst already owns, so a tap during a burst
 * re-aims it instead of doubling it up. */
static void ui_flourish_start(void)
{
    const flourish_cfg_t *f = &s_flourish[s_mood];

    s_flourish_count++;
    s_burst_colour = s_moods[s_mood].body;

    s_ripple_geom[0].d0 = BODY_D;
    s_ripple_geom[0].d1 = BODY_D + f->ring_grow;

    lv_anim_del(s_ripple[0], anim_ripple);
    lv_obj_set_style_border_color(s_ripple[0], lv_color_hex(s_burst_colour), 0);
    lv_obj_clear_flag(s_ripple[0], LV_OBJ_FLAG_HIDDEN);
    anim_ripple(s_ripple[0], 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ripple[0]);
    lv_anim_set_exec_cb(&a, anim_ripple);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_time(&a, f->ring_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, ripple_ready_cb);
    lv_anim_start(&a);

    if (f->wash > 0) {
        lv_anim_del(s_scr, anim_bg_tint);
        anim_bg_tint(s_scr, 0);

        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, s_scr);
        lv_anim_set_exec_cb(&b, anim_bg_tint);
        lv_anim_set_values(&b, 0, f->wash);
        lv_anim_set_time(&b, 90);
        lv_anim_set_playback_time(&b, f->hold_ms);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_start(&b);
    }

    if (f->sparks == 0) {
        return;
    }
    ui_stop_particles();
    if (f->rise < 0) {
        ui_start_confetti(); /* DONE: the pool is free there, so throw paper */
        return;
    }
    for (int i = 0; i < f->sparks && i < PART_N; i++) {
        part_cfg_t *c = &s_part_cfg[i];

        c->x0  = BODY_CX + (int32_t)lv_rand(0, BODY_D) - BODY_D / 2;
        c->y0  = BODY_CY;
        c->x1  = 0;
        c->y1  = f->rise + (int32_t)lv_rand(0, 6);
        c->ms  = 260 + lv_rand(0, 140);
        c->opa = 255;

        lv_obj_set_pos(s_part[i], c->x0, c->y0);
        lv_obj_set_style_bg_color(s_part[i], lv_color_hex(COL_SPARKLE), 0);
        lv_obj_set_style_radius(s_part[i], LV_RADIUS_CIRCLE, 0);
        anim_mote(s_part[i], 0);

        lv_anim_t c2;
        lv_anim_init(&c2);
        lv_anim_set_var(&c2, s_part[i]);
        lv_anim_set_exec_cb(&c2, anim_mote);
        lv_anim_set_values(&c2, 0, 1000);
        lv_anim_set_delay(&c2, (uint32_t)i * 40);
        lv_anim_set_time(&c2, c->ms);
        lv_anim_set_path_cb(&c2, lv_anim_path_ease_out);
        lv_anim_start(&c2);
    }
}

/* ---- mood ---------------------------------------------------------------- */

static mood_t mood_for(const herdr_status_t *s)
{
    if (!s->online) return MOOD_OFFLINE;

    bool blocked = false, working = false, done = false, idle = false;
    for (int i = 0; i < s->count; i++) {
        switch (s->agents[i].state) {
        case HERDR_ST_BLOCKED: blocked = true; break;
        case HERDR_ST_WORKING: working = true; break;
        case HERDR_ST_DONE:    done    = true; break;
        default:               idle    = true; break;
        }
    }
    if (blocked) return MOOD_BLOCKED;
    if (working) return MOOD_WORKING;
    if (done)    return MOOD_DONE;
    if (idle)    return MOOD_IDLE;
    return MOOD_SLEEP; /* count == 0: nobody is running */
}

/* Every animation owned by an object is deleted before its replacement starts,
 * so a mood change never leaves two animations fighting over one property. */
static void ui_apply_mood(mood_t mood)
{
    const mood_cfg_t *m = &s_moods[mood];
    s_mood = mood;

    lv_obj_set_style_bg_color(s_body, lv_color_hex(m->body), 0);

    lv_label_set_text(s_headline, m->headline);
    lv_obj_set_style_text_color(s_headline, lv_color_hex(m->body), 0);

    /* mouth: a smile arc, or a flat bar for sleep/offline */
    if (m->flat_mouth) {
        lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_mouth_flat, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mouth_flat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_width(s_mouth, m->mouth_w, LV_PART_INDICATOR);
        lv_arc_set_angles(s_mouth, m->mouth_start, m->mouth_end);
    }

    ui_start_blink();

    /* eyes recentre instantly on a mood change, then keep glancing */
    lv_anim_del(s_eye[0], anim_eye_look);
    anim_eye_look(NULL, 0);
    s_look_wait = 5; /* hold the new expression for ~1 s before the first glance */

    /* mouth movement (working only) */
    ui_start_talk();

    /* ring + sweep */
    lv_anim_del(s_sweep, anim_sweep_rotation);
    if (m->ring) {
        lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
        start_anim(s_sweep, anim_sweep_rotation, 0, 360, m->ring_ms, 0, lv_anim_path_linear);
    } else {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
    }
    ui_start_glow();

    /* alert marker */
    lv_anim_del(s_alert, anim_alert_blink);
    if (m->alert_ms) {
        lv_obj_clear_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
        start_anim(s_alert, anim_alert_blink, 255, 0,
                   m->alert_ms / 2, m->alert_ms - m->alert_ms / 2, lv_anim_path_linear);
    } else {
        lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    }

    /* "z"s */
    ui_start_z();
    if (m->z_ms) {
        lv_obj_clear_flag(s_z, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_z2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_z, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_z2, LV_OBJ_FLAG_HIDDEN);
    }

    /* breathing */
    ui_start_breathe();

    /* decorations */
    for (int i = 0; i < 2; i++) {
        if (m->glint) {
            lv_obj_clear_flag(s_glint[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_glint[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_start_blush();
    ui_start_sweat();
    ui_stop_particles();
    ui_start_motes();

    /* Body bob: restart under the new tempo, unless a tap squash owns the body
     * right now (ui_tick re-arms the bob once the squash is done). */
    lv_anim_del(s_body, anim_body_bob);
    s_bob_running = false;
    if (!s_squash_running) ui_start_bob();
}

/* ---- list + summary ----------------------------------------------------- */

static uint32_t state_colour(herdr_agent_state_t st)
{
    switch (st) {
    case HERDR_ST_BLOCKED: return COL_BLOCKED;
    case HERDR_ST_WORKING: return COL_WORKING;
    case HERDR_ST_DONE:    return COL_DONE;
    case HERDR_ST_IDLE:    return COL_IDLE;
    default:               return COL_DIM;
    }
}

static void ui_render_list(const herdr_status_t *s)
{
    /* s_page selects which slice of the list is on screen; the rows themselves
     * never move, so the layout stays identical from page to page. */
    const int first = s_page * UI_ROWS;
    const int shown = (s->count - first < UI_ROWS) ? (s->count - first) : UI_ROWS;

    for (int i = 0; i < UI_ROWS; i++) {
        if (i >= shown || i + first >= s->count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        /* Never route agent text through a format string. */
        const herdr_agent_t *a = &s->agents[first + i];
        char                 buf[64];

        if (a->focused) {
            snprintf(buf, sizeof buf, "> %.10s", a->label);
        } else {
            snprintf(buf, sizeof buf, "%.12s", a->label);
        }
        lv_label_set_text(s_row_label[i], buf);
        lv_label_set_text(s_row_status[i], herdr_state_name(a->state));

        lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(state_colour(a->state)), 0);
        lv_obj_set_style_opa(s_dot[i], s->online ? LV_OPA_COVER : 100, 0);
        lv_obj_set_style_text_color(s_row_label[i], lv_color_hex(s->online ? COL_TEXT : COL_DIM), 0);
        lv_obj_set_style_text_color(s_row_status[i], lv_color_hex(s->online ? COL_TEXT : COL_DIM), 0);
    }
}

static void ui_render_summary(const herdr_status_t *s)
{
    if (!s->online) {
        lv_label_set_text(s_summary, "no link");
        return;
    }
    if (s->count == 0) {
        lv_label_set_text(s_summary, "no agents");
        return;
    }

    unsigned counts[4] = { 0, 0, 0, 0 }; /* blocked, working, done, idle(+unknown) */
    static const char *const names[4] = { "blocked", "working", "done", "idle" };
    for (int i = 0; i < s->count; i++) {
        switch (s->agents[i].state) {
        case HERDR_ST_BLOCKED: counts[0]++; break;
        case HERDR_ST_WORKING: counts[1]++; break;
        case HERDR_ST_DONE:    counts[2]++; break;
        default:               counts[3]++; break;
        }
    }

    unsigned parts[2] = { 0, 0 };
    const char *pnames[2] = { "", "" };
    int shown = 0, kinds = 0;
    for (int k = 0; k < 4; k++) {
        if (counts[k] == 0) continue;
        kinds++;
        if (shown < 2) {
            parts[shown] = counts[k];
            pnames[shown] = names[k];
            shown++;
        }
    }

    char buf[64];
    if (shown == 1) {
        snprintf(buf, sizeof buf, "%u %s", parts[0], pnames[0]);
    } else {
        snprintf(buf, sizeof buf, "%u %s, %u %s", parts[0], pnames[0], parts[1], pnames[1]);
    }

    const int pages = (s->count + UI_ROWS - 1) / UI_ROWS;
    if (pages > 1) {
        /* More agents than rows: the page marker is the useful fact, and what is
         * not on this page is exactly what the next page shows. */
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof buf - len, " p%d/%d", s_page + 1, pages);
    }
    if (s_page * UI_ROWS >= s->count) {
        s_page = 0; /* the list shrank under us: never sit on an empty page */
    }

    /* The row is 156 px of montserrat_12; anything longer gets the short form. */
    if (strlen(buf) > 22) {
        snprintf(buf, sizeof buf, "%d agents", s->count);
    }
    lv_label_set_text(s_summary, buf);
}

/* ---- update loop -------------------------------------------------------- */

/* Runs in the LVGL task (lv_timer), so it is the only task that touches LVGL. */
static void ui_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    herdr_status_t s;
    if (!herdr_client_get(&s)) return;

    if (!s_bob_running && !s_squash_running) ui_start_bob();

    const mood_t mood = mood_for(&s);
    const bool mood_changed = (mood != s_last_mood);
    const bool online_changed = (s.online != s_last_online);
    const bool fresh = (s.gen != s_last_gen) || online_changed;
    s_last_gen = s.gen;
    s_last_online = s.online;
    if (!fresh) return;

    if (mood != s_mood) {
        ui_apply_mood(mood);
        ui_start_burst();
    }
    ui_render_list(&s);
    ui_render_summary(&s);

    /* The other two surfaces only need refreshing while they are visible. */
    if (s_view == UI_VIEW_STATS) {
        ui_stats_render();
    }
    if (s_overlay != NULL) {
        ui_overlay_render();
    }

    if (mood_changed || online_changed) {
        UI_LOGI(TAG, "mood=%s online=%d agents=%d overflow=%d",
                s_mood_names[mood], (int)s.online, s.count, s.overflow);
    }
    s_last_mood = mood;
}

/* ---- tap ---------------------------------------------------------------- */

/* One press, three meanings — the whole input vocabulary of this panel, taken
 * straight from LVGL's pointer events rather than from a reader of our own:
 *
 *   click        refresh from the bridge and play the mood's flourish
 *   long press   raise or drop the diagnostics overlay
 *   swipe        left/right switches view, up/down pages
 *
 * LVGL fires CLICKED on release "regardless to long press" (lv_event.h), so both
 * a long press and a swipe are remembered here and keep their release from also
 * reading as a tap. */
/* Two halves, four gestures, and no swipes.
 *
 * Swipes did the view switch and the paging, and they were the least reliable
 * thing on this panel: a drag that LVGL reads as a gesture also suppresses the
 * click, so a swipe that fell short did nothing at all, and one that carried on
 * into the wrong object did something else. The screen is small enough to reach
 * every corner, so the vocabulary is now positional.
 *
 * The long side of the screen is split into two equal halves, and the artwork
 * keeps the same roles in both orientations — portrait puts the face in the top
 * half with the agent list under it, landscape puts the face in the left half
 * with the list beside it (see ui_layout_init) — so "the face's half" is always
 * the first half along the split axis.
 *
 *   tap    the face's half    refresh from the bridge, play the flourish
 *   tap    the list's half    forward a page (the agent list, or the stats page)
 *   double the face's half    the other view
 *   double the list's half    the diagnostics overlay
 *   hold   anywhere           the overlay too, unchanged
 *
 * The face's tap fires immediately: it is the one that wants feedback, and its
 * double (the other view) is orthogonal to it, so both may happen. The list's tap
 * pages, which a double cannot also do, so that one waits out the double window
 * (DOUBLE_MS) and is cancelled if a second tap arrives. */
#define DOUBLE_MS 350   /* slow enough for a deliberate double tap */

static bool       s_long_fired;
static bool       s_have_last_click;
static uint32_t   s_last_click_tick;
static bool       s_last_click_list;
static lv_timer_t *s_page_timer;   /* the list's single tap, waiting out the double window */

/* The list's tap pages forward, but only once the double window has passed without
 * a second tap: paging straight away and undoing it on a double flickers through a
 * page nobody asked for, and depends on the exact state the previous click left
 * behind. The face's tap stays immediate — that is the one that wants feedback. */
static void page_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_page_timer = NULL;   /* one-shot: LVGL frees it once this returns */
    ui_companion_on_page(1);
}

static void screen_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_long_fired = false;
        break;

    case LV_EVENT_LONG_PRESSED:
        s_long_fired = true;
        s_longs++;
        ui_companion_on_toggle_overlay();
        break;

    case LV_EVENT_CLICKED: {
        if (s_long_fired) {
            break; /* the long press owns this press */
        }

        const lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) {
            break;
        }

        lv_point_t p = { 0, 0 };
        lv_indev_get_point(indev, &p);

        /* Portrait: the list is the bottom half. Landscape: the list is the right
         * half, its column starting at x=148 of 320. */
        const bool list_half = SPLIT ? (p.x >= SCR_W / 2) : (p.y >= SCR_H / 2);

        const bool dbl = s_have_last_click && (s_last_click_list == list_half) &&
                         lv_tick_elaps(s_last_click_tick) <= DOUBLE_MS;

        s_have_last_click = !dbl; /* a third click starts over rather than chaining */
        s_last_click_tick = lv_tick_get();
        s_last_click_list = list_half;

        if (dbl) {
            /* Cancel the page the first click of this pair left waiting: the pair
             * means the overlay, and the list should not also move. */
            if (s_page_timer != NULL) {
                lv_timer_del(s_page_timer);
                s_page_timer = NULL;
            }

            s_doubles++;
            if (list_half) {
                ui_companion_on_toggle_overlay();
            } else {
                ui_companion_on_switch_view(1);
            }
            break;
        }

        if (list_half) {
            if (s_page_timer != NULL) {
                lv_timer_del(s_page_timer);
            }
            s_page_timer = lv_timer_create(page_timer_cb, DOUBLE_MS, NULL);
            lv_timer_set_repeat_count(s_page_timer, 1);
        } else {
            ui_companion_on_tap();
        }
        break;
    }
    default:
        break;
    }
}

/* The screen's click handler (see screen_event_cb). */
void ui_companion_on_tap(void)
{
    s_taps++;  /* kept here rather than in the event callback so the harness's
                * direct calls are counted too */
    herdr_client_poll_now();
    ui_flourish_start();

    if (s_squash_running) return;

    /* The squash owns the body's y/height until it finishes. */
    lv_anim_del(s_body, anim_body_bob);
    s_bob_running = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_body);
    lv_anim_set_exec_cb(&a, anim_body_squash);
    lv_anim_set_values(&a, BODY_D, BODY_D - BODY_SQUASH_D);
    lv_anim_set_time(&a, 120);
    lv_anim_set_playback_time(&a, 120);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, squash_ready_cb);
    lv_anim_start(&a);
    s_squash_running = true;
}

/* ---- entry point -------------------------------------------------------- */

void ui_companion_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    s_scr = scr;
    ui_layout_init(lv_obj_get_width(scr), lv_obj_get_height(scr));

    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE); /* press/gesture land here */
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Two views, each a container at the screen's origin, so every child keeps the
     * coordinates it would have had on the screen itself and switching views is a
     * single hidden flag. The overlay is created on demand and sits above both. */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = lv_obj_create(scr);

        make_passive(c);
        lv_obj_set_size(c, SCR_W, SCR_H);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);

        if (i == 0) {
            s_mood_cont = c;
        } else {
            s_stats_cont = c;
        }
    }

    /* Bottom-most children: the mood-change rings wash out from behind the face. */
    for (int i = 0; i < RIPPLE_N; i++) {
        s_ripple[i] = create_blob(s_mood_cont, BODY_D, BODY_D, BODY_CX, BODY_CY);
        lv_obj_set_style_border_width(s_ripple[i], RIPPLE_W, 0);
        lv_obj_set_style_border_opa(s_ripple[i], LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_ripple[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Particle pool, also behind the face: sparkles, dust and confetti all
     * radiate from behind the blob. */
    for (int i = 0; i < PART_N; i++) {
        s_part[i] = create_blob(s_mood_cont, PART_D, PART_D, 0, 0);
        lv_obj_set_style_bg_opa(s_part[i], LV_OPA_TRANSP, 0);
    }

    s_headline = lv_label_create(s_mood_cont);
    make_passive(s_headline);
    lv_obj_set_style_text_font(s_headline, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(s_headline, 1, 0);
    if (SPLIT) {
        lv_obj_set_pos(s_headline, HEADLINE_X, HEADLINE_Y);
    } else {
        lv_obj_align(s_headline, LV_ALIGN_TOP_MID, 0, HEADLINE_Y);
    }

    /* The ring is the static track; the sweep spins on top of it. */
    s_ring = create_blob(s_mood_cont, RING_D, RING_D, BODY_CX, BODY_CY);
    lv_obj_set_style_border_width(s_ring, RING_W, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);

    s_sweep = create_arc(s_mood_cont, RING_D, BODY_CX, BODY_CY, RING_W);
    lv_arc_set_angles(s_sweep, 0, SWEEP_SPAN);

    s_body = create_blob(s_mood_cont, BODY_D, BODY_D, BODY_CX, BODY_CY);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, 0);

    for (int i = 0; i < 2; i++) {
        const lv_coord_t bx = BODY_CX + (i == 0 ? -BLUSH_DX : BLUSH_DX);

        s_blush[i] = create_blob(s_mood_cont, BLUSH_W, BLUSH_H, bx, BLUSH_CY);
        lv_obj_set_style_bg_color(s_blush[i], lv_color_hex(COL_BLUSH), 0);
    }

    for (int i = 0; i < 2; i++) {
        s_eye[i] = create_blob(s_mood_cont, EYE_W, EYE_H, BODY_CX + (i == 0 ? -EYE_DX : EYE_DX), EYE_CY);
        lv_obj_set_style_bg_color(s_eye[i], lv_color_hex(COL_FACE), 0);
        lv_obj_set_style_bg_opa(s_eye[i], LV_OPA_COVER, 0);
        /* The glint is a child of the eye, so a glance carries it along and the
         * clip removes it as the lid closes — no animation has to know about it. */
        lv_obj_set_style_clip_corner(s_eye[i], true, 0);
        s_glint[i] = create_blob(s_eye[i], GLINT_D, GLINT_D, GLINT_OX + GLINT_D / 2, GLINT_OY + GLINT_D / 2);
        lv_obj_set_style_bg_color(s_glint[i], lv_color_hex(COL_GLINT), 0);
        lv_obj_set_style_bg_opa(s_glint[i], LV_OPA_COVER, 0);
    }

    s_mouth = create_arc(s_mood_cont, MOUTH_D, MOUTH_CX, MOUTH_CY, MOUTH_W);
    lv_obj_set_style_arc_color(s_mouth, lv_color_hex(COL_FACE), LV_PART_INDICATOR);
    lv_arc_set_angles(s_mouth, 45, 135);

    s_mouth_flat = create_blob(s_mood_cont, MOUTH_FLAT_W, MOUTH_FLAT_H, MOUTH_CX, MOUTH_CY);
    lv_obj_set_style_bg_color(s_mouth_flat, lv_color_hex(COL_FACE), 0);
    lv_obj_set_style_bg_opa(s_mouth_flat, LV_OPA_COVER, 0);

    /* In front of the face: it has to slide over the cheek. */
    s_sweat = create_blob(s_mood_cont, SWEAT_W, SWEAT_H, SWEAT_X0, SWEAT_Y0);
    lv_obj_set_style_bg_color(s_sweat, lv_color_hex(COL_DROP), 0);

    s_alert = lv_label_create(s_mood_cont);
    make_passive(s_alert);
    lv_label_set_text(s_alert, "!");
    lv_obj_set_style_text_font(s_alert, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_alert, lv_color_hex(COL_BLOCKED), 0);
    lv_obj_set_pos(s_alert, ALERT_X, ALERT_Y);

    s_z = lv_label_create(s_mood_cont);
    make_passive(s_z);
    lv_label_set_text(s_z, "z");
    lv_obj_set_style_text_font(s_z, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_z, lv_color_hex(COL_DIM), 0);
    lv_obj_set_pos(s_z, Z_CX, Z_BASE_Y);

    s_z2 = lv_label_create(s_mood_cont);
    make_passive(s_z2);
    lv_label_set_text(s_z2, "z");
    lv_obj_set_style_text_font(s_z2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_z2, lv_color_hex(COL_DIM), 0);
    lv_obj_set_pos(s_z2, Z_CX + Z2_DX, Z_BASE_Y);

    for (int i = 0; i < UI_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_mood_cont);
        make_passive(row);
        lv_obj_set_size(row, LIST_W, LIST_ROW_H);
        lv_obj_set_pos(row, LIST_X, LIST_Y0 + i * LIST_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        s_row[i] = row;

        s_dot[i] = create_blob(row, DOT_D, DOT_D, DOT_D / 2, LIST_ROW_H / 2);
        lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);

        s_row_label[i] = lv_label_create(row);
        make_passive(s_row_label[i]);
        lv_obj_set_style_text_font(s_row_label[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_row_label[i], ROW_LABEL_DX, (LIST_ROW_H - ROW_TEXT_H) / 2);

        s_row_status[i] = lv_label_create(row);
        make_passive(s_row_status[i]);
        lv_obj_set_style_text_font(s_row_status[i], &lv_font_montserrat_12, 0);
        lv_obj_align(s_row_status[i], LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_summary = lv_label_create(s_mood_cont);
    make_passive(s_summary);
    lv_obj_set_style_text_font(s_summary, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_summary, lv_color_hex(COL_DIM), 0);
    if (SPLIT) {
        lv_obj_set_pos(s_summary, SUMMARY_X, SUMMARY_Y);
    } else {
        lv_obj_align(s_summary, LV_ALIGN_TOP_MID, 0, SUMMARY_Y);
    }

    /* Stats view: a title plus a fixed block of lines, refreshed by
     * ui_stats_render() whenever it is on screen. */
    s_stats_title = lv_label_create(s_stats_cont);
    make_passive(s_stats_title);
    lv_obj_set_style_text_font(s_stats_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_stats_title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_stats_title, "STATS");
    if (SPLIT) {
        lv_obj_set_pos(s_stats_title, HEADLINE_X, HEADLINE_Y);
    } else {
        lv_obj_align(s_stats_title, LV_ALIGN_TOP_MID, 0, HEADLINE_Y);
    }

    for (int i = 0; i < STATS_LINES; i++) {
        s_stats_txt[i] = lv_label_create(s_stats_cont);
        make_passive(s_stats_txt[i]);
        lv_obj_set_style_text_font(s_stats_txt[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_stats_txt[i], lv_color_hex(COL_TEXT), 0);
        lv_obj_set_pos(s_stats_txt[i], SPLIT ? HEADLINE_X : 10,
                       HEADLINE_Y + 20 + i * (SPLIT ? 12 : 14));
        lv_label_set_text(s_stats_txt[i], "");
    }

    /* A rebuilt screen starts on the companion view, page one, no overlay: those
     * pointers all died with the previous screen. */
    if (s_page_timer != NULL) {
        lv_timer_del(s_page_timer);   /* it would fire onto the previous screen */
        s_page_timer = NULL;
    }
    s_long_fired      = false;
    s_have_last_click = false;
    s_view            = UI_VIEW_MOOD;
    s_page         = 0;
    s_stats_page   = 0;
    s_stats_pages  = 2;
    s_overlay      = NULL;
    s_overlay_text = NULL;
    ui_apply_view();

    /* Deterministic first frame: OFFLINE matches the pre-poll shared state
     * (gen 0, online false), so the first ui_tick that carries data re-renders. */
    s_last_gen = 0;
    s_last_online = false;
    s_last_mood = MOOD_OFFLINE;
    ui_apply_mood(MOOD_OFFLINE);

    const herdr_status_t empty = { 0 };
    ui_render_list(&empty);
    ui_render_summary(&empty);

    /* Glances are event-like, so they run off their own fixed-period timer with
     * an internal random countdown (see ui_look_fire). */
    lv_timer_create(ui_look_fire, UI_LOOK_TICK, NULL);

    lv_timer_create(ui_tick, UI_LOOK_TICK, NULL);
}
