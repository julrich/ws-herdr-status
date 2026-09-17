/* The few platform numbers the stats view's device page shows that LVGL cannot
 * answer for us. Kept in its own translation unit so main/ui_companion.c stays
 * free of esp_* includes and can be compiled on the host (AGENTS.md §9a).
 */

#include "ui_stats.h"

#include "esp_system.h"

uint32_t ui_device_free_heap(void)
{
    return (uint32_t)esp_get_free_heap_size();
}
