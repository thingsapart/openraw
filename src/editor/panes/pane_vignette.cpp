#include "pane_lens_correction.h"
#include "app_state.h"
#include "imgui.h"
#include "imgui_internal.h" // For access to GImGui context
#include <cmath> // for fabsf

namespace Panes {

// Helper for a standard two-column slider with a tooltip on the label part only.
static bool SliderFloatWithLabelTooltip(const char* label, const char* tooltip, float* v, float v_min, float v_max) {
    // This uses the default ImGui layout for sliders: [ Slider ] Label.
    // By not using PushItemWidth, we allow ImGui to calculate a standard width
    // that leaves room for the label, fixing the overflow issue.
    bool changed = ImGui::SliderFloat(label, v, v_min, v_max, "%.1f");

    // After drawing, we check if the mouse is hovering over the label part of the widget.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_RectOnly))
    {
        ImGuiContext& g = *GImGui;

        // The rect of the *entire* last item (slider + label).
        const ImRect total_rect = g.LastItemData.Rect;

        // The width of the interactive part (the slider bar) is determined by CalcItemWidth().
        const float slider_width = ImGui::CalcItemWidth();

        // The label starts after the slider and the inner item spacing.
        const float label_start_x = total_rect.Min.x + slider_width + g.Style.ItemInnerSpacing.x;

        // Define the bounding box for the label.
        const ImRect label_rect(ImVec2(label_start_x, total_rect.Min.y), total_rect.Max);

        // If the mouse is hovering over this specific rectangle, show the tooltip.
        if (ImGui::IsMouseHoveringRect(label_rect.Min, label_rect.Max))
        {
            ImGui::SetTooltip("%s", tooltip);
        }
    }
    return changed;
}

// THIS FILE IS DEPRECATED AND WILL BE REMOVED.
// The vignette controls have been moved into the new `pane_lens_correction.cpp` file.

} // namespace Panes
