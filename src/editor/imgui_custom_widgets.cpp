#include "imgui_custom_widgets.h"
#include "imgui_internal.h"
#include <cstring>
#include <algorithm>

// Helper for case-insensitive substring search
static bool ImStrIStr(const char* haystack, const char* needle) {
    if (!needle) return true;
    if (!haystack) return false;
    for (; *haystack; ++haystack) {
        if (toupper((unsigned char)*haystack) == toupper((unsigned char)*needle)) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && toupper((unsigned char)*h) == toupper((unsigned char)*n)) {
                h++;
                n++;
            }
            if (*n == 0) return true;
        }
    }
    return false;
}

namespace Widgets {

bool ComboWithFilter(const char* label, std::string& current_item, const std::vector<std::string>& all_items) {
    bool changed = false;

    // The ID of the combo is the label that comes after "##"
    const char* id = strstr(label, "##");
    ImGui::PushID(id ? id : label);

    // Display only the part of the label before "##"
    ImGui::TextUnformatted(label, id);
    ImGui::SameLine();

    float control_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(control_width);

    if (ImGui::BeginCombo("##combo", current_item.c_str())) {
        static char filter_buffer[256] = "";
        
        if (ImGui::IsWindowAppearing()) {
            filter_buffer[0] = '\0';
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##Filter", "Search...", filter_buffer, sizeof(filter_buffer));
        if (ImGui::IsWindowAppearing()) {
             ImGui::SetKeyboardFocusHere(-1);
        }

        ImGui::BeginChild("##ScrollingRegion", ImVec2(0, 200), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& item : all_items) {
            if (filter_buffer[0] == '\0' || ImStrIStr(item.c_str(), filter_buffer)) {
                bool is_selected = (item == current_item);
                if (ImGui::Selectable(item.c_str(), is_selected)) {
                    if (current_item != item) {
                        current_item = item;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    
    ImGui::PopID();
    return changed;
}

bool SliderFloatWithLabel(const char* label, float* v, float v_min, float v_max, const char* format) {
    const char* id = strstr(label, "##");
    ImGui::PushID(id ? id : label);
    
    ImGui::TextUnformatted(label, id);
    ImGui::SameLine();
    
    float slider_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(slider_width);
    bool changed = ImGui::SliderFloat("##slider", v, v_min, v_max, format);
    
    ImGui::PopID();
    return changed;
}

bool DragFloatWithLabel(const char* label, float* v, float v_speed, float v_min, float v_max, const char* format) {
    const char* id = strstr(label, "##");
    ImGui::PushID(id ? id : label);

    ImGui::TextUnformatted(label, id);
    ImGui::SameLine();

    float control_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(control_width);
    bool changed = ImGui::DragFloat("##drag", v, v_speed, v_min, v_max, format);

    ImGui::PopID();
    return changed;
}

} // namespace Widgets
