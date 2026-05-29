#!/usr/bin/env python3
"""Static guards for the Orange ImGui GLFW size-cache wiring."""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, function_name: str) -> str:
    match = re.search(rf"\b{re.escape(function_name)}\s*\([^)]*\)\s*\{{", source)
    require(match is not None, f"{function_name} definition not found")
    start = match.end()
    depth = 1
    index = start
    while index < len(source):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index]
        index += 1
    raise AssertionError(f"{function_name} body did not terminate")


def test_override_header_maps_glfw_size_queries() -> None:
    header = read("src/imgui_glfw_size_cache_override.h")
    require(
        "#define glfwGetWindowSize orange_imgui_glfw_get_window_size" in header,
        "override header must map glfwGetWindowSize to Orange cache shim",
    )
    require(
        "#define glfwGetFramebufferSize orange_imgui_glfw_get_framebuffer_size" in header,
        "override header must map glfwGetFramebufferSize to Orange cache shim",
    )


def test_cmake_force_includes_override_for_imgui_glfw_backend() -> None:
    cmake = read("CMakeLists.txt")
    source_property = re.search(
        r"set_property\(\s*SOURCE\s+\"\$\{DIR_IMGUI_BACKEND\}/imgui_impl_glfw\.cpp\""
        r"(?P<body>.*?)\)",
        cmake,
        flags=re.DOTALL,
    )
    require(source_property is not None, "imgui_impl_glfw.cpp source property not found")
    body = source_property.group("body")
    require("COMPILE_OPTIONS" in body, "imgui_impl_glfw.cpp must have compile options")
    require('"-include"' in body, "imgui_impl_glfw.cpp must be compiled with -include")
    require(
        '"${SRC_DIR}/imgui_glfw_size_cache_override.h"' in body,
        "imgui_impl_glfw.cpp must force-include the size-cache override header",
    )


def test_render_a_frame_uses_cached_framebuffer_size() -> None:
    gx_helper = read("src/gx_helper.h")
    body = function_body(gx_helper, "render_a_frame")
    require(
        "glViewport(0, 0, window->framebuffer_width, window->framebuffer_height)" in body,
        "render_a_frame must use the cached framebuffer dimensions",
    )
    require(
        "glfwGetFramebufferSize" not in body,
        "render_a_frame must not poll glfwGetFramebufferSize on the hot path",
    )
    require(
        "glfwGetWindowSize" not in body,
        "render_a_frame must not poll glfwGetWindowSize on the hot path",
    )


def test_imgui_new_frame_size_queries_are_intercepted() -> None:
    imgui_glfw = read("third_party/imgui/backends/imgui_impl_glfw.cpp")
    body = function_body(imgui_glfw, "ImGui_ImplGlfw_NewFrame")
    require(
        "glfwGetWindowSize(bd->Window, &w, &h)" in body,
        "ImGui_ImplGlfw_NewFrame must still expose the main-window size query that Orange intercepts",
    )
    require(
        "glfwGetFramebufferSize(bd->Window, &display_w, &display_h)" in body,
        "ImGui_ImplGlfw_NewFrame must still expose the main-framebuffer size query that Orange intercepts",
    )


def test_cache_context_registered_during_gx_init() -> None:
    gx_helper = read("src/gx_helper.h")
    body = function_body(gx_helper, "gx_init")
    require(
        "orange_imgui_glfw_set_size_cache_context(render_target, context)" in body,
        "gx_init must register the main-window size-cache context",
    )
    require(
        "glfwSetWindowSizeCallback(render_target, gx_window_size_callback)" in body,
        "gx_init must keep cached window dimensions updated by callback",
    )
    require(
        "glfwSetFramebufferSizeCallback(render_target, gx_framebuffer_size_callback)" in body,
        "gx_init must keep cached framebuffer dimensions updated by callback",
    )


def test_cache_context_cleared_before_window_destroy() -> None:
    gx_helper = read("src/gx_helper.h")
    body = function_body(gx_helper, "gx_cleanup")
    require(
        "orange_imgui_glfw_set_size_cache_context(nullptr, nullptr)" in body,
        "gx_cleanup must clear the size-cache context",
    )
    require(
        "glfwDestroyWindow(window->render_target)" in body,
        "gx_cleanup must destroy the GLFW render target",
    )
    require(
        body.index("orange_imgui_glfw_set_size_cache_context(nullptr, nullptr)")
        < body.index("glfwDestroyWindow(window->render_target)"),
        "size-cache context must be cleared before the GLFW window is destroyed",
    )


def test_imgui_backend_does_not_own_main_window_size_callbacks() -> None:
    imgui_glfw = read("third_party/imgui/backends/imgui_impl_glfw.cpp")
    body = function_body(imgui_glfw, "ImGui_ImplGlfw_InstallCallbacks")
    require(
        "glfwSetWindowSizeCallback" not in body,
        "ImGui GLFW main-window callback install must not replace Orange's window-size callback",
    )
    require(
        "glfwSetFramebufferSizeCallback" not in body,
        "ImGui GLFW main-window callback install must not replace Orange's framebuffer-size callback",
    )


def test_recording_start_resets_size_cache_stats() -> None:
    orange = read("src/orange.cpp")
    body = function_body(orange, "gui_request_recording_start_through_operator_path")
    require(
        "display_frame_rate_stats->Reset()" in body,
        "recording start must reset GUI frame-rate telemetry",
    )
    require(
        "orange_imgui_glfw_reset_size_cache_stats()" in body,
        "recording start must reset ImGui GLFW size-cache telemetry",
    )
    require(
        body.index("orange_imgui_glfw_reset_size_cache_stats()")
        < body.index("gui_note_recording_started("),
        "size-cache counters must reset before the recording run is marked started",
    )


def main() -> int:
    tests = [
        test_override_header_maps_glfw_size_queries,
        test_cmake_force_includes_override_for_imgui_glfw_backend,
        test_render_a_frame_uses_cached_framebuffer_size,
        test_imgui_new_frame_size_queries_are_intercepted,
        test_cache_context_registered_during_gx_init,
        test_cache_context_cleared_before_window_destroy,
        test_imgui_backend_does_not_own_main_window_size_callbacks,
        test_recording_start_resets_size_cache_stats,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("All ImGui GLFW size-cache wiring tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
