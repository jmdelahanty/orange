#ifndef ORANGE_IMGUI_GLFW_SIZE_CACHE_H
#define ORANGE_IMGUI_GLFW_SIZE_CACHE_H

#include <GLFW/glfw3.h>

struct gx_context;

void orange_imgui_glfw_set_size_cache_context(GLFWwindow* window, gx_context* context);
void orange_imgui_glfw_get_window_size(GLFWwindow* window, int* width, int* height);
void orange_imgui_glfw_get_framebuffer_size(GLFWwindow* window, int* width, int* height);

#endif
