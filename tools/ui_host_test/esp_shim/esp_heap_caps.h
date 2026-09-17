#pragma once

#include <stdlib.h>

#define MALLOC_CAP_INTERNAL (1 << 0)
#define MALLOC_CAP_8BIT     (1 << 1)
#define MALLOC_CAP_DMA      (1 << 2)
#define MALLOC_CAP_SPIRAM   (1 << 3)

static inline void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    return malloc(size);
}
