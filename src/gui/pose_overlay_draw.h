// src/gui/pose_overlay_draw.h — ImGui glue for the pose overlay commands.
#pragma once

#include "gui/pose_overlay.h"

#include <functional>
#include <vector>

#include "imgui.h"

namespace orange::gui {

// Draws prebuilt overlay commands. `to_screen` maps command coordinates to
// screen pixels: identity when the commands were built for the item
// rectangle, ImPlot::PlotToPixels when they were built in preview pixels
// for an image canvas.
void draw_pose_overlay_commands(ImDrawList* draw_list,
                                const std::vector<PoseOverlayDrawCommand>& commands,
                                const std::function<ImVec2(float, float)>& to_screen);

}  // namespace orange::gui
