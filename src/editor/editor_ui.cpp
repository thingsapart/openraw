#include "editor_ui.h"
#include "app_state.h"
#include "halide_runner.h"
#include "texture_utils.h"
#include "pane_manager.h"

// Include all the individual pane headers
#include "panes/pane_preview.h"
#include "panes/pane_histogram_curves.h"
#include "panes/pane_core_pipeline.h"
#include "panes/pane_denoise.h"
#include "panes/pane_dehaze.h"
#include "panes/pane_lens_correction.h"
#include "panes/pane_local_adjustments.h"
#include "panes/pane_tone_curves.h"
#include "panes/pane_color_wheels.h"
#include "panes/pane_color_curves.h"

#include "imgui.h"
#include "imgui_internal.h" // For DockBuilder

#include <string>
#include <iostream>
#include <algorithm> // for std::max, std::min
#include <cmath>     // for fabsf, powf
#include <vector>    // for std::vector
#include <cstring>   // for memcpy

// This function now uses the PaneManager to render the right-side panel.
static void RenderRightPanel(PaneManager& pane_manager, AppState& state) {
    pane_manager.render_all_panes(state);
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
    // --- One-time Pane Registration ---
    static PaneManager pane_manager;
    static bool panes_registered = false;
    if (!panes_registered) {
        // Wrapper lambda to attach a debounce trigger to any pane function that returns 'true' on change.
        auto create_debounced_render_func = [](auto render_func) {
            return [render_func](AppState& s) {
                if (render_func(s)) {
                    s.next_render_time = std::chrono::steady_clock::now() + AppState::DEBOUNCE_DURATION;
                }
            };
        };

        // Static Panes (top, non-scrolling)
        pane_manager.register_pane("Preview", Panes::render_preview, true);
        pane_manager.register_pane("Histogram & Curves", create_debounced_render_func(Panes::render_histogram_curves), true);

        // Scrolling Panes
        pane_manager.register_pane("Core Pipeline", create_debounced_render_func(Panes::render_core_pipeline));
        pane_manager.register_pane("Denoise", create_debounced_render_func(Panes::render_denoise));
        pane_manager.register_pane("Dehaze", create_debounced_render_func(Panes::render_dehaze));
        pane_manager.register_pane("Lens Corrections", create_debounced_render_func(Panes::render_lens_correction), false, true);
        pane_manager.register_pane("Local Adjustments", create_debounced_render_func(Panes::render_local_adjustments));
        pane_manager.register_pane("Color Wheels", create_debounced_render_func(Panes::render_color_wheels), false, false);
        pane_manager.register_pane("Color Curves", create_debounced_render_func(Panes::render_color_curves), false, false);
        pane_manager.register_pane("Tone & Curve", create_debounced_render_func(Panes::render_tone_curves));

        panes_registered = true;
    }


    // --- Dockspace Setup ---
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

    // --- Render UI Components ---
    RenderMainView(state);
    RenderRightPanel(pane_manager, state);

    // --- Handle Debounced Pipeline Execution ---
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
