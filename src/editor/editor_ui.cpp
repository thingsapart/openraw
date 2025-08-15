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

// --- Helper Functions for Drawing ---

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
    ImGui::Begin("Adjustments");

    // --- Preview and Histogram (Fixed at the top) ---
    ImGui::Text("Preview");

    // Calculate thumbnail size to maintain aspect ratio
    if (state.input_image.data()) {
        const float raw_width = state.input_image.width() - 32;
        const float raw_height = state.input_image.height() - 24;
        if (raw_height > 0) {
            float aspect = raw_width / raw_height;
            float available_width = ImGui::GetContentRegionAvail().x;
            float thumb_height = available_width / aspect;
            state.thumb_view_size = ImVec2(available_width, thumb_height);

            ImGui::Image((void*)(intptr_t)state.thumb_texture_id, state.thumb_view_size, ImVec2(0,1), ImVec2(1,0));

            // --- Viewport Rectangle and Click-to-Pan Logic ---
            ImVec2 thumb_pos = ImGui::GetItemRectMin();
            ImVec2 thumb_max = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(thumb_pos);
            ImGui::InvisibleButton("##thumb_interact", state.thumb_view_size);

            if (ImGui::IsItemHovered()) {
                // Double-click to fit-to-view
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    state.zoom = 1.0f; // Reset zoom to 100% (fit)
                    // Recenter the view
                    float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
                    state.pan_offset.x = (state.main_view_size.x - raw_width * fit_scale) * 0.5f;
                    state.pan_offset.y = (state.main_view_size.y - raw_height * fit_scale) * 0.5f;
                    state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
                }
                // Click to pan
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImVec2 mouse_pos_in_thumb = ImGui::GetMousePos() - thumb_pos;
                    float norm_x = mouse_pos_in_thumb.x / state.thumb_view_size.x;
                    float norm_y = mouse_pos_in_thumb.y / state.thumb_view_size.y;
                    
                    float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
                    float zoomed_w = raw_width * fit_scale * state.zoom;
                    float zoomed_h = raw_height * fit_scale * state.zoom;

                    state.pan_offset.x = (state.main_view_size.x * 0.5f) - (norm_x * zoomed_w);
                    state.pan_offset.y = (state.main_view_size.y * 0.5f) - (norm_y * zoomed_h);
                }
            }

            // Draw the viewport rectangle
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
            float on_screen_w = raw_width * fit_scale * state.zoom;
            float on_screen_h = raw_height * fit_scale * state.zoom;
            ImVec2 view_pos_norm = ImVec2(-state.pan_offset.x / on_screen_w, -state.pan_offset.y / on_screen_h);
            ImVec2 view_size_norm = ImVec2(state.main_view_size.x / on_screen_w, state.main_view_size.y / on_screen_h);

            ImVec2 rect_min = thumb_pos + ImVec2(view_pos_norm.x, view_pos_norm.y) * state.thumb_view_size;
            ImVec2 rect_max = thumb_pos + ImVec2(view_pos_norm.x + view_size_norm.x, view_pos_norm.y + view_size_norm.y) * state.thumb_view_size;

            // This is the crucial fix: we enforce a clipping rectangle.
            draw_list->PushClipRect(thumb_pos, thumb_max, true);
            draw_list->AddRect(rect_min, rect_max, IM_COL32(255, 255, 255, 204)); // White, 80% opaque
            draw_list->PopClipRect();
        }
    }
    ImGui::Separator();
    
    // --- Histogram Rendering ---
    ImGui::Text("Histogram");
    float histo_height = 150.0f;
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.y = histo_height;
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_pos, canvas_pos + canvas_size, IM_COL32(20, 20, 22, 255));
    draw_list->AddRect(canvas_pos, canvas_pos + canvas_size, IM_COL32(80, 80, 80, 255));
    
    // Define material-style colors
    const ImU32 color_luma = IM_COL32(200, 200, 200, 255);
    const ImU32 color_r = IM_COL32(244, 67, 54, 255);
    const ImU32 color_g = IM_COL32(76, 175, 80, 255);
    const ImU32 color_b = IM_COL32(33, 150, 243, 255);

    if (state.show_luma_histo) DrawHistoCurve(draw_list, state.histogram_luma, canvas_pos, canvas_size, color_luma);
    if (state.show_r_histo) DrawHistoCurve(draw_list, state.histogram_r, canvas_pos, canvas_size, color_r);
    if (state.show_g_histo) DrawHistoCurve(draw_list, state.histogram_g, canvas_pos, canvas_size, color_g);
    if (state.show_b_histo) DrawHistoCurve(draw_list, state.histogram_b, canvas_pos, canvas_size, color_b);
    
    // --- Curve Editor Overlay ---
    if (state.show_curve_overlay) {
        static CurvesEditor curves_editor; // Static instance to preserve interaction state
        
        // Pass the linear UI-specific LUT to the editor for accurate visualization of the user's direct input.
        bool curve_changed = curves_editor.render(state.params.curve_points_global, state.ui_tone_curve_lut, canvas_pos, canvas_size);

        if (curve_changed) {
            // For instant UI feedback, we must regenerate the linear UI LUT immediately
            // instead of waiting for the debounced Halide run.
            ToneCurveUtils::generate_linear_lut(state.params, state.ui_tone_curve_lut);

            // Clear per-channel curves if we are editing the global one
            state.params.curve_points_r.clear();
            state.params.curve_points_g.clear();
            state.params.curve_points_b.clear();

            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
    }
    
    // The invisible button for the curves editor is inside its render function,
    // which handles advancing the cursor.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + histo_height);

    // Checkboxes
    ImGui::Checkbox("L", &state.show_luma_histo); ImGui::SameLine();
    ImGui::Checkbox("R", &state.show_r_histo); ImGui::SameLine();
    ImGui::Checkbox("G", &state.show_g_histo); ImGui::SameLine();
    ImGui::Checkbox("B", &state.show_b_histo); ImGui::SameLine();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    ImGui::Checkbox("Curve", &state.show_curve_overlay);

    ImGui::Separator();

    // --- Scrolling Child Region for Adjustments ---
    ImGui::BeginChild("##adjustments_scrolling", ImVec2(0, 0), true, ImGuiWindowFlags_None);

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
            state.params.curve_points_global.clear();
            state.params.curve_points_global.push_back({0.0f, 0.0f});
            state.params.curve_points_global.push_back({1.0f, 1.0f});
            state.params.curve_points_r.clear();
            state.params.curve_points_g.clear();
            state.params.curve_points_b.clear();
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
