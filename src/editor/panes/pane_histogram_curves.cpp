#include "pane_histogram_curves.h"
#include "../app_state.h"
#include "../curves_editor.h"
#include "../../tone_curve_utils.h"
#include "imgui.h"
#include <vector>

namespace { // Anonymous namespace for local helpers

static void DrawHistoCurve(ImDrawList* draw_list, const std::vector<float>& histo_data, ImVec2 canvas_pos, ImVec2 canvas_size, ImU32 color) {
    if (histo_data.empty()) return;

    std::vector<ImVec2> points;
    points.reserve(histo_data.size());

    for (size_t i = 0; i < histo_data.size(); ++i) {
        float x = canvas_pos.x + (static_cast<float>(i) / (histo_data.size() - 1)) * canvas_size.x;
        float y = canvas_pos.y + canvas_size.y - (histo_data[i] * canvas_size.y);
        points.emplace_back(x, y);
    }
    draw_list->AddPolyline(points.data(), points.size(), color, ImDrawFlags_None, 1.5f);
}

} // namespace

namespace Panes {

void render_histogram_curves(AppState& state) {
    ImGui::Text("Histogram"); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.8f, 0.8f, 0.8f, 1.0f)); ImGui::Checkbox("L##histo", &state.show_luma_histo); ImGui::PopStyleColor(); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.95f, 0.26f, 0.21f, 1.0f)); ImGui::Checkbox("R##histo", &state.show_r_histo); ImGui::PopStyleColor(); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.30f, 0.69f, 0.31f, 1.0f)); ImGui::Checkbox("G##histo", &state.show_g_histo); ImGui::PopStyleColor(); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.13f, 0.59f, 0.95f, 1.0f)); ImGui::Checkbox("B##histo", &state.show_b_histo); ImGui::PopStyleColor();

    float histo_height = 150.0f;
    ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, histo_height);
    
    // The entire canvas area is now one self-contained group for drawing and interaction.
    ImGui::BeginGroup();

    // -- Background and Histogram Drawing --
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_pos, canvas_pos + canvas_size, IM_COL32(20, 20, 22, 255));
    if (state.show_luma_histo) DrawHistoCurve(draw_list, state.histogram_luma, canvas_pos, canvas_size, IM_COL32(200, 200, 200, 255));
    if (state.show_r_histo) DrawHistoCurve(draw_list, state.histogram_r, canvas_pos, canvas_size, IM_COL32(244, 67, 54, 255));
    if (state.show_g_histo) DrawHistoCurve(draw_list, state.histogram_g, canvas_pos, canvas_size, IM_COL32(76, 175, 80, 255));
    if (state.show_b_histo) DrawHistoCurve(draw_list, state.histogram_b, canvas_pos, canvas_size, IM_COL32(33, 150, 243, 255));
    
    // -- Diagonal Reference Line --
    ImVec2 p1 = canvas_pos + ImVec2(0, canvas_size.y);
    ImVec2 p2 = canvas_pos + ImVec2(canvas_size.x, 0);
    draw_list->AddLine(p1, p2, IM_COL32(255, 255, 255, 100), 1.0f);
    draw_list->AddRect(canvas_pos, canvas_pos + canvas_size, IM_COL32(80, 80, 80, 255));

    if (state.show_curve_overlay) {
        bool curve_changed = false;

        const ImU32 color_r_dim = IM_COL32(100, 50, 50, 255);
        const ImU32 color_g_dim = IM_COL32(50, 100, 50, 255);
        const ImU32 color_b_dim = IM_COL32(50, 50, 100, 255);
        
        const float y_min_vis = -0.1f;
        const float y_max_vis = 1.1f;

        // Draw inactive curves by evaluating their splines.
        if (state.active_curve_channel != ActiveCurveChannel::Red) CurvesEditor::draw_readonly_spline(draw_list, state.params.curve_points_r, 1.0f, false, true, canvas_pos, canvas_size, color_r_dim, y_min_vis, y_max_vis);
        if (state.active_curve_channel != ActiveCurveChannel::Green && state.active_curve_channel != ActiveCurveChannel::Luma) CurvesEditor::draw_readonly_spline(draw_list, state.params.curve_points_g, 1.0f, false, true, canvas_pos, canvas_size, color_g_dim, y_min_vis, y_max_vis);
        if (state.active_curve_channel != ActiveCurveChannel::Blue) CurvesEditor::draw_readonly_spline(draw_list, state.params.curve_points_b, 1.0f, false, true, canvas_pos, canvas_size, color_b_dim, y_min_vis, y_max_vis);

        std::vector<Point>* active_points = nullptr;
        
        switch (state.active_curve_channel) {
            case ActiveCurveChannel::Luma:  active_points = &state.params.curve_points_luma; break;
            case ActiveCurveChannel::Red:   active_points = &state.params.curve_points_r;    break;
            case ActiveCurveChannel::Green: active_points = &state.params.curve_points_g;    break;
            case ActiveCurveChannel::Blue:  active_points = &state.params.curve_points_b;    break;
        }
        
        // The editor now handles its own interaction button and state.
        curve_changed = CurvesEditor::render("##ToneCurveEditor", *active_points, canvas_size, 1.0f, false, true, y_min_vis, y_max_vis);

        if (curve_changed) {
            if (state.active_curve_channel == ActiveCurveChannel::Luma) {
                state.params.curve_points_r = state.params.curve_points_luma;
                state.params.curve_points_g = state.params.curve_points_luma;
                state.params.curve_points_b = state.params.curve_points_luma;
            }
            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
    }
    ImGui::EndGroup();

    ImGui::Checkbox("Curve##toggle", &state.show_curve_overlay);
    if (state.show_curve_overlay) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 140);
        float button_size = ImGui::GetFrameHeight();
        ImVec4 bg_color_l = (state.active_curve_channel == ActiveCurveChannel::Luma) ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
        ImVec4 bg_color_r = (state.active_curve_channel == ActiveCurveChannel::Red) ? ImVec4(0.8f, 0.3f, 0.3f, 1.0f) : ImVec4(0.5f, 0.2f, 0.2f, 1.0f);
        ImVec4 bg_color_g = (state.active_curve_channel == ActiveCurveChannel::Green) ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.2f, 0.5f, 0.2f, 1.0f);
        ImVec4 bg_color_b = (state.active_curve_channel == ActiveCurveChannel::Blue) ? ImVec4(0.3f, 0.3f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.5f, 1.0f);

        auto switch_channel = [&](ActiveCurveChannel ch) {
            if (state.active_curve_channel == ch) return;
            state.active_curve_channel = ch;
            if (ch == ActiveCurveChannel::Luma) {
                ToneCurveUtils::average_rgb_to_luma(state.params);
            }
        };

        ImGui::PushStyleColor(ImGuiCol_Button, bg_color_l); if (ImGui::Button("L##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Luma); } ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, bg_color_r); if (ImGui::Button("R##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Red); } ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, bg_color_g); if (ImGui::Button("G##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Green); } ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, bg_color_b); if (ImGui::Button("B##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Blue); } ImGui::PopStyleColor();
    }
}

} // namespace Panes
