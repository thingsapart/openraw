#include "pane_lens_correction.h"
#include "../app_state.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <algorithm>
#include <string>
#include <map>
#include <set>
#include <chrono>

// To enable Lensfun support, compile with -DUSE_LENSFUN
#ifdef USE_LENSFUN
// The main lensfun.h header is included via app_state.h
#endif

namespace Panes {

namespace { // Anonymous namespace for local helpers and state

// --- State for the error blink effect ---
static std::chrono::steady_clock::time_point s_feedback_time;
static std::set<std::string> s_invalid_fields;
static std::string s_feedback_message;
static ImVec4 s_feedback_color;
static constexpr std::chrono::milliseconds FEEDBACK_DURATION{3000};


// A helper for creating a searchable dropdown combo box.
bool SearchableCombo(const char* label, std::string& current_item, const std::vector<std::string>& items, const char* hint) {
    bool changed = false;
    static std::map<std::string, std::string> filters;
    
    if (filters.find(label) == filters.end()) {
        filters[label] = "";
    }
    std::string& filter = filters[label];

    if (ImGui::BeginCombo(label, current_item.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        
        char filter_buf[256];
        strncpy(filter_buf, filter.c_str(), sizeof(filter_buf) - 1);
        filter_buf[sizeof(filter_buf)-1] = '\0';

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##filter", hint, filter_buf, sizeof(filter_buf))) {
            filter = filter_buf;
        }

        int item_index = 0;
        for (const auto& item : items) {
            std::string item_lower = item;
            std::transform(item_lower.begin(), item_lower.end(), item_lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            std::string filter_lower = filter;
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            
            std::string selectable_id = item + "##" + std::to_string(item_index++);

            if (item_lower.find(filter_lower) != std::string::npos) {
                bool is_selected = (current_item == item);
                if (ImGui::Selectable(selectable_id.c_str(), is_selected)) {
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
        ImGui::EndCombo();
    }
    return changed;
}

// Helper to draw the red error outline around the last widget.
void DrawErrorOutline(const char* field_name) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_feedback_time);

    if (elapsed < FEEDBACK_DURATION && s_invalid_fields.count(field_name)) {
        float fade = 1.0f - (static_cast<float>(elapsed.count()) / FEEDBACK_DURATION.count());
        ImU32 color = IM_COL32(255, 0, 0, static_cast<int>(fade * 255));
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), color, 2.0f, 0, 1.5f);
    }
}

} // end anonymous namespace

bool render_lens_correction(AppState& state) {
    bool changed = false;
    ProcessConfig& params = state.params;

#ifdef USE_LENSFUN
    static std::vector<const lfCamera*> s_camera_model_objects;

    // --- Section: Lens Profile ---
    if (ImGui::CollapsingHeader("Lens Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!state.lensfun_db) {
            ImGui::TextDisabled("Lensfun database not loaded.");
        } else {
            // -- Camera Make --
            if (SearchableCombo("Make", params.camera_make, state.lensfun_camera_makes, "Filter makes...")) {
                s_camera_model_objects.clear();
                state.lensfun_camera_models.clear();
                params.camera_model = ""; params.lens_profile_name = "None";
                state.lensfun_lens_models.clear();
                
                const lfCamera* const* all_cams = lf_db_get_cameras(state.lensfun_db.get());
                if (all_cams) {
                    for (int i = 0; all_cams[i]; ++i) {
                        if (std::string(lf_mlstr_get(all_cams[i]->Maker)) == params.camera_make) {
                            s_camera_model_objects.push_back(all_cams[i]);
                            state.lensfun_camera_models.push_back(lf_mlstr_get(all_cams[i]->Model));
                        }
                    }
                    std::sort(state.lensfun_camera_models.begin(), state.lensfun_camera_models.end());
                }
            }
            DrawErrorOutline("Make");

            // -- Camera Model --
            if (SearchableCombo("Model", params.camera_model, state.lensfun_camera_models, "Filter models...")) {
                state.lensfun_lens_models.clear();
                state.lensfun_lens_models.push_back("None");
                params.lens_profile_name = "None";

                const lfCamera* selected_cam = nullptr;
                for(const auto& cam_obj : s_camera_model_objects) {
                    if (params.camera_model == lf_mlstr_get(cam_obj->Model)) {
                        selected_cam = cam_obj; break;
                    }
                }
                if (selected_cam) {
                    LfScopedPtr<const lfLens*> lenses((const lfLens**)lf_db_find_lenses_hd(state.lensfun_db.get(), selected_cam, nullptr, nullptr, 0));
                    if (lenses) {
                        for (int i = 0; lenses.get()[i]; ++i) {
                            state.lensfun_lens_models.push_back(lf_mlstr_get(lenses.get()[i]->Model));
                        }
                    }
                }
                std::sort(state.lensfun_lens_models.begin() + 1, state.lensfun_lens_models.end());
            }
            DrawErrorOutline("Model");

            // -- Lens Profile --
            SearchableCombo("Lens", params.lens_profile_name, state.lensfun_lens_models, "Filter lenses...");
            DrawErrorOutline("Lens");
            
            ImGui::SliderFloat("Focal Length", &params.focal_length, 1.0f, 600.0f, "%.1f mm", ImGuiSliderFlags_Logarithmic);

            // -- Apply Button and Feedback --
            if (ImGui::Button("Apply Profile", ImVec2(-1, 0))) {
                s_invalid_fields.clear();
                if (params.camera_make.empty()) s_invalid_fields.insert("Make");
                if (params.camera_model.empty()) s_invalid_fields.insert("Model");
                if (params.lens_profile_name.empty() || params.lens_profile_name == "None") s_invalid_fields.insert("Lens");

                if (!s_invalid_fields.empty()) {
                    s_feedback_time = std::chrono::steady_clock::now();
                    s_feedback_message = "Please select all fields.";
                    s_feedback_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                } else {
                    const lfCamera* selected_cam = nullptr;
                    for(const auto& cam_obj : s_camera_model_objects) {
                        if (params.camera_model == lf_mlstr_get(cam_obj->Model)) { selected_cam = cam_obj; break; }
                    }
                    if (selected_cam) {
                        LfScopedPtr<const lfLens*> lenses((const lfLens**)lf_db_find_lenses_hd(state.lensfun_db.get(), selected_cam, nullptr, params.lens_profile_name.c_str(), 0));
                        if (lenses && lenses.get()[0]) {
                            lfLensCalibDistortion dist_model;
                            if (lf_lens_interpolate_distortion(lenses.get()[0], params.focal_length, &dist_model)) {
                                if (dist_model.Model == LF_DIST_MODEL_POLY5) {
                                    params.dist_k1 = dist_model.Terms[0];
                                    params.dist_k2 = dist_model.Terms[1];
                                    params.dist_k3 = 0.0f; // POLY5 has no k3
                                    s_feedback_message = "POLY5 profile applied successfully.";
                                    s_feedback_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                                    changed = true;
                                } else {
                                    s_feedback_message = "Error: Profile uses an incompatible model (not POLY5).";
                                    s_feedback_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                                }
                            } else {
                                s_feedback_message = "Error: Could not get distortion for this focal length.";
                                s_feedback_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                            }
                        }
                    }
                    s_feedback_time = std::chrono::steady_clock::now();
                }
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_feedback_time);
            if (elapsed < FEEDBACK_DURATION && !s_feedback_message.empty()) {
                float fade = 1.0f - (static_cast<float>(elapsed.count()) / FEEDBACK_DURATION.count());
                ImGui::TextColored(ImVec4(s_feedback_color.x, s_feedback_color.y, s_feedback_color.z, fade), "%s", s_feedback_message.c_str());
            } else {
                ImGui::Text(""); // Reserve space
            }
        }
    }
#endif

    // --- Section: Manual Correction ---
    if (ImGui::CollapsingHeader("Manual Correction", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Distortion k1", &params.dist_k1, -0.2f, 0.2f, "%.4f");
        changed |= ImGui::SliderFloat("Distortion k2", &params.dist_k2, -0.2f, 0.2f, "%.4f");
        changed |= ImGui::SliderFloat("Distortion k3", &params.dist_k3, -0.2f, 0.2f, "%.4f");
        ImGui::Separator();
        changed |= ImGui::SliderFloat("CA Red/Cyan", &params.ca_red_cyan, -100.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("CA Blue/Yellow", &params.ca_blue_yellow, -100.0f, 100.0f, "%.1f");
    }

    // --- Section: Vignette ---
    if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Amount##vignette", &params.vignette_amount, -100.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Feather", &params.vignette_midpoint, 0.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Roundness", &params.vignette_roundness, 0.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Highlights", &params.vignette_highlights, 0.0f, 100.0f, "%.1f");
    }

    // --- Section: Geometry ---
    if (ImGui::CollapsingHeader("Geometry")) {
        changed |= ImGui::SliderFloat("Rotate", &params.geo_rotate, -45.0f, 45.0f, "%.2f °");
        changed |= ImGui::SliderFloat("Scale", &params.geo_scale, 50.0f, 150.0f, "%.1f %%");
        changed |= ImGui::SliderFloat("Aspect", &params.geo_aspect, 0.5f, 2.0f, "%.3f");
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Keystone V", &params.geo_keystone_v, -100.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Keystone H", &params.geo_keystone_h, -100.0f, 100.0f, "%.1f");
        ImGui::Separator();
        changed |= ImGui::DragFloat("Offset X", &params.geo_offset_x, 1.0f, -500.0f, 500.0f, "%.0f px");
        changed |= ImGui::DragFloat("Offset Y", &params.geo_offset_y, 1.0f, -500.0f, 500.0f, "%.0f px");
    }

    return changed;
}

} // namespace Panes
