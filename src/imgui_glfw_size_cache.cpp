#include "imgui_glfw_size_cache.h"

#include "gx_context.h"

static GLFWwindow* g_orange_imgui_size_cache_window = nullptr;
static gx_context* g_orange_imgui_size_cache_context = nullptr;

void orange_imgui_glfw_set_size_cache_context(GLFWwindow* window, gx_context* context)
{
    g_orange_imgui_size_cache_window = window;
    g_orange_imgui_size_cache_context = context;
}

static gx_context* orange_gx_context_for_window(GLFWwindow* window)
{
    gx_context* context = g_orange_imgui_size_cache_context;
    if (!window || window != g_orange_imgui_size_cache_window ||
        !context || context->render_target != window) {
        return nullptr;
    }
    return context;
}

void orange_imgui_glfw_get_window_size(GLFWwindow* window, int* width, int* height)
{
    if (!window) {
        if (width) {
            *width = 0;
        }
        if (height) {
            *height = 0;
        }
        return;
    }

    if (gx_context* context = orange_gx_context_for_window(window)) {
        if (width) {
            *width = context->window_width > 0
                ? context->window_width
                : static_cast<int>(context->width);
        }
        if (height) {
            *height = context->window_height > 0
                ? context->window_height
                : static_cast<int>(context->height);
        }
        return;
    }

    glfwGetWindowSize(window, width, height);
}

void orange_imgui_glfw_get_framebuffer_size(GLFWwindow* window, int* width, int* height)
{
    if (!window) {
        if (width) {
            *width = 0;
        }
        if (height) {
            *height = 0;
        }
        return;
    }

    if (gx_context* context = orange_gx_context_for_window(window)) {
        if (width) {
            *width = context->framebuffer_width > 0
                ? context->framebuffer_width
                : static_cast<int>(context->width);
        }
        if (height) {
            *height = context->framebuffer_height > 0
                ? context->framebuffer_height
                : static_cast<int>(context->height);
        }
        return;
    }

    glfwGetFramebufferSize(window, width, height);
}
