#include "imgui_glfw_size_cache.h"

#include "gx_context.h"

#include <atomic>

static GLFWwindow* g_orange_imgui_size_cache_window = nullptr;
static gx_context* g_orange_imgui_size_cache_context = nullptr;
static std::atomic<uint64_t> g_window_size_cache_hits{0};
static std::atomic<uint64_t> g_window_size_fallbacks{0};
static std::atomic<uint64_t> g_framebuffer_size_cache_hits{0};
static std::atomic<uint64_t> g_framebuffer_size_fallbacks{0};
static std::atomic<uint64_t> g_null_window_requests{0};

void orange_imgui_glfw_set_size_cache_context(GLFWwindow* window, gx_context* context)
{
    g_orange_imgui_size_cache_window = window;
    g_orange_imgui_size_cache_context = context;
}

void orange_imgui_glfw_reset_size_cache_stats()
{
    g_window_size_cache_hits.store(0, std::memory_order_relaxed);
    g_window_size_fallbacks.store(0, std::memory_order_relaxed);
    g_framebuffer_size_cache_hits.store(0, std::memory_order_relaxed);
    g_framebuffer_size_fallbacks.store(0, std::memory_order_relaxed);
    g_null_window_requests.store(0, std::memory_order_relaxed);
}

OrangeImguiGlfwSizeCacheStats orange_imgui_glfw_size_cache_stats()
{
    return {
        g_orange_imgui_size_cache_context != nullptr &&
            g_orange_imgui_size_cache_window != nullptr,
        g_window_size_cache_hits.load(std::memory_order_relaxed),
        g_window_size_fallbacks.load(std::memory_order_relaxed),
        g_framebuffer_size_cache_hits.load(std::memory_order_relaxed),
        g_framebuffer_size_fallbacks.load(std::memory_order_relaxed),
        g_null_window_requests.load(std::memory_order_relaxed)
    };
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
        g_null_window_requests.fetch_add(1, std::memory_order_relaxed);
        if (width) {
            *width = 0;
        }
        if (height) {
            *height = 0;
        }
        return;
    }

    if (gx_context* context = orange_gx_context_for_window(window)) {
        g_window_size_cache_hits.fetch_add(1, std::memory_order_relaxed);
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

    g_window_size_fallbacks.fetch_add(1, std::memory_order_relaxed);
    glfwGetWindowSize(window, width, height);
}

void orange_imgui_glfw_get_framebuffer_size(GLFWwindow* window, int* width, int* height)
{
    if (!window) {
        g_null_window_requests.fetch_add(1, std::memory_order_relaxed);
        if (width) {
            *width = 0;
        }
        if (height) {
            *height = 0;
        }
        return;
    }

    if (gx_context* context = orange_gx_context_for_window(window)) {
        g_framebuffer_size_cache_hits.fetch_add(1, std::memory_order_relaxed);
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

    g_framebuffer_size_fallbacks.fetch_add(1, std::memory_order_relaxed);
    glfwGetFramebufferSize(window, width, height);
}
