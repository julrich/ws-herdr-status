/* The one place that knows 0015/lvgl_kawaii_face exists. See mood_face.h.
 *
 * It is kept deliberately thin: the component owns the drawing, the animation timer
 * and the expression table; this only maps our names onto its enum and forwards.
 */
#include "mood_face.h"

#include "lvgl_kawaii_face.h"

static bool s_created;

/* The component animates on its own timer and draws into canvases that are children
 * of the panel we hand it. When that panel dies — a screen rebuild on a rotation, or
 * the host harness starting the next scenario — the canvases go with it, and a timer
 * that outlives them touches freed objects. So the face is torn down from the
 * parent's own DELETE event, while the canvases are still there. */
static void parent_deleted_cb(lv_event_t *e)
{
    (void)e;
    mood_face_destroy();
}

void mood_face_create(lv_obj_t *parent)
{
    if (s_created) {
        mood_face_destroy();
    }

    lv_obj_add_event_cb(parent, parent_deleted_cb, LV_EVENT_DELETE, NULL);

    const face_config_t cfg = {
        .parent          = parent,
        .animation_speed = 30,    /* ~33 fps, matching the panel's refresh */
        .blink_interval  = 3000,
        .auto_blink      = true,
    };

    if (face_animation_init((face_config_t *)&cfg) == ESP_OK) {
        s_created = true;
    }
}

void mood_face_set(mood_face_t face, bool smooth)
{
    static const face_emotion_t map[MOOD_FACE_COUNT] = {
        [MOOD_FACE_NEUTRAL]    = FACE_NEUTRAL,
        [MOOD_FACE_WORRIED]    = FACE_WORRIED,
        [MOOD_FACE_WORKING]    = FACE_WORKING_HARD,
        [MOOD_FACE_HAPPY]      = FACE_HAPPY,
        [MOOD_FACE_SLEEPY]     = FACE_SLEEPY,
        [MOOD_FACE_SAD]        = FACE_SAD,
        [MOOD_FACE_SURPRISED]  = FACE_SURPRISED,
        [MOOD_FACE_COOL]       = FACE_COOL,
        [MOOD_FACE_EXCITED]    = FACE_EXCITED,
        [MOOD_FACE_SMIRK]      = FACE_SMIRK,
        [MOOD_FACE_CONFUSED]   = FACE_CONFUSED,
        [MOOD_FACE_WINK]       = FACE_WINK,
    };

    if (!s_created || face < 0 || face >= MOOD_FACE_COUNT) {
        return;
    }

    face_set_emotion(map[face], smooth);
}

void mood_face_destroy(void)
{
    if (!s_created) {
        return;
    }

    face_animation_deinit();
    s_created = false;
}
