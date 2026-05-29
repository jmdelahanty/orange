#ifndef GX_CONTEXT_H
#define GX_CONTEXT_H

#include <GLFW/glfw3.h>

#include "types.h"

typedef struct gx_context
{
    u32 swap_interval;
    u32 frame_max_fps;
    u32 width;
    u32 height;
    int window_width;
    int window_height;
    int framebuffer_width;
    int framebuffer_height;
    GLFWwindow *render_target;
    char *render_target_title;
    char *glsl_version;
} gx_context;

#endif
