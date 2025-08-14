#include "editor_ui.h"
#include "app_state.h"
#include "halide_runner.h"
#include "texture_utils.h"

#include "imgui.h"
#include "imgui_internal.h" // For DockBuilder

#include <string>
#include <iostream>
#include <algorithm> // for std::max, std::min
#include <cmath>     // for fabsf, powf

// This function now renders all right-hand panels inside a single, scrollable window.
static void RenderRightPanel(AppState& state) {
    ImGui::Begin("Adjustments");

    // --- Preview and Histogram (not collapsible) ---
    ImGui::Text("Preview");

    // Calculate thumbnail size to maintain aspect ratio
    if (state.input_image.data()) {
        float raw_width = state.input_image.width() - 32;
        float raw_height = state.input_image.height() - 24;
        if (raw_height > 0) {
            float aspect = raw_width / raw_height;
            float available_width = ImGui::GetContentRegionAvail().x;
            float thumb_height = available_width / aspect;
            state.thumb_view_size = ImVec2(available_width, thumb_height);
            ImGui::ImageButton("##thumbnail", (void*)(intptr_t)state.thumb_texture_id, state.thumb_view_size, ImVec2(0,1), ImVec2(1,0));
        }
    }
    ImGui::Separator();
    
    ImGui::Text("Histogram");
    ImGui::Text("Histogram placeholder."); // Replace with actual histogram later
    ImGui::Separator();

    bool changed = false;

    // --- Adjustment Groups (collapsible) ---
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
            changed = true;
        }

        changed |= ImGui::SliderFloat("Color Temp", &state.params.color_temp, 1500.0f, 15000.0f, "%.0f K");
        changed |= ImGui::SliderFloat("Tint", &state.params.tint, -1.0f, 1.0f);
        changed |= ImGui::SliderFloat("CA Correction", &state.params.ca_strength, 0.0f, 2.0f);
    }

    if (ImGui::CollapsingHeader("Denoise", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Strength", &state.params.denoise_strength, 0.0f, 100.0f);
        changed |= ImGui::SliderFloat("Epsilon", &state.params.denoise_eps, 0.0001f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
    }

    if (ImGui::CollapsingHeader("Local Adjustments", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Based on Laplacian Pyramid");
        changed |= ImGui::SliderFloat("Detail", &state.params.ll_detail, -100.0f, 100.0f);
        changed |= ImGui::SliderFloat("Clarity", &state.params.ll_clarity, -100.0f, 100.0f);
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Shadows", &state.params.ll_shadows, -100.0f, 100.0f);
        changed |= ImGui::SliderFloat("Highlights", &state.params.ll_highlights, -100.0f, 100.0f);
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Blacks", &state.params.ll_blacks, -100.0f, 100.0f);
        changed |= ImGui::SliderFloat("Whites", &state.params.ll_whites, -100.0f, 100.0f);
    }
    
    if (ImGui::CollapsingHeader("Tone & Curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Contrast", &state.params.contrast, 0.0f, 100.0f);
        changed |= ImGui::SliderFloat("Gamma", &state.params.gamma, 1.0f, 3.0f);
        ImGui::Text("Custom curve points will go here.");
    }

    if (changed) {
        state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
    }

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
        if (io.MouseWheel != 0) {
            float old_zoom = state.zoom;
            // The zoom factor is now updated instantly for smooth UI scaling
            state.zoom *= (io.MouseWheel > 0) ? 1.1f : (1.0f / 1.1f);
            
            ImVec2 mouse_pos_in_window = ImGui::GetMousePos() - cursor_screen_pos;
            // Adjust the pan offset to keep the point under the mouse stationary
            state.pan_offset = mouse_pos_in_window + (state.pan_offset - mouse_pos_in_window) * (state.zoom / old_zoom);

            // Schedule a high-quality re-render for later
            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            // Direct "grab-a-pixel" panning
            state.pan_offset = state.pan_offset + io.MouseDelta;
        }
    }

    if (state.main_texture_id != 0 && state.main_output_planar.data()) {
        // The display size is the full source image size, scaled by the UI zoom
        const float source_w = state.input_image.width() - 32;
        const float source_h = state.input_image.height() - 24;

        // Calculate the base "fit-to-view" scale factor
        float fit_scale_x = state.main_view_size.x / source_w;
        float fit_scale_y = state.main_view_size.y / source_h;
        float fit_scale = std::min(fit_scale_x, fit_scale_y);

        // The final displayed size is the base fit size multiplied by the user's zoom
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
        
        // Calculate initial "fit-to-view" pan and zoom
        const float source_w = state.input_image.width() - 32;
        const float source_h = state.input_image.height() - 24;
        float fit_scale_x = state.main_view_size.x / source_w;
        float fit_scale_y = state.main_view_size.y / source_h;
        float fit_scale = std::min(fit_scale_x, fit_scale_y);
        
        // Zoom is a multiplier of this fit_scale, so it starts at 1.0
        state.zoom = 1.0f; 
        
        // Pan is calculated to center the scaled image
        state.pan_offset.x = (state.main_view_size.x - source_w * fit_scale) * 0.5f;
        state.pan_offset.y = (state.main_view_size.y - source_h * fit_scale) * 0.5f;

        state.next_render_time = std::chrono::steady_clock::now();
    }
}
