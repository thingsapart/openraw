#include "pane_core_pipeline.h"
#include "../app_state.h"
#include "imgui.h"

namespace Panes {

bool render_core_pipeline(AppState& state) {
    bool changed = false;

    const char* demosaic_items[] = { "fast", "ahd", "lmmse", "ri" };
    int current_item_idx = 0;
    for (int n = 0; n < IM_ARRAYSIZE(demosaic_items); n++) {
        if (state.params.demosaic_algorithm == demosaic_items[n]) {
            current_item_idx = n;
            break;
        }
    }
    if (ImGui::Combo("Demosaic", &current_item_idx, demosaic_items, IM_ARRAYSIZE(demosaic_items))) {
        state.params.demosaic_algorithm = demosaic_items[current_item_idx];
        changed = true;
    }

    changed |= ImGui::SliderFloat("Exposure", &state.params.exposure, -4.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Color Temp", &state.params.color_temp, 1500.0f, 15000.0f, "%.0f K");
    changed |= ImGui::SliderFloat("Tint", &state.params.tint, -1.0f, 1.0f);
    changed |= ImGui::SliderFloat("CA Correction", &state.params.ca_strength, 0.0f, 2.0f);

    return changed;
}

} // namespace Panes
