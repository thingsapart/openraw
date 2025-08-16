#include "pane_manager.h"
#include "app_state.h"
#include "imgui.h"

void PaneManager::register_pane(const std::string& title, PaneRenderFunction render_func, bool is_static, bool default_open) {
    Pane pane = {title, std::move(render_func), default_open};
    if (is_static) {
        static_panes_.push_back(std::move(pane));
    } else {
        scrolling_panes_.push_back(std::move(pane));
    }
}

void PaneManager::render_all_panes(AppState& state) {
    ImGui::Begin("Adjustments");

    // --- Render Static (non-scrolling) Panes ---
    for (const auto& pane : static_panes_) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        bool is_open = ImGui::CollapsingHeader(pane.title.c_str(), pane.default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleVar();

        if (is_open) {
            pane.render_func(state);
        }
    }

    // --- Render Scrolling Panes in a Seamless Child Window ---
    // The child window has no border, a transparent background, and no internal padding.
    // This makes the CollapsingHeaders inside it appear to be part of the main window,
    // while allowing their content to scroll independently.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##scrolling_panes", ImVec2(0, 0), false, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    for (const auto& pane : scrolling_panes_) {
        // We add our own padding around the header to match the parent window's style.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        bool is_open = ImGui::CollapsingHeader(pane.title.c_str(), pane.default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleVar();

        if (is_open) {
            pane.render_func(state);
        }
    }

    ImGui::EndChild();
    ImGui::End();
}
