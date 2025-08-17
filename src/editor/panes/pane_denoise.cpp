#include "pane_denoise.h"
#include "../app_state.h"
#include "imgui.h"

namespace Panes {

bool render_denoise(AppState& state) {
    bool changed = false;

    changed |= ImGui::SliderFloat("Strength", &state.params.denoise_strength, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Epsilon", &state.params.denoise_eps, 0.0001f, 0.0001f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);

    return changed;
}

} // namespace Panes
