#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the blob companion on the active screen.
 * Must be called while the LVGL port lock is held. */
void ui_companion_create(void);

#ifdef __cplusplus
}
#endif