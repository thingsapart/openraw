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

bool render_vignette(AppState& state) {
    bool changed = false;

    changed |= SliderFloatWithLabelTooltip("Amount",
        "Controls the strength of the vignette.\n"
        "> 0: Brightens corners to correct vignetting.\n"
        "< 0: Darkens corners for a creative effect.",
        &state.params.vignette_amount, -100.0f, 100.0f);

    changed |= SliderFloatWithLabelTooltip("Feather",
        "Controls the falloff shape and how far the vignette reaches.\n"
        "The slider has a non-linear response to give more fine-grained control.\n"
        "Low values create a soft, gradual vignette that reaches into the center.\n"
        "High values create a hard-edged vignette confined to the corners.",
        &state.params.vignette_midpoint, 0.0f, 100.0f);

    changed |= SliderFloatWithLabelTooltip("Roundness",
        "Controls the shape of the vignette.\n"
        "0: Perfect circle.\n"
        "100: Ellipse matching the image aspect ratio.",
        &state.params.vignette_roundness, 0.0f, 100.0f);

    changed |= SliderFloatWithLabelTooltip("Highlights",
        "Preserves detail in bright areas when correcting vignetting.\n"
        "Prevents bright corners (e.g., sky) from blowing out.",
        &state.params.vignette_highlights, 0.0f, 100.0f);

    return changed;
}

} // namespace Panes
