#include "editor_ui.h"
#include "app_state.h"
#include "halide_runner.h"
#include "texture_utils.h"

#include "imgui.h"
#include "imgui_internal.h" // For DockBuilder

#include <string>
#include <iostream>

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
    ImVec2 view_center_screen = cursor_screen_pos + state.main_view_size * 0.5f;

    const float source_image_w = state.input_image.width() - 32;
    const float source_image_h = state.input_image.height() - 24;

    // Calculate the scale factor from source image pixels to screen pixels
    float display_scale = state.main_view_size.x / (source_image_w * state.view_scale);

    if (ImGui::IsWindowHovered()) {
        if (io.MouseWheel != 0) {
            // 1. Where is the mouse in normalized image coordinates?
            ImVec2 mouse_pos_from_center_screen = ImGui::GetMousePos() - view_center_screen;
            ImVec2 mouse_pos_from_center_norm = mouse_pos_from_center_screen / state.main_view_size * state.view_scale;
            ImVec2 mouse_im_coords_norm = state.view_center_norm + mouse_pos_from_center_norm;

            // 2. Apply zoom to the view scale
            float zoom_delta = (io.MouseWheel > 0) ? 1.0f / 1.2f : 1.2f;
            float old_view_scale = state.view_scale;
            state.view_scale *= zoom_delta;
            state.view_scale = std::max(0.001f, std::min(state.view_scale, 2.0f)); // Clamp zoom

            // 3. Update the view center to keep the point under the mouse stationary
            state.view_center_norm = mouse_im_coords_norm + (state.view_center_norm - mouse_im_coords_norm) * (state.view_scale / old_view_scale);
            
            state.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            state.view_center_norm.x -= io.MouseDelta.x / (source_image_w * display_scale);
            state.view_center_norm.y -= io.MouseDelta.y / (source_image_h * display_scale);
        }
    }

    if (state.main_texture_id != 0 && state.main_output_planar.data()) {
        ImVec2 displayed_size = ImVec2(source_image_w * display_scale, source_image_h * display_scale);
        ImVec2 top_left_im_pos = ImVec2(source_image_w * state.view_center_norm.x, source_image_h * state.view_center_norm.y);
        ImVec2 top_left_screen_pos = view_center_screen - top_left_im_pos * display_scale;

        ImGui::SetCursorPos(top_left_screen_pos - cursor_screen_pos);
        ImGui::Image((void*)(intptr_t)state.main_texture_id, displayed_size, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImVec2 center_text_pos = cursor_screen_pos + state.main_view_size * 0.5f;
        ImGui::GetWindowDrawList()->AddText(center_text_pos, IM_COL32(255,255,255,200), "Adjust a parameter to render the image.");
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
        CreateOrUpdateTexture(state.main_texture_id, state.main_output_planar.width(), state.main_output_planar.height(), state.main_output_interleaved);
        CreateOrUpdateTexture(state.thumb_texture_id, state.thumb_output_planar.width(), state.thumb_output_planar.height(), state.thumb_output_interleaved);
        state.next_render_time = std::chrono::steady_clock::time_point::max();
    }

    if (!state.ui_ready && state.main_view_size.x > 1 && state.main_view_size.y > 1) {
        state.ui_ready = true;
        state.next_render_time = std::chrono::steady_clock::now();
    }
}
