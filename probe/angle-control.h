#pragma once
#include <stdint.h>

typedef struct AngleControlResult {
    uint32_t stage;
    uint32_t egl_error;
    uint32_t gl_error;
    uint32_t egl_major;
    uint32_t egl_minor;
    uint8_t pixel[4];
} AngleControlResult;
