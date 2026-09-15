/* Turn detector: decides when the device has been turned a quarter turn.
 *
 * Pure C by design — no esp_* and no LVGL headers — so tools/rotation_test
 * compiles this file unmodified on the host (AGENTS.md §9a).
 *
 * A device lying on a desk is turned by spinning it about the screen's normal
 * axis. Gravity cannot see that motion; the gyroscope can. The detector
 * therefore calibrates, from the resting attitude, which accelerometer axis is
 * the screen normal and which way it faces, then integrates the gyro component
 * along that axis. Nothing here is board-specific: the same code works for a
 * panel mounted either side up, at any of the six axis/sign combinations.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Calibration: running sums of the accepted resting samples. */
    float    sum[3];
    uint16_t n;
    bool     calibrated;
    int8_t   axis; /* 0 = x, 1 = y, 2 = z; the screen normal */
    int8_t   sign; /* +1 when +axis points out of the screen */

    /* Turn integration. */
    float    yaw;        /* deg accumulated during the current gesture */
    uint32_t quiet_ms;   /* time since |turn_rate| last passed the threshold */
    uint32_t lockout_ms; /* time left during which a commit is suppressed */
} rot_detector_t;

/* Zeroes the detector: uncalibrated, no accumulated turn. */
void rot_detector_init(rot_detector_t *d);

/* Feed one sample: acceleration in g and turn rate in deg/s in the chip's own
 * axes, plus the time since the previous sample. Returns 0 normally, or the
 * quarter turn to apply to the UI: +90 or -90 (see the sign note in the .c). */
int rot_detector_feed(rot_detector_t *d, float ax, float ay, float az,
                      float gx, float gy, float gz, uint32_t dt_ms);

/* True once the normal axis has been determined from a resting device. */
bool rot_detector_calibrated(const rot_detector_t *d);

/* The calibrated screen normal: which accelerometer axis, and its sign. Only
 * meaningful after rot_detector_calibrated(); before that they read x / +1. */
int rot_detector_normal_axis(const rot_detector_t *d);
int rot_detector_normal_sign(const rot_detector_t *d);
