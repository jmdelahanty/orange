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


def main() -> int:
    tests = [
        test_override_header_maps_glfw_size_queries,
        test_cmake_force_includes_override_for_imgui_glfw_backend,
        test_render_a_frame_uses_cached_framebuffer_size,
        test_cache_context_registered_during_gx_init,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("All ImGui GLFW size-cache wiring tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
