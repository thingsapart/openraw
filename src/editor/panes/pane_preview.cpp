#include "pane_preview.h"
#include "../app_state.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm> // for std::min

namespace Panes {

bool render_preview(AppState& state) {
    bool changed = false;
    if (state.input_image.data()) {
        const float raw_width = state.input_image.width() - 32;
        const float raw_height = state.input_image.height() - 24;
        if (raw_height > 0) {
            float aspect = raw_width / raw_height;
            float available_width = ImGui::GetContentRegionAvail().x;
            float thumb_height = available_width / aspect;
            state.thumb_view_size = ImVec2(available_width, thumb_height);

            ImGui::Image((void*)(intptr_t)state.thumb_texture_id, state.thumb_view_size, ImVec2(0, 1), ImVec2(1, 0));

            ImVec2 thumb_pos = ImGui::GetItemRectMin();
            ImVec2 thumb_max = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(thumb_pos);
            ImGui::InvisibleButton("##thumb_interact", state.thumb_view_size);

            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    state.zoom = 1.0f;
                    float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
                    state.pan_offset.x = (state.main_view_size.x - raw_width * fit_scale) * 0.5f;
                    state.pan_offset.y = (state.main_view_size.y - raw_height * fit_scale) * 0.5f;
                    changed = true;
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImVec2 mouse_pos_in_thumb = ImGui::GetMousePos() - thumb_pos;
                    float norm_x = mouse_pos_in_thumb.x / state.thumb_view_size.x;
                    float norm_y = mouse_pos_in_thumb.y / state.thumb_view_size.y;
                    float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
                    float zoomed_w = raw_width * fit_scale * state.zoom;
                    float zoomed_h = raw_height * fit_scale * state.zoom;
                    state.pan_offset.x = (state.main_view_size.x * 0.5f) - (norm_x * zoomed_w);
                    state.pan_offset.y = (state.main_view_size.y * 0.5f) - (norm_y * zoomed_h);
                    changed = true;
                }
            }
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            float fit_scale = std::min(state.main_view_size.x / raw_width, state.main_view_size.y / raw_height);
            float on_screen_w = raw_width * fit_scale * state.zoom;
            float on_screen_h = raw_height * fit_scale * state.zoom;
            ImVec2 view_pos_norm = ImVec2(-state.pan_offset.x / on_screen_w, -state.pan_offset.y / on_screen_h);
            ImVec2 view_size_norm = ImVec2(state.main_view_size.x / on_screen_w, state.main_view_size.y / on_screen_h);
            ImVec2 rect_min = thumb_pos + ImVec2(view_pos_norm.x, view_pos_norm.y) * state.thumb_view_size;
            ImVec2 rect_max = thumb_pos + ImVec2(view_pos_norm.x + view_size_norm.x, view_pos_norm.y + view_size_norm.y) * state.thumb_view_size;

            draw_list->PushClipRect(thumb_pos, thumb_max, true);
            draw_list->AddRect(rect_min, rect_max, IM_COL32(255, 255, 255, 204));
            draw_list->PopClipRect();
        }
    }
    return changed;
}

} // namespace Panes
