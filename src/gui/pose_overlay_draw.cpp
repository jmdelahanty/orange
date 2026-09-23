// src/gui/pose_overlay_draw.cpp — see pose_overlay_draw.h.
#include "gui/pose_overlay_draw.h"

namespace orange::gui {

namespace {
ImU32 keypoint_color(uint16_t label_id)
{
    static const ImU32 palette[] = {
        IM_COL32(255, 80, 80, 255),    // 0: swim bladder
        IM_COL32(80, 220, 255, 255),   // 1: eye left
        IM_COL32(255, 220, 60, 255),   // 2: eye right
        IM_COL32(160, 255, 120, 255),
        IM_COL32(255, 140, 255, 255),
        IM_COL32(255, 255, 255, 255),
    };
    return palette[label_id % (sizeof(palette) / sizeof(palette[0]))];
}
}  // namespace

void draw_pose_overlay_commands(ImDrawList* draw_list,
                                const std::vector<PoseOverlayDrawCommand>& commands,
                                const std::function<ImVec2(float, float)>& to_screen)
{
    if (!draw_list) {
        return;
    }
    for (const auto& cmd : commands) {
        switch (cmd.kind) {
            case PoseOverlayDrawCommand::Kind::kRect:
                draw_list->AddRect(to_screen(cmd.x0, cmd.y0), to_screen(cmd.x1, cmd.y1),
                                   IM_COL32(255, 200, 0, 200), 0.0f, 0, cmd.thickness);
                break;
            case PoseOverlayDrawCommand::Kind::kLine:
                draw_list->AddLine(to_screen(cmd.x0, cmd.y0), to_screen(cmd.x1, cmd.y1),
                                   IM_COL32(255, 255, 255, 220), cmd.thickness);
                break;
            case PoseOverlayDrawCommand::Kind::kCircle: {
                const ImVec2 c = to_screen(cmd.x0, cmd.y0);
                draw_list->AddCircleFilled(c, cmd.radius, keypoint_color(cmd.label_id), 12);
                draw_list->AddCircle(c, cmd.radius + 1.0f, IM_COL32(0, 0, 0, 200), 12, 1.0f);
                break;
            }
        }
    }
}

}  // namespace orange::gui
