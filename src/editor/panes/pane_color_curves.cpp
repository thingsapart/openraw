#include "pane_color_curves.h"
#include "../app_state.h"
#include "../curves_editor.h"
#include "../../tone_curve_utils.h"
#include "imgui.h"

namespace { // Anonymous namespace for local helpers

enum class CurveBackgroundType {
    Luma,
    Hue,
    Saturation
};

void DrawCurveBackground(ImDrawList* draw_list, ImVec2 pos, ImVec2 size, CurveBackgroundType type) {
    ImU32 col_trans = IM_COL32(0,0,0,0);
    if (type == CurveBackgroundType::Luma) {
        ImU32 col_a = IM_COL32(20, 20, 22, 255);
        ImU32 col_b = IM_COL32(230, 230, 235, 255);
        draw_list->AddRectFilledMultiColor(pos, pos + size, col_a, col_b, col_b, col_a);
    } else if (type == CurveBackgroundType::Hue) {
        for (int i = 0; i < (int)size.x; ++i) {
            float hue = (float)i / size.x;
            ImVec4 rgb;
            ImGui::ColorConvertHSVtoRGB(hue, 0.7f, 0.7f, rgb.x, rgb.y, rgb.z);
            draw_list->AddLine(ImVec2(pos.x + i, pos.y), ImVec2(pos.x + i, pos.y + size.y), IM_COL32(rgb.x*255, rgb.y*255, rgb.z*255, 255));
        }
    } else if (type == CurveBackgroundType::Saturation) {
        ImU32 col_a = IM_COL32(128, 128, 128, 255); // Mid-gray for desaturated
        ImU32 col_b = IM_COL32(230, 50, 50, 255);   // Saturated Red
        draw_list->AddRectFilledMultiColor(pos, pos + size, col_a, col_b, col_b, col_a);
    }
    draw_list->AddRect(pos, pos + size, IM_COL32(80, 80, 80, 255));
}

bool RenderCurveEditor(const char* id, std::vector<Point>& points, float default_y, bool is_additive, bool is_identity, float y_min, float y_max, CurveBackgroundType bg_type) {
    // Ensure default points exist for interaction.
    if (points.empty()) {
        if (is_identity) {
            points.push_back({0.0f, 0.0f});
            points.push_back({1.0f, 1.0f});
        } else {
            points.push_back({0.0f, default_y});
            points.push_back({1.0f, default_y});
        }
    }
    
    ImGui::PushID(id);

    float editor_height = 150.0f;
    ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, editor_height);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // -- Background and Grid --
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    DrawCurveBackground(draw_list, canvas_pos, canvas_size, bg_type);
    float y_center = canvas_pos.y + canvas_size.y * (y_max - default_y) / (y_max - y_min);
    draw_list->AddLine(ImVec2(canvas_pos.x, y_center), ImVec2(canvas_pos.x + canvas_size.x, y_center), IM_COL32(255, 255, 255, 100));

    // -- The Interactive Curve Widget --
    CurvesEditor editor;
    bool changed = editor.render(id, points, canvas_size, default_y, is_additive, is_identity, y_min, y_max);
    
    ImGui::PopID();
    return changed;
}

} // namespace


namespace Panes {

bool render_color_curves(AppState& state) {
    bool changed = false;
    
    ImGui::BeginTabBar("ColorCurvesTabBar");

    if (ImGui::BeginTabItem("H vs H")) {
        changed |= RenderCurveEditor("hvh", state.params.curve_hue_vs_hue, 0.0f, true, false, -0.5f, 0.5f, CurveBackgroundType::Hue);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("H vs S")) {
        changed |= RenderCurveEditor("hvs", state.params.curve_hue_vs_sat, 1.0f, false, false, 0.0f, 2.0f, CurveBackgroundType::Hue);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("H vs L")) {
        changed |= RenderCurveEditor("hvl", state.params.curve_hue_vs_lum, 0.0f, true, false, -0.5f, 0.5f, CurveBackgroundType::Hue);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("L vs S")) {
        changed |= RenderCurveEditor("lvs", state.params.curve_lum_vs_sat, 1.0f, false, false, 0.0f, 2.0f, CurveBackgroundType::Luma);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("S vs S")) {
        changed |= RenderCurveEditor("svs", state.params.curve_sat_vs_sat, 1.0f, false, true, 0.0f, 1.0f, CurveBackgroundType::Saturation);
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    return changed;
}

} // namespace Panes
