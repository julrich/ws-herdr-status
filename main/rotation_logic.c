/* Turn detector. See rotation_logic.h for the idea and the API. */

#include "rotation_logic.h"

#include <math.h>
#include <string.h>

/* ---- thresholds ---------------------------------------------------------- */

/* 20 accepted samples at the IMU task's 25 ms period = 0.5 s of rest, which the
 * device satisfies while it sits on the desk — the expected boot attitude. */
#define ROT_CAL_SAMPLES 20
/* |a| ≈ 1 g: rejects free fall, a shove, and the moment the device is picked
 * up. Outside this band a sample says nothing about how the board is mounted. */
#define ROT_CAL_MIN_G 0.7f
#define ROT_CAL_MAX_G 1.3f
/* ...and the gyro must be quiet, otherwise the "resting" attitude is a moving
 * one and the averaged axis is meaningless. */
#define ROT_CAL_MAX_DPS 40.0f
/* A flat-lying device puts nearly the whole 1 g on the normal axis, so 0.8 g
 * rejects a board standing on an edge, where the mapping is ambiguous. */
#define ROT_CAL_MIN_NORMAL_G 0.8f

/* Below this the motion is hand tremor or sensor noise; a deliberate turn is an
 * order of magnitude faster. */
#define ROT_ACTIVE_DPS 20.0f
/* The hand pauses for a moment between the two halves of a 180 deg turn. 150 ms
 * is longer than a sample gap and shorter than a deliberate pause, so it
 * separates gestures without splitting one of them. */
#define ROT_SETTLE_MS 150
/* 55 deg of a 90 deg gesture: enough that a nudge or a knock cannot trip it,
 * early enough that the content lands before the hand has quite stopped. */
#define ROT_COMMIT_DEG 55.0f
#define ROT_QUARTER_DEG 90
/* After a commit the hand is still finishing the turn; suppressing the next
 * 400 ms keeps one gesture from committing twice. */
#define ROT_LOCKOUT_MS 400
/* A wild spin must not bank several commits: one gesture can accumulate at most
 * half a turn's worth. */
#define ROT_YAW_CLAMP 180.0f

/* ---- calibration --------------------------------------------------------- */

/* Works out which accelerometer axis carries the 1 g of gravity and which way
 * it faces, from samples taken while the device is at rest. A device lying flat
 * on a desk has ~1 g on the screen normal, so the largest |mean| is that normal
 * and the mean's sign says whether it points out of the screen.
 *
 * This is what makes the axis mapping board-independent: no per-board constants
 * are needed, and the board may even be mounted upside down. */
static void rot_calibrate(rot_detector_t *d, float ax, float ay, float az,
                          float gx, float gy, float gz)
{
    const float mag_a = sqrtf(ax * ax + ay * ay + az * az);
    const float mag_g = sqrtf(gx * gx + gy * gy + gz * gz);
    if (mag_a < ROT_CAL_MIN_G || mag_a > ROT_CAL_MAX_G || mag_g > ROT_CAL_MAX_DPS) {
        /* Moving: skip the sample instead of folding it into the average. */
        return;
    }

    d->sum[0] += ax;
    d->sum[1] += ay;
    d->sum[2] += az;
    if (++d->n < ROT_CAL_SAMPLES) {
        return;
    }

    int best = 0;
    for (int i = 1; i < 3; i++) {
        if (fabsf(d->sum[i]) > fabsf(d->sum[best])) {
            best = i;
        }
    }
    const float mean = d->sum[best] / (float)d->n;
    if (fabsf(mean) < ROT_CAL_MIN_NORMAL_G) {
        /* Resting on an edge or held at a steep tilt: no axis is the screen
         * normal, so this average is worthless. Start over rather than latch a
         * wrong axis — the device will be flat on the desk soon enough. */
        d->n = 0;
        d->sum[0] = d->sum[1] = d->sum[2] = 0.0f;
        return;
    }

    d->axis = (int8_t)best;
    d->sign = (mean > 0.0f) ? 1 : -1;
    d->calibrated = true;
}

/* ---- API ----------------------------------------------------------------- */

void rot_detector_init(rot_detector_t *d)
{
    memset(d, 0, sizeof *d);
    d->sign = 1; /* what the accessors report before calibration */
}

bool rot_detector_calibrated(const rot_detector_t *d)
{
    return d->calibrated;
}

int rot_detector_normal_axis(const rot_detector_t *d)
{
    return d->axis;
}

int rot_detector_normal_sign(const rot_detector_t *d)
{
    return d->sign;
}

int rot_detector_feed(rot_detector_t *d, float ax, float ay, float az,
                      float gx, float gy, float gz, uint32_t dt_ms)
{
    if (d->lockout_ms > 0) {
        /* A commit has just happened and the device is still moving. Dropping
         * the sample outright (rather than integrating and discarding) keeps the
         * hand's follow-through from priming the next commit as well. */
        d->lockout_ms = (d->lockout_ms > dt_ms) ? d->lockout_ms - dt_ms : 0;
        d->quiet_ms = 0;
        return 0;
    }

    if (!d->calibrated) {
        rot_calibrate(d, ax, ay, az, gx, gy, gz);
        return 0;
    }

    const float comp = (d->axis == 0) ? gx : (d->axis == 1) ? gy : gz;
    /* The gyro reads positive for a counter-clockwise turn of the device seen
     * from outside the screen (right-handed axes: +z out of the panel), so a
     * positive `yaw` means the device was turned CCW. `sign` folds in which way
     * the panel faces up, i.e. whether the calibrated axis points out of the
     * screen or into it. */
    const float turn_rate = (float)d->sign * comp;

    if (fabsf(turn_rate) >= ROT_ACTIVE_DPS) {
        d->quiet_ms = 0;
        d->yaw += turn_rate * (float)dt_ms / 1000.0f;
        if (d->yaw > ROT_YAW_CLAMP) {
            d->yaw = ROT_YAW_CLAMP;
        } else if (d->yaw < -ROT_YAW_CLAMP) {
            d->yaw = -ROT_YAW_CLAMP;
        }
        return 0;
    }

    /* Quiet. Only once the motion has really stopped is the accumulated angle
     * worth looking at: mid-turn readings are still updating. */
    d->quiet_ms += dt_ms;
    if (d->quiet_ms <= ROT_SETTLE_MS) {
        return 0;
    }
    d->quiet_ms = 0;

    if (fabsf(d->yaw) < ROT_COMMIT_DEG) {
        /* Too little for a quarter turn — a nudge, a knock, or a tap. */
        d->yaw = 0.0f;
        return 0;
    }

    const float turned = d->yaw;
    d->yaw = 0.0f;
    d->lockout_ms = ROT_LOCKOUT_MS;

    /* A CCW turn of the device must rotate the content clockwise on the panel
     * (i.e. -90) so it stays upright for whoever is looking at it. This one
     * negation is the single place to flip if the first real turn on hardware
     * goes the wrong way. */
    return (turned > 0.0f) ? -ROT_QUARTER_DEG : ROT_QUARTER_DEG;
}
