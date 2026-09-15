/* Host test for main/touch_gesture.c — the recogniser behind the companion's
 * touch gestures (AGENTS.md §9a: the logic is pure so it can be exercised
 * without a finger).
 *
 * Every case is a sequence of 25 ms touch samples, the same period the firmware
 * polls the controller at, followed by the lift that ends the session. A case
 * passes when the recogniser emits exactly the expected event (or nothing at
 * all) across the whole sequence.
 */
#include <stdio.h>
#include <string.h>

#include "touch_gesture.h"

#define STEP_MS 25

typedef struct {
    int points;
    int x0, y0;
    int x1, y1;
} frame_t;

#define P1(x, y)       ((frame_t){ 1, (x), (y), 0, 0 })
#define P2(x, y, a, b) ((frame_t){ 2, (x), (y), (a), (b) })
#define LIFT           ((frame_t){ 0, 0, 0, 0, 0 })

typedef struct {
    const char *name;
    const frame_t *frames;
    size_t         count;
    tg_event_t     want;      /* every event produced must equal this */
    int            want_count;/* how many events to expect (usually 1, 0 for none) */
} case_t;

static int g_failed;

/* Runs one case and reports the events it produced. */
static void run(const case_t *c)
{
    static const int MAX_EVENTS = 8;
    tg_t      t;
    tg_event_t seen[8];
    int       events = 0;
    int       bad    = 0;

    tg_init(&t);
    for (size_t i = 0; i < c->count; i++) {
        const tg_event_t e = tg_feed(&t, c->frames[i].points, c->frames[i].x0, c->frames[i].y0,
                                     c->frames[i].x1, c->frames[i].y1, STEP_MS);
        if (e == TG_NONE) {
            continue;
        }
        if (events < MAX_EVENTS) {
            seen[events] = e;
        }
        events++;
        if (e != c->want) {
            bad++;
        }
    }

    if (events == c->want_count && bad == 0) {
        printf("PASS %s\n", c->name);
        return;
    }
    printf("FAIL %s: want %s x%d, got", c->name, tg_event_name(c->want), c->want_count);
    if (events == 0) {
        printf(" nothing");
    }
    for (int i = 0; i < events && i < MAX_EVENTS; i++) {
        printf(" %s", tg_event_name(seen[i]));
    }
    printf("\n");
    g_failed++;
}

int main(void)
{
    /* A still finger for 100 ms, then release. */
    static const frame_t tap[] = {
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), LIFT,
    };
    /* Unhurried press: still a tap at 400 ms, with a little wobble. */
    static const frame_t tap_slow[] = {
        P1(80, 120), P1(81, 121), P1(80, 119), P1(82, 120), P1(80, 120), P1(81, 120),
        P1(80, 121), P1(80, 120), P1(81, 119), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(81, 120), P1(80, 120), P1(80, 120), LIFT,
    };
    /* Pressed and held for 800 ms without travelling: deliberately not a tap. */
    static const frame_t hold[] = {
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120), P1(80, 120),
        P1(80, 120), P1(80, 120), LIFT,
    };
    /* 20 px of travel: past tap_max_px (12), short of swipe_min_px (30). */
    static const frame_t drag_short[] = {
        P1(100, 120), P1(95, 120), P1(90, 120), P1(85, 120), P1(80, 120), LIFT,
    };
    static const frame_t swipe_left[] = {
        P1(140, 120), P1(130, 120), P1(115, 120), P1(100, 120), P1(90, 120), P1(82, 120), LIFT,
    };
    static const frame_t swipe_right[] = {
        P1(40, 120), P1(60, 120), P1(75, 120), P1(95, 120), P1(110, 120), LIFT,
    };
    static const frame_t swipe_up[] = {
        P1(80, 200), P1(80, 185), P1(80, 170), P1(80, 160), P1(80, 155), LIFT,
    };
    static const frame_t swipe_down[] = {
        P1(80, 60), P1(80, 75), P1(80, 90), P1(80, 105), P1(80, 120), LIFT,
    };
    /* Mostly horizontal: the dominant axis wins. */
    static const frame_t swipe_diagonal[] = {
        P1(140, 100), P1(125, 105), P1(110, 112), P1(95, 118), P1(85, 124), LIFT,
    };
    /* Two fingers down and up together. */
    static const frame_t tap2[] = {
        P2(60, 120, 100, 120), P2(60, 120, 100, 120), P2(60, 120, 100, 120),
        P2(60, 120, 100, 120), P2(60, 120, 100, 120), P2(60, 120, 100, 120), LIFT,
    };
    /* The regression that motivated per-slot travel: one finger lifts ~100 ms
     * before the other, so the midpoint moves 20 px even though neither finger
     * travelled. Still a two-finger tap. */
    static const frame_t tap2_staggered[] = {
        P2(60, 120, 100, 120), P2(60, 120, 100, 120), P2(60, 120, 100, 120),
        P2(60, 120, 100, 120),
        P1(100, 120), P1(100, 120), P1(100, 120), P1(100, 120), LIFT,
    };
    /* A hand resting on the toy: two fingers, moving. Not a gesture. */
    static const frame_t drag2[] = {
        P2(60, 120, 100, 120), P2(70, 122, 110, 122), P2(85, 125, 125, 125),
        P2(100, 128, 140, 128), P2(115, 130, 155, 130), LIFT,
    };
    /* Two separate taps in a row both register. */
    static const frame_t taps_in_a_row[] = {
        P1(80, 120), P1(80, 120), P1(80, 120), LIFT,
        P1(90, 140), P1(90, 140), P1(90, 140), LIFT,
    };
    /* A finger arriving after a first one is already down still counts as two. */
    static const frame_t finger_joins[] = {
        P1(60, 120), P1(60, 120),
        P2(60, 120, 100, 120), P2(60, 120, 100, 120), P2(60, 120, 100, 120), LIFT,
    };
    /* Press, hold, then travel: the travel decides, so this is a swipe. */
    static const frame_t hold_then_swipe[] = {
        P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120),
        P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120), P1(140, 120),
        P1(130, 120), P1(115, 120), P1(100, 120), P1(90, 120), P1(82, 120), LIFT,
    };
    /* Samples with no finger at all never produce anything. */
    static const frame_t idle[] = { LIFT, LIFT, LIFT };

    static const case_t cases[] = {
        { .name = "tap", .frames = tap, .want = TG_TAP, .want_count = 1,
          .count = sizeof tap / sizeof tap[0] },
        { .name = "tap_slow", .frames = tap_slow, .want = TG_TAP, .want_count = 1,
          .count = sizeof tap_slow / sizeof tap_slow[0] },
        { .name = "hold_is_not_tap", .frames = hold, .want = TG_NONE, .want_count = 0,
          .count = sizeof hold / sizeof hold[0] },
        { .name = "short_drag_is_none", .frames = drag_short, .want = TG_NONE, .want_count = 0,
          .count = sizeof drag_short / sizeof drag_short[0] },
        { .name = "swipe_left", .frames = swipe_left, .want = TG_SWIPE_LEFT, .want_count = 1,
          .count = sizeof swipe_left / sizeof swipe_left[0] },
        { .name = "swipe_right", .frames = swipe_right, .want = TG_SWIPE_RIGHT, .want_count = 1,
          .count = sizeof swipe_right / sizeof swipe_right[0] },
        { .name = "swipe_up", .frames = swipe_up, .want = TG_SWIPE_UP, .want_count = 1,
          .count = sizeof swipe_up / sizeof swipe_up[0] },
        { .name = "swipe_down", .frames = swipe_down, .want = TG_SWIPE_DOWN, .want_count = 1,
          .count = sizeof swipe_down / sizeof swipe_down[0] },
        { .name = "swipe_diagonal", .frames = swipe_diagonal, .want = TG_SWIPE_LEFT, .want_count = 1,
          .count = sizeof swipe_diagonal / sizeof swipe_diagonal[0] },
        { .name = "tap2", .frames = tap2, .want = TG_TAP2, .want_count = 1,
          .count = sizeof tap2 / sizeof tap2[0] },
        { .name = "tap2_staggered", .frames = tap2_staggered, .want = TG_TAP2, .want_count = 1,
          .count = sizeof tap2_staggered / sizeof tap2_staggered[0] },
        { .name = "two_finger_drag", .frames = drag2, .want = TG_NONE, .want_count = 0,
          .count = sizeof drag2 / sizeof drag2[0] },
        { .name = "two_taps_in_a_row", .frames = taps_in_a_row, .want = TG_TAP, .want_count = 2,
          .count = sizeof taps_in_a_row / sizeof taps_in_a_row[0] },
        { .name = "finger_joins", .frames = finger_joins, .want = TG_TAP2, .want_count = 1,
          .count = sizeof finger_joins / sizeof finger_joins[0] },
        { .name = "hold_then_swipe", .frames = hold_then_swipe, .want = TG_SWIPE_LEFT, .want_count = 1,
          .count = sizeof hold_then_swipe / sizeof hold_then_swipe[0] },
        { .name = "idle", .frames = idle, .want = TG_NONE, .want_count = 0,
          .count = sizeof idle / sizeof idle[0] },
    };

    const size_t n = sizeof cases / sizeof cases[0];
    for (size_t i = 0; i < n; i++) {
        run(&cases[i]);
    }
    printf("%zu cases, %d failed\n", n, g_failed);
    return g_failed > 0 ? 1 : 0;
}
