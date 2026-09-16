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

/* The mood face is 0015/lvgl_kawaii_face (MIT), vendored in components/: it draws
 * its own eyes, blush, mouth, tears and sparkles on LVGL canvases and knows a set of
 * expressions, which is what the mood table below hands it. LVGL-only, like this
 * file, so the host harness renders it too. */
#include "mood_face.h"

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
/* The panel's one layout: 172x320, the face above the agent list. This was a table
 * of two orientations switched by the IMU; with that gone the numbers are constants
 * and the macros that read them are names for them.
 *
 * Everything the old face needed is gone with it (eyes, mouth, blush, ring, alert
 * marker, "z"s, sweat): the kawaii widget lays those out inside its own panel. */
static lv_coord_t s_scr_w, s_scr_h;   /* the screen's size, read once at create */

#define SCR_W       (s_scr_w)
#define SCR_H       (s_scr_h)
#define HEADLINE_Y  10
#define BODY_CX     (SCR_W / 2)   /* the face is centred on the panel */
#define BODY_CY     126           /* ~10% below centre: the widget draws its mouth under
                                     its own centre, so the box sits higher than the art */
#define BODY_D      100           /* the face's panel: the widget fills whatever it gets */
#define RIPPLE_D1   (BODY_D + 80) /* the mood-change ring's travel */
#define PART_TOP0   6             /* ambient motes above the face */
#define PART_TOP1   42
#define PART_LOW0   172           /* ...and below it */
#define PART_LOW1   186
#define LIST_X      8
#define LIST_Y0     194
#define LIST_ROW_H  24
#define LIST_W      (SCR_W - 16)
#define SUMMARY_Y   300

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
/* The screen's size, which the layout constants above are written against (and
 * which the harness can vary to check that nothing assumes more than it needs). */
static void ui_layout_init(lv_coord_t w, lv_coord_t h)
{
    s_scr_w = w;
    s_scr_h = h;
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
    uint32_t       body;       /* headline colour, and the mood-change tint */
    const char    *headline;
    mood_face_t    face;       /* the resting expression */
    mood_face_t    reaction;   /* what a change into this mood plays first */
    /* Ambient particles only: the kawaii face brings its own eyes, blush, mouth,
     * tears and sparkles, so the blob's decorations are gone with the blob. */
    bool           busy;       /* sweep the activity bar behind the headline */
    uint32_t       activity_ms;/* how long one sweep takes */
    uint32_t       mote_ms;    /* ambient mote cycle; 0 == none */
    lv_opa_t       mote_opa;   /* peak mote opacity */
    int32_t        mote_rise;  /* mote travel, px */
    bool           party;      /* confetti on a mood change */
} mood_cfg_t;

/* One animation per property per mood — every property below (y, width, x,
 * height, angles, arc width, opa, border opa, bg opa) is written by at most one
 * running animation, so nothing fights over a value. See ui_apply_mood(). */
static const mood_cfg_t s_moods[MOOD_N] = {
    /* Mood to expression. The kawaii face has seventeen, so each mood gets the one
     * that reads right, and a reaction that says what *changed* before the resting
     * expression takes over (see s_face_reaction in ui_tick). */
    [MOOD_BLOCKED] = { .body = COL_BLOCKED, .headline = "NEEDS YOU",
                       .face = MOOD_FACE_WORRIED,      .reaction = MOOD_FACE_SURPRISED },
    [MOOD_WORKING] = { .body = COL_WORKING, .headline = "WORKING",
                       .face = MOOD_FACE_WORKING,      .reaction = MOOD_FACE_COOL,
                       .busy = true, .activity_ms = 1400,
                       .mote_ms = 800, .mote_opa = 255, .mote_rise = 16 },
    [MOOD_DONE]    = { .body = COL_DONE, .headline = "DONE",
                       .face = MOOD_FACE_HAPPY,        .reaction = MOOD_FACE_EXCITED,
                       .party = true },
    [MOOD_IDLE]    = { .body = COL_IDLE, .headline = "IDLE",
                       .face = MOOD_FACE_NEUTRAL,      .reaction = MOOD_FACE_SMIRK },
    [MOOD_SLEEP]   = { .body = COL_SLEEP, .headline = "NO AGENTS",
                       .face = MOOD_FACE_SLEEPY,       .reaction = MOOD_FACE_SLEEPY,
                       .mote_ms = 3200, .mote_opa = 110, .mote_rise = 26 },
    [MOOD_OFFLINE] = { .body = COL_OFFLINE, .headline = "OFFLINE",
                       .face = MOOD_FACE_SAD,          .reaction = MOOD_FACE_CONFUSED },
};

static const char *s_mood_names[MOOD_N] = {
    "blocked", "working", "done", "idle", "sleep", "offline",
};

/* ---- state -------------------------------------------------------------- */

static const char *TAG = "ui";

static lv_obj_t *s_scr; /* the active screen: background tint target */
static lv_obj_t *s_tint;       /* the mood-coloured panel behind the face */
static lv_obj_t *s_activity;   /* its track */
static lv_obj_t *s_activity_hl;/* ...and the highlight that sweeps along it */
static lv_obj_t *s_headline;
static lv_obj_t *s_face;       /* the kawaii face's parent panel: it fills this */
static lv_obj_t *s_part[PART_N];
static lv_obj_t *s_ripple[RIPPLE_N];
/* Each ring's own diameter range. The fade is driven by the animation's progress
 * rather than by the current diameter: a size-tied fade (v - BODY_D over the
 * burst's RIPPLE_D1) stops part-way when a shorter animation — a tap flourish —
 * ends early, which left a faint ring parked around the face. */
static struct {
    int32_t d0, d1;
} s_ripple_geom[RIPPLE_N];
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
/* The stats view is a two-column table: a label on the left, a figure on the right,
 * a hairline under each section header, and a status dot in front of every session
 * row. The right column is a separate label because the fonts here are not
 * monospaced — padding with spaces would not line anything up. */
#define STATS_DOT_D  6
#define STATS_ROWS   (STATS_LINES - 5)   /* a header, three totals, a header, then rows */
#define STATS_X_LEFT (10)
#define STATS_TEXT_X (22)   /* body lines that carry a dot */
#define STATS_DOT_X  (10)
#define STATS_ROW_Y(i) (HEADLINE_Y + 20 + (i) * (14))

static lv_obj_t *s_stats_txt[STATS_LINES];
static lv_obj_t *s_stats_val[STATS_LINES];   /* right column, right-aligned */
static lv_obj_t *s_stats_rule[2];            /* hairline under a section header */
static lv_obj_t *s_stats_dot[STATS_ROWS];    /* one per session row */

static uint32_t  s_taps;      /* interaction counters, shown by the overlay */
static uint32_t  s_longs;
static uint32_t  s_doubles;
static uint8_t   s_page_due;  /* beats left before the list's page lands, 0 = none */

/* --- the mood panel ------------------------------------------------------- */

/* The tinted panel behind the face, and the headline that sits on it. The panel is
 * the mood's own colour at low opacity over the screen's near-black, so the text has
 * to be chosen against *that* rather than against the screen: the mood colour where
 * it reads there, and a light one where it does not (SLEEP and OFFLINE are dark
 * enough to vanish into their own tint). */
#define COL_RATE      0x5CD8FF   /* the tokens/s figure in a row */
#define COL_MONEY     0xFFC844   /* every $ figure, here and on the stats page */

#define TINT_H        186    /* ~58% of the panel: down to just above the agent list */
#define TINT_OPA      46     /* "slightly transparent": the screen stays dominant */
#define TINT_LUMA_GAP 60     /* how much brighter the text must be than the panel */

#define ACT_X         0      /* the activity bar: hard against the top edge, full width */
#define ACT_Y         0
#define ACT_W         SCR_W
#define ACT_H         5
#define ACT_HL_W      40
#define MOOD_HEAD_Y   18     /* the headline sits under the bar, not on it: the highlight
                              * would otherwise sweep under the text and wreck its
                              * contrast on the way past */

/* Rec. 601, integer: plenty for a contrast decision. */
static uint8_t colour_luma(uint32_t rgb)
{
    const unsigned r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb & 0xFFu;

    return (uint8_t)((r * 30u + g * 59u + b * 11u) / 100u);
}

/* `rgb` at `opa` over the screen's background: what the panel actually looks like. */
static uint32_t blend_over_bg(uint32_t rgb, uint8_t opa)
{
    const unsigned r  = (rgb >> 16) & 0xFFu, g  = (rgb >> 8) & 0xFFu, b  = rgb & 0xFFu;
    const unsigned br = (COL_BG >> 16) & 0xFFu, bg = (COL_BG >> 8) & 0xFFu, bb = COL_BG & 0xFFu;

    const unsigned nr = br + (r - br) * opa / 255u;
    const unsigned ng = bg + (g - bg) * opa / 255u;
    const unsigned nb = bb + (b - bb) * opa / 255u;

    return (nr << 16) | (ng << 8) | nb;
}
static uint8_t   s_face_reaction;  /* beats left of a mood-change reaction, 0 = none */
#define FACE_REACTION_TICKS 6   /* how long a mood change holds its reaction */
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
static uint32_t s_last_gen;

/* Output tokens per second, per agent, from successive /stats fetches: the list shows
 * this for a working agent instead of the word "working", because a number that moves
 * says more about a busy one than a status that has not changed in an hour. A session
 * that has gone quiet reports zero rather than its last burst. */
#define RATE_STALE_S 20
static uint32_t s_rate[HERDR_MAX_AGENTS];
static uint32_t s_prev_out[HERDR_MAX_AGENTS];
static uint32_t s_out_changed_ms[HERDR_MAX_AGENTS];  /* when each total last moved */
static uint32_t s_sessions_sig;      /* the rows' figures, summed: see ui_tick */
static bool     s_sessions_moved;
static bool     s_have_prev;
static bool     s_last_online;
static mood_t   s_last_mood;
static uint32_t s_burst_colour;  /* mood colour of the ripple/tint burst in flight */
static int32_t  s_eye_dx;        /* current sideways glance offset, px */

/* Per-particle parameters, filled in when a mote/confetti run starts. Motes use
 * x0/y0 as the resting spot and y1 as the travel; confetti uses x0,y0 -> x1,y1. */
typedef struct {
    int32_t  x0, y0, x1, y1;
    uint32_t ms;
    lv_opa_t opa;
} part_cfg_t;

static part_cfg_t s_part_cfg[PART_N];

/* ---- animation callbacks ------------------------------------------------ */


/* Tap feedback: squash towards the centre, then LVGL plays it back. */


/* Breathing: the face gets a little wider and back, always about its centre.
 * Width and x only — y belongs to the bob, height to the tap squash. */

/* Both eyes glance sideways together; their spacing never changes. */

/* Ring + sweep glow breathing. The sweep's rotation is a different animation on
 * a different property, so the two coexist. */

/* Working face mutters to itself: the smile thickens and thins. Animating the
 * arc's angles instead moved each corner by ~2 px — invisible. */



/* Single callback: the "z" drifts up and fades out over one span. */

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

/* Sweat: the first half of the cycle slides and fades; the rest is parked
 * invisible, which is how a one-value animation gets a pause. */

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




/* The ring only exists in the WORKING mood, so the glow follows it. */


/* Two "z"s, the second half a cycle behind the first. */

/* Cheek blush fades in and out on its own slow cycle. */


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
        lv_obj_delete(s_overlay); /* takes the label with it */
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
    herdr_status_t     s    = { 0 };

    herdr_client_stats(&link);
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
             "input   tap %u  long %u\n"
             "        dbl %u  view %s",
             (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60),
             (unsigned)(ui_device_free_heap() / 1024), (unsigned)(ui_device_min_free_heap() / 1024),
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct,
             (unsigned)link.gen, link.online ? "online" : "offline",
             (unsigned)link.polls, (unsigned)link.fail_total,
             (unsigned)link.rtt_ms, (unsigned)link.failures,
             have ? s.count : 0, have ? s.overflow : 0,
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

/* The activity bar's highlight slides across its track: a plain linear sweep, the
 * shape of "something is happening" without a percentage to report. */
static void anim_activity(void *var, int32_t v)
{
    lv_obj_t *hl = var;

    lv_obj_set_x(hl, ACT_X + (ACT_W - ACT_HL_W) * v / 1000);
}

/* Defined with the mood view's list; the session rows use the same colours. */
static uint32_t state_colour(herdr_agent_state_t st);

/* Micro-dollars as money. Integer arithmetic throughout: a float here would be
 * slower on this chip and could print $4.55 where the figure is $4.56. */
static void fmt_cost(char *buf, size_t n, uint32_t micro)
{
    /* Round to cents before splitting dollars off, so $12.7459 reads $12.75 rather
     * than $12.74 — and so the carry can move the dollar figure itself up. */
    const uint32_t cents_total = (micro + 5000u) / 10000u;
    const uint32_t whole       = cents_total / 100u;
    const uint32_t cents       = cents_total % 100u;

    if (whole >= 1000u) {
        snprintf(buf, n, "$%u.%uk", (unsigned)(whole / 1000u), (unsigned)((whole / 100u) % 10u));
    } else if (whole >= 100u) {
        snprintf(buf, n, "$%u", (unsigned)whole);
    } else {
        snprintf(buf, n, "$%u.%02u", (unsigned)whole, (unsigned)cents);
    }
}

/* A line's role changes with the page — line 5 is a device figure on page one and a
 * session row on page two — so the style is set on every render rather than left to
 * whatever the previous page wanted. */
static void stats_style(int idx, bool header, lv_coord_t x)
{
    if (idx < 0 || idx >= STATS_LINES || s_stats_txt[idx] == NULL) {
        return;
    }

    /* Headers keep the 12 px face for the hierarchy; the rows take 10 px, because a
     * full row (dot, label, token count, money) does not fit 172 px at 12: measured,
     * the widest label ran 4 px into its figure. */
    const lv_font_t *font = header ? &lv_font_montserrat_12 : &lv_font_montserrat_10;

    lv_obj_set_style_text_font(s_stats_txt[idx], font, 0);
    lv_obj_set_style_text_font(s_stats_val[idx], font, 0);
    lv_obj_set_style_text_color(s_stats_txt[idx], lv_color_hex(header ? COL_DIM : COL_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_stats_txt[idx], header ? 1 : 0, 0);
    lv_obj_set_x(s_stats_txt[idx], header ? STATS_X_LEFT : x);
    lv_obj_set_style_text_color(s_stats_val[idx], lv_color_hex(COL_TEXT), 0);
}

/* One right-hand figure. Its own label, because there is nothing to right-align
 * against otherwise. */
static void stats_val(int idx, const char *fmt, ...)
{
    if (idx < 0 || idx >= STATS_LINES || s_stats_val[idx] == NULL) {
        return;
    }

    char    buf[32];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    lv_label_set_text(s_stats_val[idx], buf);
}

/* A section heading, with the hairline that separates it from what follows. */
static void stats_header(int idx, const char *text)
{
    stats_style(idx, true, 0);
    stats_line(idx, "%s", text);
    stats_val(idx, "");

    lv_obj_t *rule = s_stats_rule[idx == 0 ? 0 : 1];

    lv_obj_set_pos(rule, STATS_X_LEFT, STATS_ROW_Y(idx) + 13);
    lv_obj_set_width(rule, SCR_W - 2 * STATS_X_LEFT);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_HIDDEN);
}

/* A body line: left text, right figure, both in the brighter colour. */
static void stats_row(int idx, lv_coord_t x, const char *left)
{
    stats_style(idx, false, x);
    stats_line(idx, "%s", left);
}

static void ui_stats_render(void)
{
    herdr_link_stats_t link = { 0 };

    herdr_client_stats(&link);

    herdr_status_t   s = { 0 };
    const bool       have = herdr_client_get(&s);

    {
        /* The page marker lives in the title: without it there is nothing on screen
         * to say a second page exists. */
        char title[16];

        snprintf(title, sizeof title, "STATS  %u/%u", (unsigned)(s_stats_page + 1),
                 (unsigned)s_stats_pages);
        lv_label_set_text(s_stats_title, title);
    }

    /* Nothing a page does not use may keep the previous page's text: the two pages
     * share these eleven lines and disagree about what belongs on them. */
    for (int i = 0; i < STATS_LINES; i++) {
        stats_style(i, false, STATS_X_LEFT);
        stats_line(i, "");
        stats_val(i, "");
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(s_stats_rule[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < STATS_ROWS; i++) {
        lv_obj_add_flag(s_stats_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Page one is the sessions: the figures worth opening the view for are the
     * tokens and the money, and the link's own health is the second thing anyone
     * wants (it is also the page the long-press overlay summarises). */
    if (s_stats_page == 1) {
        /* Link, device, and the things that go wrong. Each line is a label and a
         * figure, because one long line is what made this view hard to read. */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);

        stats_header(0, "LINK");
        stats_row(1, STATS_X_LEFT, "gen");
        stats_val(1, "%u", (unsigned)link.gen);
        stats_row(2, STATS_X_LEFT, "poll");
        stats_val(2, "%u ok  %u err", (unsigned)link.polls, (unsigned)link.fail_total);
        stats_row(3, STATS_X_LEFT, "rtt");
        stats_val(3, "%u ms  %u fail", (unsigned)link.rtt_ms, (unsigned)link.failures);
        lv_obj_set_style_text_color(s_stats_val[1], lv_color_hex(link.online ? COL_TEXT : COL_BLOCKED), 0);

        stats_header(4, "DEVICE");
        stats_row(5, STATS_X_LEFT, "up");
        stats_val(5, "%uh%02um  %uK", (unsigned)(lv_tick_get() / 3600000u),
                  (unsigned)((lv_tick_get() / 60000u) % 60u), (unsigned)(ui_device_free_heap() / 1024));
        stats_row(6, STATS_X_LEFT, "lvgl");
        stats_val(6, "%u%%  frag %u%%", (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
        stats_row(7, STATS_X_LEFT, "input");
        stats_val(7, "%u tap  %u dbl", (unsigned)s_taps, (unsigned)s_doubles);

        s_stats_pages = 2;
        return;
    }

    /* Sessions page: what the agents have actually been doing, from the bridge's
     * read of their session logs. */
    herdr_sessions_t sess = { 0 };
    const bool       have_sessions = herdr_stats_get(&sess) && sess.valid;

    if (!have_sessions) {
        /* Two lines, ASCII only: the bundled fonts stop at 0x7F, so an em-dash or a
         * bullet renders as an empty box (AGENTS.md §8). */
        stats_header(0, "SESSIONS");
        stats_row(1, STATS_X_LEFT, "no data yet: the bridge");
        stats_row(2, STATS_X_LEFT, "has not answered /stats");
        return;
    }

    char tin[16], tout[16], cost[16], age[12];

    fmt_tokens(tin, sizeof tin, sess.tokens_in);
    fmt_tokens(tout, sizeof tout, sess.tokens_out);
    fmt_cost(cost, sizeof cost, sess.cost_micro);
    fmt_age(age, sizeof age, sess.age_s);

    stats_header(0, "SESSIONS");
    stats_val(0, "%u", (unsigned)sess.sessions);

    stats_row(1, STATS_X_LEFT, "tokens");
    stats_val(1, "%s in  %s out", tin, tout);
    stats_row(2, STATS_X_LEFT, "activity");
    stats_val(2, "%u calls  %u msg", (unsigned)sess.tool_calls, (unsigned)sess.messages);

    /* The figure worth reading first, so it takes the one accent colour on the page. */
    stats_row(3, STATS_X_LEFT, "spend");
    stats_val(3, "%s", cost);
    lv_obj_set_style_text_color(s_stats_val[3], lv_color_hex(COL_MONEY), 0);

    stats_header(4, "AGENTS");

    const int shown = (have && s.count < HERDR_MAX_AGENTS) ? s.count : HERDR_MAX_AGENTS;
    const int rows  = (shown < STATS_ROWS) ? shown : STATS_ROWS;

    for (int i = 0; i < rows; i++) {
        fmt_tokens(tin, sizeof tin, sess.per[i].tokens_in);
        fmt_cost(cost, sizeof cost, sess.per[i].cost_micro);

        /* The label and its token count on the left, the money on the right. */
        stats_style(5 + i, false, STATS_TEXT_X);
        stats_line(5 + i, "%-9.9s %s", have ? s.agents[i].label : "?", tin);
        stats_val(5 + i, "%s", cost);

        lv_obj_set_pos(s_stats_dot[i], STATS_DOT_X, STATS_ROW_Y(5 + i) + 4);
        lv_obj_set_style_bg_color(
            s_stats_dot[i], lv_color_hex(have ? state_colour(s.agents[i].state) : COL_DIM), 0);
        lv_obj_clear_flag(s_stats_dot[i], LV_OBJ_FLAG_HIDDEN);
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

    lv_label_set_text(s_headline, m->headline);

    /* The panel takes the mood's colour, and the headline is chosen against what the
     * panel actually looks like — not against the screen, which is what the old
     * colour choice assumed (and what made SLEEP and OFFLINE unreadable). */
    lv_obj_set_style_bg_color(s_tint, lv_color_hex(m->body), 0);

    const uint32_t panel = blend_over_bg(m->body, TINT_OPA);
    const uint32_t ink   = (colour_luma(m->body) > colour_luma(panel) + TINT_LUMA_GAP)
                           ? m->body : COL_TEXT;

    lv_obj_set_style_text_color(s_headline, lv_color_hex(ink), 0);

    /* The activity bar sweeps while there is something to wait for, in the mood's own
     * colour with a lighter highlight running along it. */
    lv_obj_set_style_bg_color(s_activity, lv_color_hex(m->body), 0);
    lv_obj_set_style_bg_color(s_activity_hl, lv_color_lighten(lv_color_hex(m->body), 120), 0);

    lv_anim_del(s_activity_hl, anim_activity);
    if (m->busy) {
        lv_obj_clear_flag(s_activity, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_activity_hl, LV_OBJ_FLAG_HIDDEN);
        start_anim(s_activity_hl, anim_activity, 0, 1000, m->activity_ms, 0, lv_anim_path_linear);
    } else {
        lv_obj_add_flag(s_activity, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_activity_hl, LV_OBJ_FLAG_HIDDEN);
    }

    /* The face reacts to the change first and settles into the mood's own
     * expression a few beats later (the countdown runs in ui_tick). A mood whose
     * reaction is its expression just changes once. */
    if (m->reaction != m->face) {
        mood_face_set(m->reaction, true);
        s_face_reaction = FACE_REACTION_TICKS;
    } else {
        mood_face_set(m->face, true);
        s_face_reaction = 0;
    }

    /* Ambient motes are the only decoration left that the face does not draw. */
    ui_stop_particles();
    ui_start_motes();
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
        /* What the agent is *doing*, where that is knowable: a busy one reports its
         * token rate, an idle one what its session has cost, and the rest keep the
         * state word they share with the mood. Both figures come from /stats, whose
         * rows are keyed and ordered like the agents. */
        herdr_sessions_t sess = { 0 };
        const bool       have_sessions = herdr_stats_get(&sess) && sess.valid;
        const int        idx = first + i;

        if (have_sessions && a->state == HERDR_ST_WORKING && sess.per[idx].tokens_per_s > 0) {
            char tok[16], rate[24];

            fmt_tokens(tok, sizeof tok, sess.per[idx].tokens_per_s);
            snprintf(rate, sizeof rate, "%s/s", tok);
            lv_label_set_text(s_row_status[i], rate);
            lv_obj_set_style_text_color(s_row_status[i], lv_color_hex(COL_RATE), 0);
        } else if (have_sessions && a->state == HERDR_ST_IDLE) {
            char cost[16];

            fmt_cost(cost, sizeof cost, sess.per[idx].cost_micro);
            lv_label_set_text(s_row_status[i], cost);
            lv_obj_set_style_text_color(s_row_status[i], lv_color_hex(COL_MONEY), 0);
        } else {
            lv_label_set_text(s_row_status[i], herdr_state_name(a->state));
            lv_obj_set_style_text_color(s_row_status[i],
                                        lv_color_hex(s->online ? COL_TEXT : COL_DIM), 0);
        }

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
    /* Every timer in this UI is due every UI_LOOK_TICK. A run that is later than
     * the slack below means the LVGL task did not get the CPU — which is what a
     * marginal WiFi link does to it, since the WiFi task runs at priority 23 and
     * this one at 4. That is the difference between "the input is broken" and "the
     * whole UI was stalled", and it is not otherwise visible: the touch driver's
     * own logs look the same either way.
     *
     * Reported at most once a second, carrying the worst lateness in that second,
     * so a stalling link cannot flood the console with it. */
    {
        static uint32_t s_last_tick_ms;
        static uint32_t s_worst_late_ms;
        static uint32_t s_last_report_ms;

        const uint32_t now = lv_tick_get();

        if (s_last_tick_ms != 0) {
            const uint32_t late = lv_tick_elaps(s_last_tick_ms);

            if (late > UI_LOOK_TICK + 100 && late > s_worst_late_ms) {
                s_worst_late_ms = late;
            }
        }
        s_last_tick_ms = now;

        if (s_worst_late_ms != 0 && lv_tick_elaps(s_last_report_ms) >= 1000) {
            UI_LOGW(TAG, "lvgl late by %u ms", (unsigned)s_worst_late_ms);
            s_worst_late_ms   = 0;
            s_last_report_ms  = now;
        }
    }

    LV_UNUSED(timer);

    herdr_status_t s;
    if (!herdr_client_get(&s)) return;


    /* What the rows display, summed: a change here counts as something to redraw (see
     * the freshness test below), or a working row would keep saying "working" until an
     * agent's state happened to change. The figures themselves come from /stats as it
     * reports them — the rate is omp's own, not something computed here. */
    {
        herdr_sessions_t sess = { 0 };

        if (herdr_stats_get(&sess) && sess.valid) {
            uint32_t sig = 0;

            for (int i = 0; i < HERDR_MAX_AGENTS; i++) {
                sig += sess.per[i].tokens_out + sess.per[i].cost_micro +
                       sess.per[i].tokens_per_s;
            }
            s_sessions_moved = (sig != s_sessions_sig);
            s_sessions_sig   = sig;
        }
    }

    const mood_t mood = mood_for(&s);
    const bool mood_changed = (mood != s_last_mood);
    const bool online_changed = (s.online != s_last_online);
    /* The rows show session figures, so a tick that moves those has to redraw them
     * even when no agent's state changed — otherwise a working row would keep saying
     * "working" until the mood or the agent list happened to change again. The
     * signature is the same numbers the rows read, summed. */
    const bool fresh = (s.gen != s_last_gen) || online_changed || s_sessions_moved;
    s_last_gen = s.gen;
    s_last_online = s.online;
    /* A list tap's page lands here, a few beats after the tap (see PAGE_DELAY_TICKS). */
    if (s_page_due != 0 && --s_page_due == 0) {
        ui_companion_on_page(1);
    }

    /* A mood-change reaction is over: back to the mood's resting face. */
    if (s_face_reaction != 0 && --s_face_reaction == 0) {
        mood_face_set(s_moods[s_mood].face, true);
    }

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
 * every corner, so the vocabulary is positional.
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
 *   hold   anywhere           the overlay too, and nothing else
 *
 * LVGL 9 tells single clicks from doubles itself (SINGLE_CLICKED / DOUBLE_CLICKED,
 * classified inside the long-press time), which is a good deal less code and less
 * guesswork than the hand-rolled window this used to keep: it also means a long
 * press sends no click at all, so a hold cannot be mistaken for a tap.
 *
 * The face's tap acts at once — it is the one that wants feedback, and its double
 * only switches view, which is orthogonal. The list's tap pages, which a double
 * cannot also do, so it waits out the window in a one-shot timer that the double
 * cancels. */
/* The list's tap pages forward, but only if no second tap arrives: paging straight
 * away and undoing it on a double flickers through a page nobody asked for. The wait
 * is counted in ui_tick's own 200 ms beats rather than kept in a one-shot lv_timer,
 * because it has to outlast LVGL's double-click classification — and three beats
 * (~600 ms) clears even a deliberate double. */
#define PAGE_DELAY_TICKS 3
static lv_timer_t *s_ui_timer;     /* the 200 ms ui_tick timer, owned per screen */

static void page_delay_cancel(void)
{
    s_page_due = 0;
}

/* Which half the pointer is in. Portrait: the list is the bottom half. Landscape:
 * the list is the right half, its column starting at x=148 of 320. */
static bool event_in_list_half(lv_event_t *e)
{
    /* LVGL 9 hands the indev over as the event's parameter; lv_event_get_indev() is
     * NULL for these click events. Fall back to the active indev, which is this one
     * while its own event is being dispatched. */
    const lv_indev_t *indev = lv_event_get_param(e);
    lv_point_t        p = { 0, 0 };

    if (indev == NULL) {
        indev = lv_indev_active();
    }
    if (indev == NULL) {
        return false;
    }
    lv_indev_get_point(indev, &p);

    return (p.y >= SCR_H / 2);
}

static void screen_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SINGLE_CLICKED:
        if (event_in_list_half(e)) {
            s_page_due = PAGE_DELAY_TICKS;
        } else {
            ui_companion_on_tap();
        }
        break;

    case LV_EVENT_DOUBLE_CLICKED:
        page_delay_cancel();   /* the pair means the overlay, not a page */
        s_doubles++;

        if (event_in_list_half(e)) {
            ui_companion_on_toggle_overlay();
        } else {
            ui_companion_on_switch_view(1);
        }
        break;

    case LV_EVENT_LONG_PRESSED:
        s_longs++;
        ui_companion_on_toggle_overlay();
        break;

    default:
        break;
    }
}

/* The screen's click handler (see screen_event_cb). */
void ui_companion_on_tap(void)
{
    UI_LOGI(TAG, "tap: refresh from the bridge");

    s_taps++;  /* kept here rather than in the event callback so the harness's
                * direct calls are counted too */
    herdr_client_poll_now();

    /* A wink is the face's acknowledgement of the poke; the ring is the mood's. */
    mood_face_set(MOOD_FACE_WINK, false);
    s_face_reaction = FACE_REACTION_TICKS;

    ui_flourish_start();
}

/* ---- entry point -------------------------------------------------------- */

void ui_companion_create(void)
{
    lv_obj_t *scr = lv_screen_active();
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

    /* The mood panel, created first so everything else draws over it: the face's
     * half of the screen in the mood's own colour at low opacity. What the headline
     * does about it is in ui_apply_mood(). */
    s_tint = lv_obj_create(s_mood_cont);
    make_passive(s_tint);
    lv_obj_set_size(s_tint, SCR_W, TINT_H);
    lv_obj_set_pos(s_tint, 0, 0);
    lv_obj_set_style_bg_opa(s_tint, TINT_OPA, 0);
    lv_obj_set_style_border_width(s_tint, 0, 0);
    lv_obj_set_style_radius(s_tint, 0, 0);
    lv_obj_set_style_pad_all(s_tint, 0, 0);

    /* The activity bar, behind the headline: a dim track and the highlight that
     * sweeps along it while the mood is busy. */
    s_activity = lv_obj_create(s_mood_cont);
    make_passive(s_activity);
    lv_obj_set_size(s_activity, ACT_W, ACT_H);
    lv_obj_set_pos(s_activity, ACT_X, ACT_Y);
    lv_obj_set_style_bg_color(s_activity, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(s_activity, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_activity, ACT_H / 2, 0);
    lv_obj_set_style_border_width(s_activity, 0, 0);
    lv_obj_set_style_pad_all(s_activity, 0, 0);

    s_activity_hl = lv_obj_create(s_mood_cont);
    make_passive(s_activity_hl);
    lv_obj_set_size(s_activity_hl, ACT_HL_W, ACT_H);
    lv_obj_set_pos(s_activity_hl, ACT_X, ACT_Y);
    lv_obj_set_style_bg_color(s_activity_hl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_opa(s_activity_hl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_activity_hl, ACT_H / 2, 0);
    lv_obj_set_style_border_width(s_activity_hl, 0, 0);
    lv_obj_set_style_pad_all(s_activity_hl, 0, 0);

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
    lv_obj_align(s_headline, LV_ALIGN_TOP_MID, 0, MOOD_HEAD_Y);

    /* The face itself: a widget from components/lvgl_kawaii_face (wrapped by
     * mood_face.c) that fills this panel, so the panel's size and position are the
     * whole layout. Everything the blob drew for a face — eyes, glints, blush,
     * mouth, sweat, alert marker, "z"s, ring — comes from it now. */
    s_face = lv_obj_create(s_mood_cont);
    make_passive(s_face);
    lv_obj_set_size(s_face, BODY_D, BODY_D);
    lv_obj_set_pos(s_face, BODY_CX - BODY_D / 2, BODY_CY - BODY_D / 2);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_face, 0, 0);
    lv_obj_set_style_radius(s_face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_face, 0, 0);
    mood_face_create(s_face);

    /* One row per agent: dot, label, state. PAGE_DELAY_TICKS's paging swaps which
     * slice of the list is shown, never the rows themselves. */
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
    lv_obj_align(s_summary, LV_ALIGN_TOP_MID, 0, SUMMARY_Y);

    /* Stats view: a title plus a fixed block of lines, refreshed by
     * ui_stats_render() whenever it is on screen. */
    s_stats_pages = 2;   /* known before the first draw, so the title can say so */
    s_stats_title = lv_label_create(s_stats_cont);
    make_passive(s_stats_title);
    lv_obj_set_style_text_font(s_stats_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_stats_title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_stats_title, "STATS");
    lv_obj_align(s_stats_title, LV_ALIGN_TOP_MID, 0, HEADLINE_Y);

    for (int i = 0; i < STATS_LINES; i++) {
        s_stats_txt[i] = lv_label_create(s_stats_cont);
        make_passive(s_stats_txt[i]);
        lv_obj_set_style_text_font(s_stats_txt[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_stats_txt[i], lv_color_hex(COL_TEXT), 0);
        lv_obj_set_pos(s_stats_txt[i], STATS_X_LEFT, STATS_ROW_Y(i));
        lv_label_set_text(s_stats_txt[i], "");

        /* Right-aligned by alignment, not by padding: this font is not monospaced. */
        s_stats_val[i] = lv_label_create(s_stats_cont);
        make_passive(s_stats_val[i]);
        lv_obj_set_style_text_font(s_stats_val[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_stats_val[i], lv_color_hex(COL_TEXT), 0);
        lv_obj_align(s_stats_val[i], LV_ALIGN_TOP_RIGHT, -10, STATS_ROW_Y(i));
        lv_label_set_text(s_stats_val[i], "");
    }

    /* The hairline under a section header, in the same track colour as the face's
     * ring, so the two views read as one design. */
    for (int i = 0; i < 2; i++) {
        s_stats_rule[i] = lv_obj_create(s_stats_cont);
        make_passive(s_stats_rule[i]);
        lv_obj_set_size(s_stats_rule[i], SCR_W - 2 * STATS_X_LEFT, 1);
        lv_obj_set_style_bg_color(s_stats_rule[i], lv_color_hex(COL_TRACK), 0);
        lv_obj_set_style_bg_opa(s_stats_rule[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_stats_rule[i], 0, 0);
        lv_obj_set_style_radius(s_stats_rule[i], 0, 0);
        lv_obj_set_style_pad_all(s_stats_rule[i], 0, 0);
        lv_obj_add_flag(s_stats_rule[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* One per session row: the agent's own state colour, as in the mood view's list. */
    for (int i = 0; i < STATS_ROWS; i++) {
        s_stats_dot[i] = create_blob(s_stats_cont, STATS_DOT_D, STATS_DOT_D, STATS_DOT_X, 0);
        lv_obj_set_style_bg_color(s_stats_dot[i], lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_bg_opa(s_stats_dot[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_stats_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* A rebuilt screen starts on the companion view, page one, no overlay: those
     * pointers all died with the previous screen. */
    s_page_due = 0;

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


    if (s_ui_timer != NULL) {
        lv_timer_del(s_ui_timer);   /* the previous screen's timer: its screen is gone */
    }
    s_ui_timer = lv_timer_create(ui_tick, UI_LOOK_TICK, NULL);
}
