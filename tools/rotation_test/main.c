/* Host test for main/rotation_logic.c — the device source, compiled
 * unmodified (AGENTS.md §9a).
 *
 * Every sample is fed as a 25 ms step, the cadence of the `imu_rot` task in
 * main/ui_rotation.c (IMU_TASK_PERIOD_MS), so the integrator arithmetic is
 * exercised exactly as it is on the device.
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "rotation_logic.h"

#define DT_MS 25 /* main/ui_rotation.c: IMU_TASK_PERIOD_MS */

static int g_cases;
static int g_failed;

static void check(const char *name, bool ok, const char *detail, ...)
{
    g_cases++;
    if (ok) {
        printf("PASS %s\n", name);
        return;
    }

    g_failed++;
    printf("FAIL %s ", name);
    va_list ap;
    va_start(ap, detail);
    vprintf(detail, ap);
    va_end(ap);
    printf("\n");
}

/* ---- synthetic samples --------------------------------------------------- */

/* Resting attitudes: ~1 g on the axis in question and a little on the others,
 * because a board on a desk is never perfectly level. */
static const float REST[6][3] = {
    {  0.998f,  0.05f, -0.02f }, /* +x */
    { -0.998f,  0.05f,  0.02f }, /* -x */
    {  0.05f,  0.998f, -0.02f }, /* +y */
    { -0.05f, -0.998f,  0.02f }, /* -y */
    {  0.05f, -0.02f,  0.998f }, /* +z */
    { -0.05f,  0.02f, -0.998f }, /* -z */
};
static const int REST_AXIS[6] = { 0, 0, 1, 1, 2, 2 };
static const int REST_SIGN[6] = { 1, -1, 1, -1, 1, -1 };

static const float STILL[3] = { 0.0f, 0.0f, 0.0f };

/* Screen (+z) up, the expected boot attitude. */
static const float REST_ZUP[3] = { 0.05f, -0.02f, 0.998f };

typedef struct {
    int commits; /* non-zero returns so far */
    int last;    /* the most recent one */
    int sum;     /* their sum, i.e. the net committed rotation */
} tally_t;

static void feed(rot_detector_t *d, const float a[3], const float w[3], int n, tally_t *t)
{
    for (int i = 0; i < n; i++) {
        const int delta = rot_detector_feed(d, a[0], a[1], a[2], w[0], w[1], w[2], DT_MS);
        if (delta != 0 && t != NULL) {
            t->commits++;
            t->last = delta;
            t->sum += delta;
        }
    }
}

/* Gyro vector for a turn of `dps` about `axis`; the attitude does not change
 * while turning about the normal. */
static void turn_rate_vec(float w[3], int axis, float dps)
{
    w[0] = w[1] = w[2] = 0.0f;
    w[axis] = dps;
}

/* Steps needed to turn `deg` at `dps` (integer here, so the integral lands
 * exactly on the intended angle). */
static int steps(int deg, float dps)
{
    return (int)lround((double)deg * 1000.0 / ((double)dps * DT_MS));
}

/* Rest until the screen-up attitude has been calibrated. */
static void calibrate(rot_detector_t *d)
{
    feed(d, REST_ZUP, STILL, 20, NULL);
}

int main(void)
{
    rot_detector_t d;
    tally_t        t;
    float          w[3];

    /* (a) calibration: the normal axis and its sign come out of the resting
     * attitude alone, at every axis/sign combination. */
    for (int i = 0; i < 6; i++) {
        rot_detector_init(&d);
        feed(&d, REST[i], STILL, 19, NULL);
        const bool early = rot_detector_calibrated(&d);
        feed(&d, REST[i], STILL, 1, NULL);

        char name[24];
        snprintf(name, sizeof name, "calibrate_%c%c", "xyz"[REST_AXIS[i]], REST_SIGN[i] > 0 ? '+' : '-');

        const bool ok = !early && rot_detector_calibrated(&d) &&
                        rot_detector_normal_axis(&d) == REST_AXIS[i] &&
                        rot_detector_normal_sign(&d) == REST_SIGN[i];
        check(name, ok, "calibrated after 19 samples=%d, now=%d, axis=%d (want %d), sign=%d (want %d)",
              (int)early, (int)rot_detector_calibrated(&d), rot_detector_normal_axis(&d),
              REST_AXIS[i], rot_detector_normal_sign(&d), REST_SIGN[i]);
    }

    /* (b) a 90 deg counter-clockwise turn at 150 dps: one commit, -90.
     * Accel +z => the calibrated normal is +z with sign +1, and a positive gyro
     * z is a CCW turn of the device, which must be applied as -90 so the content
     * stays upright. `steps(90, 150)` = 24 samples = 600 ms = exactly 90 deg. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    const int during = t.commits;
    feed(&d, REST_ZUP, STILL, 7, &t); /* 175 ms > the 150 ms settle timer */
    check("turn_90_ccw", during == 0 && t.commits == 1 && t.last == -90,
          "committed %d during the turn, %d after settling %d ms, last %d (want 0, 1, -90)",
          during, t.commits, 7 * DT_MS, t.last);

    feed(&d, REST_ZUP, STILL, 200, &t); /* 5 s of no motion */
    check("turn_90_ccw_stays", t.commits == 1,
          "%d commits without further motion (want 1)", t.commits);

    /* The mirrored direction: clockwise on the desk commits +90. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, -150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    feed(&d, REST_ZUP, STILL, 7, &t);
    check("turn_90_cw", t.commits == 1 && t.last == 90,
          "%d commits, last %d (want 1, 90)", t.commits, t.last);

    /* (c) slow drift: 5 dps for 30 s is below the 20 dps activity threshold and
     * never accumulates. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 5.0f);
    feed(&d, REST_ZUP, w, 30 * 1000 / DT_MS, &t); /* 1200 samples = 30 s */
    check("drift", t.commits == 0 && rot_detector_calibrated(&d),
          "%d commits, calibrated=%d (want 0, 1)", t.commits, (int)rot_detector_calibrated(&d));

    /* (d) a 180 deg turn: two quarter turns, each committed once the hand stops
     * (the pause is what separates them; see the clamp case below). */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    feed(&d, REST_ZUP, STILL, 40, &t); /* pause: settle + lockout + margin */
    const int half = t.commits;
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    feed(&d, REST_ZUP, STILL, 40, &t);
    check("turn_180", half == 1 && t.commits == 2 && t.sum == -180,
          "%d commits after the first half, %d after both, net %d (want 1, 2, -180)",
          half, t.commits, t.sum);

    /* (e) jitter: the hands resting on the device shake it +-10 dps, below the
     * 20 dps threshold, so nothing accumulates. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    for (int i = 0; i < 400; i++) { /* 10 s */
        turn_rate_vec(w, 2, (i % 2 == 0) ? 10.0f : -10.0f);
        feed(&d, REST_ZUP, w, 1, &t);
    }
    check("jitter", t.commits == 0, "%d commits (want 0)", t.commits);

    /* (f) the 400 ms lockout swallows a second turn that starts immediately
     * after a commit, and expires afterwards. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    feed(&d, REST_ZUP, STILL, 7, &t);
    const int after_first = t.commits;
    turn_rate_vec(w, 2, 600.0f);
    feed(&d, REST_ZUP, w, steps(90, 600.0f), &t); /* 150 ms = another 90 deg, inside the lockout */
    feed(&d, REST_ZUP, STILL, 40, &t);
    const int suppressed = t.commits;
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t); /* now outside the lockout */
    feed(&d, REST_ZUP, STILL, 7, &t);
    check("lockout", after_first == 1 && suppressed == 1 && t.commits == 2 && t.last == -90,
          "%d commits, %d after a turn inside the lockout window, %d after a later turn (want 1, 1, 2)",
          after_first, suppressed, t.commits);

    /* (g) nothing commits before the normal axis is known: while the gyro is
     * moving the samples are not calibration material either. */
    rot_detector_init(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, 100, &t); /* 2.5 s of turning, never at rest */
    const bool calibrated = rot_detector_calibrated(&d);
    const int  before     = t.commits;
    feed(&d, REST_ZUP, STILL, 20, &t); /* put it down: now it can calibrate */
    const bool now = rot_detector_calibrated(&d);
    turn_rate_vec(w, 2, 150.0f);
    feed(&d, REST_ZUP, w, steps(90, 150.0f), &t);
    feed(&d, REST_ZUP, STILL, 7, &t);
    check("uncalibrated", !calibrated && before == 0 && now && t.commits == 1 && t.last == -90,
          "calibrated while moving=%d, %d commits before calibration, calibrated at rest=%d, %d commits after (want 0, 0, 1, 1)",
          (int)calibrated, before, (int)now, t.commits);

    /* A wild spin cannot bank several commits: 540 deg without stopping is one
     * gesture, clamped to 180 deg of accumulation, so it commits once. */
    rot_detector_init(&d);
    calibrate(&d);
    t = (tally_t){ 0 };
    turn_rate_vec(w, 2, 300.0f);
    feed(&d, REST_ZUP, w, steps(540, 300.0f), &t); /* 1.8 s = 540 deg */
    const int spinning = t.commits;
    feed(&d, REST_ZUP, STILL, 7, &t);
    check("spin_clamp", spinning == 0 && t.commits == 1 && t.last == -90,
          "%d commits during the spin, %d after (want 0, 1), last %d", spinning, t.commits, t.last);

    printf("%d cases, %d failed\n", g_cases, g_failed);
    return g_failed == 0 ? 0 : 1;
}
