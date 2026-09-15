#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the companion is showing. Swiped left/right between the two. */
typedef enum {
    UI_VIEW_MOOD = 0, /* the face plus the agent list (the default view) */
    UI_VIEW_STATS,    /* session statistics for the agents */
    UI_VIEW_COUNT,
} ui_view_t;

/* Build the UI on the active screen; call with the LVGL port lock held.
 * The layout follows the screen it is given, so this works at 172x320 and at
 * 320x172 (main.c switches the resolution when the device is turned). */
void ui_companion_create(void);

/* ---- input handlers --------------------------------------------------------
 * The screen's LVGL event callbacks call these (click, long press, gesture), and
 * the host harness calls them directly — the panel reports one touch, so LVGL's
 * own event model is the whole input path. */

/* Single tap: refresh from the bridge and play the mood's flourish. */
void ui_companion_on_tap(void);

/* Two-finger tap: show or hide the diagnostics overlay. */
void ui_companion_on_toggle_overlay(void);

/* Horizontal one-finger swipe: -1 for left, +1 for right, wrapping around. */
void ui_companion_on_switch_view(int dir);

/* Vertical one-finger swipe: -1 up, +1 down. Pages the agent list when the
 * bridge reports more agents than the layout has rows; ignoring it otherwise. */
void ui_companion_on_page(int dir);

/* Which view is showing, for logs and tests. */
ui_view_t ui_companion_view(void);
const char *ui_view_name(ui_view_t view);

#ifdef __cplusplus
}
#endif
