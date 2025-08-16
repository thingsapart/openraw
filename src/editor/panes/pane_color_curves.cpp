#include "pane_color_curves.h"
#include "../app_state.h"
#include "../curves_editor.h"
#include "../../tone_curve_utils.h"
#include "imgui.h"
#include "imgui_internal.h" // For ImLerp

namespace { // Anonymous namespace for local helpers

enum class CurveBackgroundType {
    LumaVsSat,
    Hue,
    SatVsSat
};

enum class HueSubType {
    HvsH, HvsS, HvsL
};

// Converts the backend's normalized hue [0,1] (where 0.5=Red) to the visual HSV hue [0,1] (where 0=Red).
float backend_hue_to_display_hue(float h_norm_backend) {
    // Backend: 0=Green, 0.25=Blue, 0.5=Red, 0.75=Yellow, 1.0=Green
    // Display: 0=Red, 0.166=Yellow, 0.333=Green, 0.666=Blue, 1.0=Red
    if (h_norm_backend < 0.25f)      // Green -> Blue
        return ImLerp(120.0f/360.0f, 240.0f/360.0f, h_norm_backend / 0.25f);
    else if (h_norm_backend < 0.5f)  // Blue -> Red
        return ImLerp(240.0f/360.0f, 360.0f/360.0f, (h_norm_backend - 0.25f) / 0.25f);
    else if (h_norm_backend < 0.75f) // Red -> Yellow
        return ImLerp(0.0f/360.0f, 60.0f/360.0f, (h_norm_backend - 0.5f) / 0.25f);
    else                             // Yellow -> Green
        return ImLerp(60.0f/360.0f, 120.0f/360.0f, (h_norm_backend - 0.75f) / 0.25f);
}

void DrawCurveBackground(ImDrawList* draw_list, ImVec2 pos, ImVec2 size,
                         CurveBackgroundType type, HueSubType sub_type,
                         float default_y, float y_min, float y_max)
{
    if (type == CurveBackgroundType::LumaVsSat) {
        for (int x = 0; x < (int)size.x; ++x) {
            float luma = (float)x / (size.x - 1);
            float h = 0.66f, s_bottom = 0.0f, s_top = 1.0f;
            ImVec4 c1_rgb, c2_rgb;
            ImGui::ColorConvertHSVtoRGB(h, s_bottom, luma, c1_rgb.x, c1_rgb.y, c1_rgb.z);
            ImGui::ColorConvertHSVtoRGB(h, s_top, luma, c2_rgb.x, c2_rgb.y, c2_rgb.z);
            draw_list->AddRectFilledMultiColor(ImVec2(pos.x + x, pos.y), ImVec2(pos.x + x + 1, pos.y + size.y),
                                               IM_COL32(c2_rgb.x*255, c2_rgb.y*255, c2_rgb.z*255, 255),
                                               IM_COL32(c2_rgb.x*255, c2_rgb.y*255, c2_rgb.z*255, 255),
                                               IM_COL32(c1_rgb.x*255, c1_rgb.y*255, c1_rgb.z*255, 255),
                                               IM_COL32(c1_rgb.x*255, c1_rgb.y*255, c1_rgb.z*255, 255));
        }
    } else if (type == CurveBackgroundType::SatVsSat) {
        ImVec4 c1, c2;
        ImGui::ColorConvertHSVtoRGB(0.0f, 0.0f, 0.8f, c1.x, c1.y, c1.z); // Gray
        ImGui::ColorConvertHSVtoRGB(0.0f, 1.0f, 0.8f, c2.x, c2.y, c2.z); // Red
        draw_list->AddRectFilledMultiColor(pos, pos + size, 
            IM_COL32(c1.x*255, c1.y*255, c1.z*255, 255),
            IM_COL32(c2.x*255, c2.y*255, c2.z*255, 255),
            IM_COL32(c2.x*255, c2.y*255, c2.z*255, 255),
            IM_COL32(c1.x*255, c1.y*255, c1.z*255, 255));

    } else if (type == CurveBackgroundType::Hue) {
        const float strip_half_width = 10.0f;
        const float opaque_half_width = 2.5f;
        const float center_y_px = pos.y + size.y * (y_max - default_y) / (y_max - y_min);

        for (int x = 0; x < (int)size.x; ++x) {
            float h_norm_backend = (float)x / (size.x - 1.0f);
            float display_hue = backend_hue_to_display_hue(h_norm_backend);

            for (int y = 0; y < (int)size.y; ++y) {
                float y_norm = (float)y / (size.y - 1.0f);
                float output_amount = ImLerp(y_max, y_min, y_norm);

                float final_hue = display_hue, final_sat = 0.8f, final_val = 0.8f;
                if (sub_type == HueSubType::HvsH) {
                    final_hue = fmodf(display_hue + output_amount, 1.0f);
                    if (final_hue < 0.0f) final_hue += 1.0f;
                } else if (sub_type == HueSubType::HvsS) {
                    final_sat = ImClamp(output_amount, 0.0f, 1.0f);
                } else if (sub_type == HueSubType::HvsL) {
                    final_val = ImClamp(0.5f + output_amount, 0.0f, 1.0f);
                }

                ImVec4 rgb_bg;
                ImGui::ColorConvertHSVtoRGB(final_hue, final_sat, final_val, rgb_bg.x, rgb_bg.y, rgb_bg.z);
                
                float dist_from_center = fabsf((pos.y + y) - center_y_px);
                float alpha = 0.0f;
                if (dist_from_center < opaque_half_width) {
                    alpha = 1.0f;
                } else if (dist_from_center < strip_half_width) {
                    alpha = 1.0f - (dist_from_center - opaque_half_width) / (strip_half_width - opaque_half_width);
                }
                
                if (alpha > 0.0f) {
                    ImVec4 rgb_strip;
                    ImGui::ColorConvertHSVtoRGB(display_hue, 0.7f, 0.7f, rgb_strip.x, rgb_strip.y, rgb_strip.z);
                    rgb_bg.x = ImLerp(rgb_bg.x, rgb_strip.x, alpha);
                    rgb_bg.y = ImLerp(rgb_bg.y, rgb_strip.y, alpha);
                    rgb_bg.z = ImLerp(rgb_bg.z, rgb_strip.z, alpha);
                }
                draw_list->AddRectFilled(ImVec2(pos.x + x, pos.y + y), ImVec2(pos.x + x + 1, pos.y + y + 1), IM_COL32(rgb_bg.x*255, rgb_bg.y*255, rgb_bg.z*255, 255));
            }
        }
    }
    draw_list->AddRect(pos, pos + size, IM_COL32(80, 80, 80, 255));
}


bool RenderCurveEditor(const char* id, std::vector<Point>& points, float default_y, bool is_additive, bool is_identity, float y_min, float y_max, CurveBackgroundType bg_type, HueSubType sub_type = HueSubType::HvsH) {
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
    DrawCurveBackground(draw_list, canvas_pos, canvas_size, bg_type, sub_type, default_y, y_min, y_max);
    float y_center = canvas_pos.y + canvas_size.y * (y_max - default_y) / (y_max - y_min);
    draw_list->AddLine(ImVec2(canvas_pos.x, y_center), ImVec2(canvas_pos.x + canvas_size.x, y_center), IM_COL32(255, 255, 255, 100));

    // -- The Interactive Curve Widget --
    bool changed = CurvesEditor::render(id, points, canvas_size, default_y, is_additive, is_identity, y_min, y_max);
    
    ImGui::PopID();
    return changed;
}

} // namespace


namespace Panes {

bool render_color_curves(AppState& state) {
    bool changed = false;
    
    ImGui::BeginTabBar("ColorCurvesTabBar");

    if (ImGui::BeginTabItem("H vs H")) {
        changed |= RenderCurveEditor("hvh", state.params.curve_hue_vs_hue, 0.0f, true, false, -0.5f, 0.5f, CurveBackgroundType::Hue, HueSubType::HvsH);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("H vs S")) {
        changed |= RenderCurveEditor("hvs", state.params.curve_hue_vs_sat, 1.0f, false, false, 0.0f, 2.0f, CurveBackgroundType::Hue, HueSubType::HvsS);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("H vs L")) {
        changed |= RenderCurveEditor("hvl", state.params.curve_hue_vs_lum, 0.0f, true, false, -0.5f, 0.5f, CurveBackgroundType::Hue, HueSubType::HvsL);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("L vs S")) {
        changed |= RenderCurveEditor("lvs", state.params.curve_lum_vs_sat, 1.0f, false, false, 0.0f, 2.0f, CurveBackgroundType::LumaVsSat);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("S vs S")) {
        changed |= RenderCurveEditor("svs", state.params.curve_sat_vs_sat, 1.0f, false, true, 0.0f, 1.0f, CurveBackgroundType::SatVsSat);
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    return changed;
}

} // namespace Panes
