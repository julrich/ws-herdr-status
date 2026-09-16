/* The mood face, wrapped.
 *
 * The face itself is 0015/lvgl_kawaii_face (MIT), vendored in components/: it draws
 * on LVGL canvases and knows seventeen expressions. Its header pulls in esp_err.h,
 * and this project keeps ui_companion.c free of esp_* includes so the host harness
 * can compile it unmodified (AGENTS.md §9a) — so the component is reached through
 * this file, which knows the expressions by name and nothing else.
 *
 * The host harness compiles this and the component for real (tools/ui_host_test
 * carries shims for the ESP headers it touches), so the face it renders is the face
 * the panel shows.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Only the expressions this project uses; the component has more. */
typedef enum {
    MOOD_FACE_NEUTRAL = 0,
    MOOD_FACE_WORRIED,   /* BLOCKED resting: nervous, needs you */
    MOOD_FACE_WORKING,   /* WORKING resting: gritted teeth, sweating */
    MOOD_FACE_HAPPY,     /* DONE resting */
    MOOD_FACE_SLEEPY,    /* SLEEP resting */
    MOOD_FACE_SAD,       /* OFFLINE resting */
    MOOD_FACE_SURPRISED, /* BLOCKED reaction */
    MOOD_FACE_COOL,      /* WORKING reaction */
    MOOD_FACE_EXCITED,   /* DONE reaction */
    MOOD_FACE_SMIRK,     /* IDLE reaction */
    MOOD_FACE_CONFUSED,  /* OFFLINE reaction */
    MOOD_FACE_WINK,      /* a tap's acknowledgement */
    MOOD_FACE_COUNT
} mood_face_t;

/* Build the face inside `parent`, which the face fills: size and position are the
 * parent's. Safe to call again on a rebuilt screen. */
void mood_face_create(lv_obj_t *parent);

/* Switch expression. `smooth` asks the component to transition into it. */
void mood_face_set(mood_face_t face, bool smooth);

/* Drop the face's timers; the next mood_face_create() builds a new one. */
void mood_face_destroy(void);

#ifdef __cplusplus
}
#endif
