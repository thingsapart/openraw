#include "editor_ui.h"
#include "app_state.h"
#include "halide_runner.h"
#include "texture_utils.h"
#include "curves_editor.h"
#include "tone_curve_utils.h"

#include "imgui.h"
#include "imgui_internal.h" // For DockBuilder

#include <string>
#include <iostream>
#include <algorithm> // for std::max, std::min
#include <cmath>     // for fabsf, powf
#include <vector>    // for std::vector
#include <cstring>   // for memcpy

// --- Helper Functions for Drawing (static to this file) ---

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

// This function now renders all right-hand panels inside a single, scrollable window.
static void RenderRightPanel(AppState& state) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("Adjustments");
    ImGui::PopStyleVar();

    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (state.input_image.data()) {
            // ... (Preview logic is unchanged) ...
        }
    }

    if (ImGui::CollapsingHeader("Histogram & Curves", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Histogram"); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.8f, 0.8f, 0.8f, 1.0f)); ImGui::Checkbox("L##histo", &state.show_luma_histo); ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.95f, 0.26f, 0.21f, 1.0f)); ImGui::Checkbox("R##histo", &state.show_r_histo); ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.30f, 0.69f, 0.31f, 1.0f)); ImGui::Checkbox("G##histo", &state.show_g_histo); ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.13f, 0.59f, 0.95f, 1.0f)); ImGui::Checkbox("B##histo", &state.show_b_histo); ImGui::PopStyleColor();

        float histo_height = 150.0f;
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, histo_height);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // Use a group to manage the overlay of the curve editor on the histogram canvas.
        ImGui::BeginGroup();

        // This InvisibleButton is now the single source of truth for all canvas interactions.
        ImGui::InvisibleButton("##curve_canvas_interaction", canvas_size);
        bool canvas_hovered = ImGui::IsItemHovered();
        bool canvas_active = ImGui::IsItemActive();

        draw_list->AddRectFilled(canvas_pos, canvas_pos + canvas_size, IM_COL32(20, 20, 22, 255));

        if (state.show_luma_histo) DrawHistoCurve(draw_list, state.histogram_luma, canvas_pos, canvas_size, IM_COL32(200, 200, 200, 255));
        if (state.show_r_histo) DrawHistoCurve(draw_list, state.histogram_r, canvas_pos, canvas_size, IM_COL32(244, 67, 54, 255));
        if (state.show_g_histo) DrawHistoCurve(draw_list, state.histogram_g, canvas_pos, canvas_size, IM_COL32(76, 175, 80, 255));
        if (state.show_b_histo) DrawHistoCurve(draw_list, state.histogram_b, canvas_pos, canvas_size, IM_COL32(33, 150, 243, 255));
        draw_list->AddRect(canvas_pos, canvas_pos + canvas_size, IM_COL32(80, 80, 80, 255));

        if (state.show_curve_overlay) {
            static CurvesEditor curves_editor;
            bool curve_changed = false;

            const ImU32 color_r_dim = IM_COL32(100, 50, 50, 255);
            const ImU32 color_g_dim = IM_COL32(50, 100, 50, 255);
            const ImU32 color_b_dim = IM_COL32(50, 50, 100, 255);

            if (state.active_curve_channel != ActiveCurveChannel::Red) CurvesEditor::draw_readonly_curve(draw_list, state.ui_tone_curve_lut, 0, canvas_pos, canvas_size, color_r_dim);
            if (state.active_curve_channel != ActiveCurveChannel::Green && state.active_curve_channel != ActiveCurveChannel::Luma) CurvesEditor::draw_readonly_curve(draw_list, state.ui_tone_curve_lut, 1, canvas_pos, canvas_size, color_g_dim);
            if (state.active_curve_channel != ActiveCurveChannel::Blue) CurvesEditor::draw_readonly_curve(draw_list, state.ui_tone_curve_lut, 2, canvas_pos, canvas_size, color_b_dim);

            std::vector<Point>* active_points = nullptr;
            int active_channel_idx = 1;
            ImU32 active_color = IM_COL32(255, 255, 255, 255);

            switch(state.active_curve_channel) {
                case ActiveCurveChannel::Luma:  active_points = &state.params.curve_points_luma; active_channel_idx = 1; active_color = IM_COL32(224, 224, 224, 255); break;
                case ActiveCurveChannel::Red:   active_points = &state.params.curve_points_r;    active_channel_idx = 0; active_color = IM_COL32(244, 67, 54, 255); break;
                case ActiveCurveChannel::Green: active_points = &state.params.curve_points_g;    active_channel_idx = 1; active_color = IM_COL32(76, 175, 80, 255); break;
                case ActiveCurveChannel::Blue:  active_points = &state.params.curve_points_b;    active_channel_idx = 2; active_color = IM_COL32(33, 150, 243, 255); break;
            }

            curve_changed = curves_editor.render_multi_channel(*active_points, state.ui_tone_curve_lut, active_channel_idx, canvas_pos, canvas_size, active_color, canvas_hovered, canvas_active, ImGui::GetIO());

            if (curve_changed) {
                if (state.active_curve_channel == ActiveCurveChannel::Luma) {
                    state.params.curve_points_r = state.params.curve_points_luma;
                    state.params.curve_points_g = state.params.curve_points_luma;
                    state.params.curve_points_b = state.params.curve_points_luma;
                }
                ToneCurveUtils::generate_linear_lut(state.params, state.ui_tone_curve_lut);
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
                    ToneCurveUtils::generate_linear_lut(state.params, state.ui_tone_curve_lut);
                }
            };

            ImGui::PushStyleColor(ImGuiCol_Button, bg_color_l); if (ImGui::Button("L##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Luma); } ImGui::PopStyleColor(); ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, bg_color_r); if (ImGui::Button("R##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Red); } ImGui::PopStyleColor(); ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, bg_color_g); if (ImGui::Button("G##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Green); } ImGui::PopStyleColor(); ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, bg_color_b); if (ImGui::Button("B##btn", {button_size, button_size})) { switch_channel(ActiveCurveChannel::Blue); } ImGui::PopStyleColor();
        }
    }

    // The scrolling area is now a borderless child window for a seamless look.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::BeginChild("##adjustments_scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    bool pipeline_changed = false;

    if (ImGui::CollapsingHeader("Core Pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* demosaic_items[] = { "fast", "ahd", "lmmse", "ri" };
        int current_item_idx = 0;
        for(int n = 0; n < IM_ARRAYSIZE(demosaic_items); n++) {
            if (state.params.demosaic_algorithm == demosaic_items[n]) {
                current_item_idx = n;
                break;
            }
        }
        if (ImGui::Combo("Demosaic", &current_item_idx, demosaic_items, IM_ARRAYSIZE(demosaic_items))) {
            state.params.demosaic_algorithm = demosaic_items[current_item_idx];
            pipeline_changed = true;
        }

        pipeline_changed |= ImGui::SliderFloat("Exposure", &state.params.exposure, -4.0f, 4.0f, "%.2f");
        pipeline_changed |= ImGui::SliderFloat("Color Temp", &state.params.color_temp, 1500.0f, 15000.0f, "%.0f K");
        pipeline_changed |= ImGui::SliderFloat("Tint", &state.params.tint, -1.0f, 1.0f);
        pipeline_changed |= ImGui::SliderFloat("CA Correction", &state.params.ca_strength, 0.0f, 2.0f);
    }

    if (ImGui::CollapsingHeader("Denoise", ImGuiTreeNodeFlags_DefaultOpen)) {
        pipeline_changed |= ImGui::SliderFloat("Strength", &state.params.denoise_strength, 0.0f, 100.0f);
        pipeline_changed |= ImGui::DragFloat("Epsilon", &state.params.denoise_eps, 0.0001f, 0.0001f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
    }

    if (ImGui::CollapsingHeader("Local Adjustments", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Based on Laplacian Pyramid");
        pipeline_changed |= ImGui::SliderFloat("Detail", &state.params.ll_detail, -100.0f, 100.0f);
        pipeline_changed |= ImGui::SliderFloat("Clarity", &state.params.ll_clarity, -100.0f, 100.0f);
        ImGui::Separator();
        pipeline_changed |= ImGui::SliderFloat("Shadows", &state.params.ll_shadows, -100.0f, 100.0f);
        pipeline_changed |= ImGui::SliderFloat("Highlights", &state.params.ll_highlights, -100.0f, 100.0f);
        ImGui::Separator();
        pipeline_changed |= ImGui::SliderFloat("Blacks", &state.params.ll_blacks, -100.0f, 100.0f);
        pipeline_changed |= ImGui::SliderFloat("Whites", &state.params.ll_whites, -100.0f, 100.0f);
    }

    if (ImGui::CollapsingHeader("Tone & Curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        pipeline_changed |= ImGui::SliderFloat("Contrast", &state.params.contrast, 0.0f, 100.0f);
        pipeline_changed |= ImGui::SliderFloat("Gamma", &state.params.gamma, 1.0f, 3.0f);

        if (ImGui::Button("Reset Curve")) {
            auto reset_curve = [](std::vector<Point>& points){
                points.clear();
                points.push_back({0.0f, 0.0f});
                points.push_back({1.0f, 1.0f});
            };
            reset_curve(state.params.curve_points_luma);
            reset_curve(state.params.curve_points_r);
            reset_curve(state.params.curve_points_g);
            reset_curve(state.params.curve_points_b);
            pipeline_changed = true;
        }
    }

    if (pipeline_changed) {
        state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
    }

    ImGui::EndChild();
    ImGui::End();
}

static void RenderMainView(AppState& state) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("Main View", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    state.main_view_size = ImGui::GetContentRegionAvail();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 cursor_screen_pos = ImGui::GetCursorScreenPos();

    if (ImGui::IsWindowHovered()) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const float source_w = state.input_image.width() - 32;
            float fit_scale = std::min(state.main_view_size.x / source_w, state.main_view_size.y / (state.input_image.height() - 24));
            float one_to_one_zoom = 1.0f / fit_scale;
            state.zoom = one_to_one_zoom;

            ImVec2 mouse_pos_in_window = ImGui::GetMousePos() - cursor_screen_pos;
            state.pan_offset = mouse_pos_in_window - (state.main_view_size * 0.5f);
            state.pan_offset = state.pan_offset * -1.0f;

            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
        else if (io.MouseWheel != 0) {
            float old_zoom = state.zoom;
            state.zoom *= (io.MouseWheel > 0) ? 1.1f : (1.0f / 1.1f);

            ImVec2 mouse_pos_in_window = ImGui::GetMousePos() - cursor_screen_pos;
            state.pan_offset = mouse_pos_in_window + (state.pan_offset - mouse_pos_in_window) * (state.zoom / old_zoom);

            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            state.pan_offset = state.pan_offset + io.MouseDelta;
        }
    }

    if (state.main_texture_id != 0 && state.main_output_planar.data()) {
        const float source_w = state.input_image.width() - 32;
        const float source_h = state.input_image.height() - 24;

        float fit_scale_x = state.main_view_size.x / source_w;
        float fit_scale_y = state.main_view_size.y / source_h;
        float fit_scale = std::min(fit_scale_x, fit_scale_y);

        float img_w = source_w * fit_scale * state.zoom;
        float img_h = source_h * fit_scale * state.zoom;

        ImGui::SetCursorPos(state.pan_offset);
        ImGui::Image((void*)(intptr_t)state.main_texture_id, ImVec2(img_w, img_h), ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImVec2 center = cursor_screen_pos + state.main_view_size * 0.5f;
        ImGui::GetWindowDrawList()->AddText(center, IM_COL32(255,255,255,200), "Adjust a parameter to render the image.");
    }

    ImGui::End();
}

void RenderUI(AppState& state) {
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    ImGui::End();

    static bool first_time_docking = true;
    if (first_time_docking) {
        first_time_docking = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        ImGuiID main_id;
        ImGuiID right_id = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.25f, nullptr, &main_id);

        ImGuiDockNode* main_node = ImGui::DockBuilderGetNode(main_id);
        if(main_node) main_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

        ImGuiDockNode* right_node = ImGui::DockBuilderGetNode(right_id);
        if(right_node) right_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

        ImGui::DockBuilderDockWindow("Main View", main_id);
        ImGui::DockBuilderDockWindow("Adjustments", right_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    RenderMainView(state);
    RenderRightPanel(state);

    auto now = std::chrono::steady_clock::now();
    if (state.ui_ready && now >= state.next_render_time) {
        std::cout << "Debounce triggered: Rerunning Halide pipeline..." << std::endl;
        RunHalidePipelines(state);
        if (state.main_output_planar.data()) {
            CreateOrUpdateTexture(state.main_texture_id, state.main_output_planar.width(), state.main_output_planar.height(), state.main_output_interleaved);
            CreateOrUpdateTexture(state.thumb_texture_id, state.thumb_output_planar.width(), state.thumb_output_planar.height(), state.thumb_output_interleaved);
        }
        state.next_render_time = std::chrono::steady_clock::time_point::max();
    }

    if (!state.ui_ready && state.main_view_size.x > 1 && state.main_view_size.y > 1) {
        state.ui_ready = true;

        const float source_w = state.input_image.width() - 32;
        const float source_h = state.input_image.height() - 24;
        float fit_scale_x = state.main_view_size.x / source_w;
        float fit_scale_y = state.main_view_size.y / source_h;
        float fit_scale = std::min(fit_scale_x, fit_scale_y);

        state.zoom = 1.0f;
        state.pan_offset.x = (state.main_view_size.x - source_w * fit_scale) * 0.5f;
        state.pan_offset.y = (state.main_view_size.y - source_h * fit_scale) * 0.5f;

        state.next_render_time = std::chrono::steady_clock::now();
    }
}
