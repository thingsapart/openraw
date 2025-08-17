#define IMGUI_DEFINE_MATH_OPERATORS

#include "panes/pane_histogram_curves.h"
#include "app_state.h"
#include "editor/curves_editor.h"
#include "tone_curve_utils.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <vector>

namespace { // Anonymous namespace for local helpers

// Helper to draw a single histogram channel.
void DrawHistoCurve(ImDrawList* draw_list, const std::vector<float>& histo_data, const ImVec2& canvas_pos, const ImVec2& canvas_size, ImU32 color) {
    if (histo_data.empty()) return;

    std::vector<ImVec2> points;
    points.reserve(histo_data.size());

    for (size_t i = 0; i < histo_data.size(); ++i) {
        float x = canvas_pos.x + (static_cast<float>(i) / (histo_data.size() - 1)) * (canvas_size.x - 1.0f);
        float y = canvas_pos.y + canvas_size.y - (histo_data[i] * (canvas_size.y - 1.0f));
        points.emplace_back(x, y);
    }
    draw_list->AddPolyline(points.data(), points.size(), color, ImDrawFlags_None, 1.5f);
}


// Helper for colored buttons. Pushes style if active, returns true if clicked.
bool ColoredButton(const char* label, bool is_active, const ImVec4& active_color, const ImVec2& size = ImVec2(0,0)) {
    if (is_active) {
        ImGui::PushStyleColor(ImGuiCol_Button, active_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_color);
    }
    bool clicked = ImGui::Button(label, size);
    if (is_active) {
        ImGui::PopStyleColor(3);
    }
    return clicked;
}

} // anonymous namespace

namespace Panes {

bool render_histogram_curves(AppState& state) {
    bool changed_by_this_pane = false;

    // --- 1. Create a single container for both histogram and curve editor ---
    const float widget_height = 150.0f;
    const ImVec2 widget_size(ImGui::GetContentRegionAvail().x, widget_height);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::BeginChild("HistoCurveContainer", widget_size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // --- 2. Draw background and grid for the container ---
    draw_list->AddRectFilled(canvas_pos, canvas_pos + canvas_size, IM_COL32(20, 20, 22, 255));
    for (int i = 1; i < 4; ++i) {
        float x_pos = canvas_pos.x + i * canvas_size.x / 4.0f;
        float y_pos = canvas_pos.y + i * canvas_size.y / 4.0f;
        draw_list->AddLine(ImVec2(x_pos, canvas_pos.y), ImVec2(x_pos, canvas_pos.y + canvas_size.y), IM_COL32(50,50,50,255));
        draw_list->AddLine(ImVec2(canvas_pos.x, y_pos), ImVec2(canvas_pos.x + canvas_size.x, y_pos), IM_COL32(50,50,50,255));
    }
    // Draw the diagonal reference line
    draw_list->AddLine(ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y), ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y), IM_COL32(255, 255, 255, 60));
    draw_list->AddRect(canvas_pos, canvas_pos + canvas_size, IM_COL32(80, 80, 80, 255));

    // --- 3. Draw visible histograms using the container's bounds ---
    const ImU32 color_luma_u32 = IM_COL32(200, 200, 200, 255);
    const ImU32 color_r_u32    = IM_COL32(244, 67, 54, 255);
    const ImU32 color_g_u32    = IM_COL32(76, 175, 80, 255);
    const ImU32 color_b_u32    = IM_COL32(33, 150, 243, 255);

    if (state.show_luma_histo) DrawHistoCurve(draw_list, state.histogram_luma, canvas_pos, canvas_size, color_luma_u32);
    if (state.show_r_histo)    DrawHistoCurve(draw_list, state.histogram_r,    canvas_pos, canvas_size, color_r_u32);
    if (state.show_g_histo)    DrawHistoCurve(draw_list, state.histogram_g,    canvas_pos, canvas_size, color_g_u32);
    if (state.show_b_histo)    DrawHistoCurve(draw_list, state.histogram_b,    canvas_pos, canvas_size, color_b_u32);

    // --- 4. Overlay the curve editor widget ---
    ImGui::SetCursorScreenPos(canvas_pos);

    struct CurveInfo {
        ActiveCurveChannel channel;
        std::vector<Point>& points;
        ImU32 color;
        const char* id;
    };
    std::vector<CurveInfo> all_curves = {
        {ActiveCurveChannel::Luma,  state.params.curve_points_luma, color_luma_u32, "luma_curve"},
        {ActiveCurveChannel::Red,   state.params.curve_points_r,    color_r_u32,    "r_curve"},
        {ActiveCurveChannel::Green, state.params.curve_points_g,    color_g_u32,    "g_curve"},
        {ActiveCurveChannel::Blue,  state.params.curve_points_b,    color_b_u32,    "b_curve"},
    };

    // Draw inactive curves first (read-only)
    for (const auto& curve : all_curves) {
        if (curve.channel != state.active_curve_channel) {
            CurvesEditor::draw_readonly_spline(draw_list, curve.points, 1.0f, false, true, canvas_pos, canvas_size, curve.color, 0.0f, 1.0f);
        }
    }

    // Draw the active curve on top (interactive)
    bool curve_changed = false;
    for (const auto& curve : all_curves) {
        if (curve.channel == state.active_curve_channel) {
            if (CurvesEditor::render(curve.id, curve.points, canvas_size, 1.0f, false, true, 0.0f, 1.0f)) {
                curve_changed = true;
            }
            break;
        }
    }
    
    ImGui::EndChild();

    if (curve_changed) {
        changed_by_this_pane = true;
        if (state.active_curve_channel == ActiveCurveChannel::Luma) {
            // Luma is the master curve; editing it syncs all RGB channels.
            state.params.curve_points_r = state.params.curve_points_luma;
            state.params.curve_points_g = state.params.curve_points_luma;
            state.params.curve_points_b = state.params.curve_points_luma;
        } else {
            // An RGB channel was edited; update the Luma curve to be the average.
            ToneCurveUtils::average_rgb_to_luma(state.params);
        }
    }

    // --- 5. Draw controls (checkboxes and new curve selection buttons) ---
    const ImVec4 color_luma_v4 = ImVec4(200/255.f, 200/255.f, 200/255.f, 1.0f);
    const ImVec4 color_r_v4    = ImVec4(244/255.f, 67/255.f, 54/255.f, 1.0f);
    const ImVec4 color_g_v4    = ImVec4(76/255.f, 175/255.f, 80/255.f, 1.0f);
    const ImVec4 color_b_v4    = ImVec4(33/255.f, 150/255.f, 243/255.f, 1.0f);

    ImGui::PushItemWidth(25);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, color_luma_v4);
    ImGui::Checkbox("##L_histo_check", &state.show_luma_histo); ImGui::SameLine(0, 2);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_CheckMark, color_r_v4);
    ImGui::Checkbox("##R_histo_check", &state.show_r_histo); ImGui::SameLine(0, 2);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_CheckMark, color_g_v4);
    ImGui::Checkbox("##G_histo_check", &state.show_g_histo); ImGui::SameLine(0, 2);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_CheckMark, color_b_v4);
    ImGui::Checkbox("##B_histo_check", &state.show_b_histo); ImGui::SameLine(0, 8);
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();
    
    ImVec2 button_size(25, ImGui::GetFrameHeight());
    const ImVec4 active_l_color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    const ImVec4 active_r_color = ImVec4(0.95f, 0.26f, 0.21f, 1.0f);
    const ImVec4 active_g_color = ImVec4(0.29f, 0.68f, 0.31f, 1.0f);
    const ImVec4 active_b_color = ImVec4(0.12f, 0.58f, 0.95f, 1.0f);

    if (ColoredButton("L##curve_btn", state.active_curve_channel == ActiveCurveChannel::Luma, active_l_color, button_size)) {
        state.active_curve_channel = ActiveCurveChannel::Luma;
    }
    ImGui::SameLine(0, 2);
    if (ColoredButton("R##curve_btn", state.active_curve_channel == ActiveCurveChannel::Red, active_r_color, button_size)) {
        state.active_curve_channel = ActiveCurveChannel::Red;
    }
    ImGui::SameLine(0, 2);
    if (ColoredButton("G##curve_btn", state.active_curve_channel == ActiveCurveChannel::Green, active_g_color, button_size)) {
        state.active_curve_channel = ActiveCurveChannel::Green;
    }
    ImGui::SameLine(0, 2);
    if (ColoredButton("B##curve_btn", state.active_curve_channel == ActiveCurveChannel::Blue, active_b_color, button_size)) {
        state.active_curve_channel = ActiveCurveChannel::Blue;
    }
    
    if (curve_changed) {
        if (state.active_curve_channel == ActiveCurveChannel::Red ||
            state.active_curve_channel == ActiveCurveChannel::Green ||
            state.active_curve_channel == ActiveCurveChannel::Blue) {
            state.params.curve_mode = 1; // Switch to RGB mode if an RGB curve is edited
        } else {
            state.params.curve_mode = 0; // Switch to Luma mode
        }
    }

    return changed_by_this_pane;
}

} // namespace Panes
