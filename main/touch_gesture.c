/* Touch gesture recognition. See touch_gesture.h for the idea and the API. */

#include "touch_gesture.h"

#include <stdlib.h>

#define TG_SWIPE_MIN_PX_DEFAULT 30
#define TG_TAP_MAX_PX_DEFAULT   12
#define TG_TAP_MAX_MS_DEFAULT   600

void tg_init(tg_t *t)
{
    if (t == NULL) {
        return;
    }
    tg_cfg_t cfg = {
        .tap_max_ms   = TG_TAP_MAX_MS_DEFAULT,
        .tap_max_px   = TG_TAP_MAX_PX_DEFAULT,
        .swipe_min_px = TG_SWIPE_MIN_PX_DEFAULT,
    };
    t->cfg      = cfg;
    t->active      = false;
    t->max_points  = 0;
    t->last_points = 0;
    t->held_ms     = 0;
    for (int i = 0; i < 2; i++) {
        t->slot[i].present  = false;
        t->slot[i].start_x = t->slot[i].start_y = 0;
        t->slot[i].last_x = t->slot[i].last_y = 0;
        t->slot[i].moved_px = 0;
    }
}

bool tg_active(const tg_t *t)
{
    return (t != NULL) && t->active;
}

const char *tg_event_name(tg_event_t e)
{
    switch (e) {
    case TG_TAP:         return "tap";
    case TG_TAP2:        return "tap2";
    case TG_SWIPE_LEFT:  return "swipe_left";
    case TG_SWIPE_RIGHT: return "swipe_right";
    case TG_SWIPE_UP:    return "swipe_up";
    case TG_SWIPE_DOWN:  return "swipe_down";
    default:             return "none";
    }
}

static int32_t iabs32(int32_t v)
{
    return (v < 0) ? -v : v;
}

/* End a session without classifying it (shared by the release path and init). */
static void tg_init_reset_session(tg_t *t)
{
    t->active      = false;
    t->max_points  = 0;
    t->last_points = 0;
    t->held_ms     = 0;
    for (int i = 0; i < 2; i++) {
        t->slot[i].present  = false;
        t->slot[i].moved_px = 0;
    }
}

/* The session's travel, and the delta of the slot that travelled furthest (which
 * is the one whose direction a swipe should follow). */
static int32_t session_travel(const tg_t *t, int32_t *dx_out, int32_t *dy_out)
{
    int32_t best = -1;
    int32_t dx = 0, dy = 0;

    for (int i = 0; i < 2; i++) {
        if (t->slot[i].moved_px > best) {
            best = t->slot[i].moved_px;
            dx   = t->slot[i].last_x - t->slot[i].start_x;
            dy   = t->slot[i].last_y - t->slot[i].start_y;
        }
    }
    if (best < 0) {
        best = 0;
    }
    if (dx_out != NULL) {
        *dx_out = dx;
    }
    if (dy_out != NULL) {
        *dy_out = dy;
    }
    return best;
}

static tg_event_t classify(const tg_t *t)
{
    int32_t       dx = 0, dy = 0;
    const int32_t travel = session_travel(t, &dx, &dy);

    /* A brief session with almost no travel is a tap; how many fingers were
     * involved decides which one. */
    if (travel <= t->cfg.tap_max_px && t->held_ms <= t->cfg.tap_max_ms) {
        return (t->max_points >= 2) ? TG_TAP2 : TG_TAP;
    }

    /* Anything that travelled enough is a swipe — but only with one finger.
     * A two-finger drag is not a gesture the companion acts on: it is far more
     * likely to be a hand resting on the desk toy. */
    if (travel >= t->cfg.swipe_min_px && t->max_points == 1) {
        if (iabs32(dx) >= iabs32(dy)) {
            return (dx < 0) ? TG_SWIPE_LEFT : TG_SWIPE_RIGHT;
        }
        return (dy < 0) ? TG_SWIPE_UP : TG_SWIPE_DOWN;
    }

    /* Everything else — a long press, a small drag, a two-finger drag — is
     * deliberately not a gesture. */
    return TG_NONE;
}

tg_event_t tg_feed(tg_t *t, int points, int x0, int y0, int x1, int y1, uint32_t dt_ms)
{
    if (t == NULL) {
        return TG_NONE;
    }
    if (dt_ms == 0) {
        dt_ms = 1; /* never let a zero-length step stall the held-time budget */
    }
    if (points < 0) {
        points = 0;
    }
    if (points > 2) {
        points = 2; /* the controller reports two; anything else is noise */
    }

    if (points == 0) {
        if (!t->active) {
            return TG_NONE;
        }
        const tg_event_t ev = classify(t);
        tg_init_reset_session(t);
        return ev;
    }

    const int xs[2] = { x0, x1 };
    const int ys[2] = { y0, y1 };

    if (!t->active) {
        /* Session start: nothing is classified from the first sample, so it only
         * establishes the origin of whichever slots are down. */
        t->active      = true;
        t->max_points  = 0;
        t->last_points = 0;
        t->held_ms     = 0;
        for (int i = 0; i < 2; i++) {
            t->slot[i].present = false;
        }
    } else {
        t->held_ms += dt_ms;
    }

    if (points > (int)t->max_points) {
        t->max_points = (uint8_t)points;
    }

    /* The finger slots are not stable identities: a controller reports the
     * survivor of a two-finger tap in slot 0 even when it started in slot 1, so
     * the slot that "travelled" is an artefact of the count change. Re-latch the
     * origins of the slots still down whenever the count changes — the travel
     * already accumulated stays on record, so only the jump is ignored. */
    const bool count_changed = (points != (int)t->last_points);
    t->last_points = (uint8_t)points;

    for (int i = 0; i < 2; i++) {
        if (i >= points) {
            t->slot[i].present = false; /* lifted; its travel stays on record */
            continue;
        }
        if (!t->slot[i].present) {
            /* This slot just appeared: its origin is here, which is what keeps a
             * staggered two-finger lift harmless. */
            t->slot[i].present = true;
            t->slot[i].start_x = t->slot[i].last_x = xs[i];
            t->slot[i].start_y = t->slot[i].last_y = ys[i];
            t->slot[i].moved_px = 0;
            continue;
        }
        if (count_changed) {
            t->slot[i].start_x = xs[i];
            t->slot[i].start_y = ys[i];
        }
        t->slot[i].last_x = xs[i];
        t->slot[i].last_y = ys[i];

        const int32_t dx = iabs32(xs[i] - t->slot[i].start_x);
        const int32_t dy = iabs32(ys[i] - t->slot[i].start_y);
        const int32_t d  = (dx > dy) ? dx : dy; /* Chebyshev: cheap, direction-agnostic */
        if (d > t->slot[i].moved_px) {
            t->slot[i].moved_px = d;
        }
    }
    return TG_NONE;
}
