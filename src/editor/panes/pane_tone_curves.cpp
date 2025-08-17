#include "pane_tone_curves.h"
#include "../app_state.h"
#include "imgui.h"

namespace Panes {

bool render_tone_curves(AppState& state) {
    bool changed = false;

    changed |= ImGui::SliderFloat("Contrast", &state.params.contrast, 0.0f, 100.0f);
    changed |= ImGui::SliderFloat("Gamma", &state.params.gamma, 1.0f, 3.0f);

    if (ImGui::Button("Reset Curve")) {
        auto reset_curve = [](std::vector<Point>& points){
            points.clear();
            points.push_back({0.0f, 0.0f});
            points.push_back({1.0f, 1.0f});
        };
        reset_curve(state.params.curve_points_luma);
        reset_curve(state.params.curve_points_r);
        reset_curve(state.params.curve_points_g);
        reset_curve(state.params.curve_points_b);
        changed = true;
    }

    return changed;
}

} // namespace Panes
