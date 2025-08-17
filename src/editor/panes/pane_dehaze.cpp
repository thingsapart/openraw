#include "pane_dehaze.h"
#include "../app_state.h"
#include "imgui.h"

namespace Panes {

bool render_dehaze(AppState& state) {
    bool changed = false;

    changed |= ImGui::SliderFloat("Strength##dehaze", &state.params.dehaze_strength, 0.0f, 100.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remove or add atmospheric haze.\nBased on the Color Attenuation Prior.");
    }

    return changed;
}

} // namespace Panes
