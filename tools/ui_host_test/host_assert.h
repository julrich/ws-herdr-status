/* Host-side LVGL assert handler.
 *
 * Pulled in by lv_conf.h through LV_ASSERT_HANDLER_INCLUDE. LVGL's stock
 * handler is `while(1);`, which on this harness would hang the process with no
 * clue; aborting loudly with the raise site is what the harness needs.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

/* lv_conf_internal.h seeds LV_ASSERT_HANDLER with its own default before this
 * header is reached, so drop it first. */
#ifdef LV_ASSERT_HANDLER
#undef LV_ASSERT_HANDLER
#endif

/* Note: LVGL pastes this into a brace block right after another statement, so
 * it has to be a plain statement sequence ending in ';' (as its own
 * `while(1);` default is) -- wrapping it in do/while(0) breaks LV_ASSERT(). */
#define LV_ASSERT_HANDLER                                                     \
    fprintf(stderr, "LVGL assert %s:%d\n", __FILE__, __LINE__);               \
    abort();
