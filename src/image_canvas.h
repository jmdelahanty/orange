#ifndef ORANGE_IMAGE_CANVAS_H
#define ORANGE_IMAGE_CANVAS_H

#include "imgui.h"
#include "implot.h"

#include <GL/glew.h>

#include <algorithm>

namespace orange::ui {

struct ImageCanvasViewState {
    bool fit_requested = true;
    int last_image_width = 0;
    int last_image_height = 0;
};

inline ImVec2 compute_image_canvas_size(int image_width, int image_height, float max_display_fraction)
{
    ImVec2 available = ImGui::GetContentRegionAvail();
    const float width = std::max(200.0f, available.x);
    const float aspect = (image_width > 0) ? (static_cast<float>(image_height) / static_cast<float>(image_width)) : 1.0f;
    const float preferred_height = width * aspect;
    const float max_height = std::clamp(ImGui::GetIO().DisplaySize.y * max_display_fraction, 260.0f, 1200.0f);
    float height = std::clamp(preferred_height, 220.0f, max_height);
    if (available.y > 220.0f) {
        height = std::min(height, available.y);
    }
    return ImVec2(width, height);
}

inline bool begin_image_canvas(
    const char* plot_id,
    GLuint texture,
    int image_width,
    int image_height,
    ImageCanvasViewState* view_state,
    float max_display_fraction,
    const char* image_item_id = "##image_canvas")
{
    if (texture == 0 || image_width <= 0 || image_height <= 0 || view_state == nullptr) {
        return false;
    }

    if (view_state->last_image_width != image_width || view_state->last_image_height != image_height) {
        view_state->last_image_width = image_width;
        view_state->last_image_height = image_height;
        view_state->fit_requested = true;
    }

    const ImVec2 canvas_size = compute_image_canvas_size(image_width, image_height, max_display_fraction);
    const ImPlotAxisFlags axis_flags =
        ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines;

    if (!ImPlot::BeginPlot(plot_id, canvas_size, ImPlotFlags_Equal)) {
        return false;
    }

    ImPlot::SetupAxes("Pixel X", "Pixel Y", axis_flags, axis_flags | ImPlotAxisFlags_Invert);
    if (view_state->fit_requested) {
        ImPlot::SetupAxesLimits(0.0, static_cast<double>(image_width), 0.0, static_cast<double>(image_height), ImGuiCond_Always);
        view_state->fit_requested = false;
    } else {
        ImPlot::SetupAxesLimits(0.0, static_cast<double>(image_width), 0.0, static_cast<double>(image_height), ImGuiCond_Once);
    }

    // Keep plot coordinates in camera-pixel convention (origin top-left,
    // y increasing downward) while drawing the texture with the same visual
    // orientation as ImGui::Image/live camera previews.
    ImPlot::PlotImage(image_item_id,
                      (void*)(intptr_t)texture,
                      ImPlotPoint(0, 0),
                      ImPlotPoint(static_cast<double>(image_width), static_cast<double>(image_height)),
                      ImVec2(0, 1),
                      ImVec2(1, 0));
    return true;
}

} // namespace orange::ui

#endif
