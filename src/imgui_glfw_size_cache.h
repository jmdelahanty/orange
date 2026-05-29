#ifndef ORANGE_IMGUI_GLFW_SIZE_CACHE_H
#define ORANGE_IMGUI_GLFW_SIZE_CACHE_H

#include <GLFW/glfw3.h>

#include <cstdint>

struct gx_context;

struct OrangeImguiGlfwSizeCacheStats
{
    bool cache_context_registered = false;
    uint64_t window_size_cache_hits = 0;
    uint64_t window_size_fallbacks = 0;
    uint64_t framebuffer_size_cache_hits = 0;
    uint64_t framebuffer_size_fallbacks = 0;
    uint64_t null_window_requests = 0;
};

void orange_imgui_glfw_set_size_cache_context(GLFWwindow* window, gx_context* context);
void orange_imgui_glfw_reset_size_cache_stats();
OrangeImguiGlfwSizeCacheStats orange_imgui_glfw_size_cache_stats();
void orange_imgui_glfw_get_window_size(GLFWwindow* window, int* width, int* height);
void orange_imgui_glfw_get_framebuffer_size(GLFWwindow* window, int* width, int* height);

#endif
