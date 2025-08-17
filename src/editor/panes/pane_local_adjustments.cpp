#include "pane_local_adjustments.h"
#include "../app_state.h"
#include "imgui.h"

namespace Panes {

bool render_local_adjustments(AppState& state) {
    bool changed = false;

    changed |= ImGui::SliderFloat("Detail", &state.params.ll_detail, -100.0f, 100.0f);
    changed |= ImGui::SliderFloat("Clarity", &state.params.ll_clarity, -100.0f, 100.0f);
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Shadows", &state.params.ll_shadows, -100.0f, 100.0f);
    changed |= ImGui::SliderFloat("Highlights", &state.params.ll_highlights, -100.0f, 100.0f);
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Blacks", &state.params.ll_blacks, -100.0f, 100.0f);
    changed |= ImGui::SliderFloat("Whites", &state.params.ll_whites, -100.0f, 100.0f);

    return changed;
}

} // namespace Panes
