#include "editor_ui.h"
#include "app_state.h"

#include "imgui.h"
#include "imgui_internal.h" // For DockBuilder

#include <string>

// This function now renders all right-hand panels inside a single, scrollable window.
static void RenderRightPanel(AppState& state) {
    ImGui::Begin("Adjustments");

    // --- Preview and Histogram (not collapsible) ---
    ImGui::Text("Preview");
    ImGui::Text("Full-image thumbnail placeholder."); // Replace with actual image later
    ImGui::Separator();
    
    ImGui::Text("Histogram");
    ImGui::Text("Histogram placeholder."); // Replace with actual histogram later
    ImGui::Separator();

    // --- Adjustment Groups (collapsible) ---
    if (ImGui::CollapsingHeader("Core Pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Dropdown for demosaic algorithm
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
        }

        ImGui::SliderFloat("Color Temp", &state.params.color_temp, 1500.0f, 15000.0f, "%.0f K");
        ImGui::SliderFloat("Tint", &state.params.tint, -1.0f, 1.0f);
        ImGui::SliderFloat("CA Correction", &state.params.ca_strength, 0.0f, 2.0f);
    }

    if (ImGui::CollapsingHeader("Denoise", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength", &state.params.denoise_strength, 0.0f, 100.0f);
        ImGui::SliderFloat("Epsilon", &state.params.denoise_eps, 0.0001f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
    }

    if (ImGui::CollapsingHeader("Local Adjustments", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Based on Laplacian Pyramid");
        ImGui::SliderFloat("Detail", &state.params.ll_detail, -100.0f, 100.0f);
        ImGui::SliderFloat("Clarity", &state.params.ll_clarity, -100.0f, 100.0f);
        ImGui::Separator();
        ImGui::SliderFloat("Shadows", &state.params.ll_shadows, -100.0f, 100.0f);
        ImGui::SliderFloat("Highlights", &state.params.ll_highlights, -100.0f, 100.0f);
        ImGui::Separator();
        ImGui::SliderFloat("Blacks", &state.params.ll_blacks, -100.0f, 100.0f);
        ImGui::SliderFloat("Whites", &state.params.ll_whites, -100.0f, 100.0f);
    }
    
    if (ImGui::CollapsingHeader("Tone & Curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Contrast", &state.params.contrast, 0.0f, 100.0f);
        ImGui::SliderFloat("Gamma", &state.params.gamma, 1.0f, 3.0f);
        ImGui::Text("Custom curve points will go here.");
    }

    ImGui::End();
}

// Helper to render the main dockspace which acts as a canvas for all other windows.
static void RenderMainContent(AppState& state) {
    // This function renders the main "background" view area.
    // By using ImGuiDockNodeFlags_PassthruCentralNode, the DockSpace window itself
    // becomes the drawing canvas for the central node.

    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
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

    // --- Main Image View Content ---
    // This content is rendered "behind" the docked windows, in the central node area.
    ImGuiIO& io = ImGui::GetIO();
    // We check if the main viewport background is hovered, which corresponds to the central node.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow | ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0) {
        state.zoom *= (io.MouseWheel > 0) ? 1.1f : (1.0f / 1.1f);
    }
    
    // Placeholder for the main image.
    ImVec2 center = viewport->GetCenter();
    ImGui::GetForegroundDrawList()->AddText(ImVec2(viewport->WorkPos.x + 20, viewport->WorkPos.y + 20), IM_COL32(255,255,255,200),
        "Image content will be rendered here.");
    std::string zoom_text = "Zoom: " + std::to_string(state.zoom) + "x (Use mouse wheel to zoom)";
    ImGui::GetForegroundDrawList()->AddText(ImVec2(viewport->WorkPos.x + 20, viewport->WorkPos.y + 40), IM_COL32(255,255,255,200),
        zoom_text.c_str());

    ImGui::End();
}

// Main UI rendering function
void RenderUI(AppState& state) {
    // --- Programmatic Docking for First Run ---
    static bool first_time_docking = true;
    if (first_time_docking) {
        first_time_docking = false;
        
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockBuilderRemoveNode(dockspace_id); 
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // Split the dockspace to create a right-hand column.
        // The main_id will be the central node that becomes the background.
        ImGuiID main_id; 
        ImGuiID right_id = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.25f, nullptr, &main_id);
        
        // Dock the single "Adjustments" window into the right-hand column.
        ImGui::DockBuilderDockWindow("Adjustments", right_id);
        
        // We don't dock a "Main View" window anymore. The central node (main_id)
        // is left empty to act as the passthrough background.
        
        ImGui::DockBuilderFinish(dockspace_id);
    }

    // Render the main background area and the dockspace itself.
    RenderMainContent(state);

    // Render the single, scrollable right-hand panel.
    RenderRightPanel(state);
}
