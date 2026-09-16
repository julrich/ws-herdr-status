/* Stand-ins for the ESP headers components/lvgl_kawaii_face reaches for, so the host
 * harness can compile the *real* component and render the *real* face (AGENTS.md
 * §9a). Only what that component touches is defined here — nothing else should grow.
 *
 * esp_lvgl_port.h is deliberately NOT shimmed: the component guards it with
 * __has_include, and without it the face takes its lock-free path, which is what a
 * host harness wants. */
#pragma once

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL (-1)
