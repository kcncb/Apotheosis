#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "draw_settings.h"
#include "Apotheosis.h"
#include "runtime/active_hotkey.h"

namespace
{

const std::vector<std::string>& aim_hotkey_allowed_keys()
{
    static const std::vector<std::string> kAllowed = {
        "None",
        "LeftMouseButton",
        "RightMouseButton",
        "X1MouseButton",
        "X2MouseButton",
    };
    return kAllowed;
}

const std::vector<const char*>& aim_hotkey_allowed_cstrs()
{
    static std::vector<const char*> kCstrs = [] {
        std::vector<const char*> v;
        for (const auto& s : aim_hotkey_allowed_keys())
            v.push_back(s.c_str());
        return v;
    }();
    return kCstrs;
}

int findKeyIndexByName(const std::string& keyName)
{
    const auto& list = aim_hotkey_allowed_keys();
    for (size_t k = 0; k < list.size(); ++k)
    {
        if (list[k] == keyName)
            return static_cast<int>(k);
    }
    return 0;
}

bool draw_key_list(std::vector<std::string>& bindings)
{
    const auto& names = aim_hotkey_allowed_keys();
    const auto& cstrs = aim_hotkey_allowed_cstrs();
    if (cstrs.empty())
    {
        ImGui::TextDisabled(u8"没有可用的按键列表。");
        return false;
    }

    bool changed = false;
    if (bindings.empty())
    {
        bindings.push_back("None");
        changed = true;
    }

    for (size_t i = 0; i < bindings.size();)
    {
        ImGui::PushID(static_cast<int>(i));

        int currentIndex = findKeyIndexByName(bindings[i]);
        const float rowAvail = ImGui::GetContentRegionAvail().x;
        const float btn = ImGui::GetFrameHeight();
        float comboW = rowAvail - (btn * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f);
        if (comboW < 80.0f) comboW = 80.0f;
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::Combo("##binding_combo", &currentIndex, cstrs.data(), static_cast<int>(cstrs.size())))
        {
            bindings[i] = names[currentIndex];
            changed = true;
        }

        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(btn, 0.0f)))
        {
            bindings.insert(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true;
        }

        ImGui::SameLine(0.0f, 3.0f);
        bool removed = false;
        if (ImGui::Button("-", ImVec2(btn, 0.0f)))
        {
            if (bindings.size() <= 1)
                bindings[0] = "None";
            else
            {
                bindings.erase(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removed = true;
            }
            changed = true;
        }

        ImGui::PopID();
        if (removed) continue;
        ++i;
    }

    return changed;
}

std::string class_display_name(int class_id)
{
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    for (const auto& cf : config.class_filters)
    {
        if (cf.class_id == class_id)
        {
            if (!cf.class_name.empty())
                return cf.class_name;
            break;
        }
    }
    return "class_" + std::to_string(class_id);
}

std::vector<int> collect_aim_bucket_classes()
{
    std::vector<int> out;
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    for (const auto& cf : config.class_filters)
    {
        if (cf.bucket == ClassBucket::Aim)
            out.push_back(cf.class_id);
    }
    return out;
}

bool draw_threat_class_combo(const char* label, int& selected_class_id, const std::vector<HotkeyAimClass>& aim_classes)
{
    std::vector<int> class_ids;
    class_ids.reserve(aim_classes.size() + 2);
    class_ids.push_back(-1);
    for (const auto& ac : aim_classes)
        class_ids.push_back(ac.class_id);

    if (selected_class_id >= 0
        && std::find(class_ids.begin(), class_ids.end(), selected_class_id) == class_ids.end())
    {
        class_ids.push_back(selected_class_id);
    }

    std::vector<std::string> display_names;
    display_names.reserve(class_ids.size());
    display_names.emplace_back(u8"未选 / -1");
    for (size_t i = 1; i < class_ids.size(); ++i)
        display_names.push_back(std::to_string(class_ids[i]) + ": " + class_display_name(class_ids[i]));

    std::vector<const char*> cstrs;
    cstrs.reserve(display_names.size());
    for (const auto& s : display_names)
        cstrs.push_back(s.c_str());

    int current_index = 0;
    for (size_t i = 1; i < class_ids.size(); ++i)
    {
        if (class_ids[i] == selected_class_id)
        {
            current_index = static_cast<int>(i);
            break;
        }
    }

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo(label, &current_index, cstrs.data(), static_cast<int>(cstrs.size())))
    {
        selected_class_id = class_ids[static_cast<size_t>(current_index)];
        return true;
    }
    return false;
}

bool draw_param_override_block(HotkeyProfile& hk)
{
    bool changed = false;

    if (OverlayUI::BeginSubsection(u8"视野 / FOV"))
    {
        changed |= ImGui::SliderInt("FOV X", &hk.fovX, 10, 160);
        changed |= ImGui::SliderInt("FOV Y", &hk.fovY, 10, 160);
        changed |= ImGui::Checkbox(u8"启用动态 FOV", &hk.dynamic_fov_enabled);
        ImGui::BeginDisabled(!hk.dynamic_fov_enabled);
        changed |= ImGui::SliderFloat(u8"内边距系数", &hk.dynamic_fov_margin_frac, 1.00f, 2.00f, "%.2f");
        changed |= ImGui::SliderFloat(u8"最小半径系数", &hk.dynamic_fov_min_radius_frac, 0.05f, 1.00f, "%.2f");
        ImGui::EndDisabled();
        if (hk.dynamic_fov_enabled)
        {
            const float rx = g_dynamic_fov_radius_x_px.load();
            const float ry = g_dynamic_fov_radius_y_px.load();
            ImGui::TextDisabled(u8"实时有效 FOV: %.0f x %.0f px", rx * 2.0f, ry * 2.0f);
        }
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection("PID"))
    {
        changed |= ImGui::SliderFloat("P X", &hk.pid_p_x, 0.0f, 5.0f, "%.3f");
        changed |= ImGui::SliderFloat("P Y", &hk.pid_p_y, 0.0f, 5.0f, "%.3f");
        hk.pid_p = (hk.pid_p_x + hk.pid_p_y) * 0.5f;
        changed |= ImGui::SliderFloat("I", &hk.pid_i, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::SliderFloat("D", &hk.pid_d, 0.0f, 2.0f, "%.3f");
        ImGui::Separator();
        changed |= ImGui::Checkbox(u8"启用 Flick / Track 双模式", &hk.flick_track_enabled);
        ImGui::BeginDisabled(!hk.flick_track_enabled);
        changed |= ImGui::SliderFloat("P X##trk", &hk.pid_track_p_x, 0.0f, 5.0f, "%.3f");
        changed |= ImGui::SliderFloat("P Y##trk", &hk.pid_track_p_y, 0.0f, 5.0f, "%.3f");
        hk.pid_track_p = (hk.pid_track_p_x + hk.pid_track_p_y) * 0.5f;
        changed |= ImGui::SliderFloat("I##trk", &hk.pid_track_i, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::SliderFloat("D##trk", &hk.pid_track_d, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"切换阈值 (px)", &hk.flick_track_threshold_px, 0.0f, 256.0f, "%.1f");
        changed |= ImGui::SliderFloat(u8"切换迟滞 (px)", &hk.flick_track_hysteresis_px, 0.0f, 64.0f, "%.1f");
        ImGui::EndDisabled();
        changed |= ImGui::SliderFloat(u8"吸附锁死力度", &hk.aim_lock_strength, 0.0f, 1.0f, "%.2f");
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection("Kalman"))
    {
        changed |= ImGui::Checkbox(u8"启用 Kalman", &hk.kalman_enabled);
        ImGui::BeginDisabled(!hk.kalman_enabled);
        changed |= ImGui::SliderFloat(u8"过程噪声 (位置)", &hk.kalman_process_noise_position, 0.001f, 5000.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"过程噪声 (速度)", &hk.kalman_process_noise_velocity, 0.001f, 50000.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"测量噪声", &hk.kalman_measurement_noise, 0.001f, 5000.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"速度阻尼", &hk.kalman_velocity_damping, 0.0f, 3.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"最大速度", &hk.kalman_max_velocity, 100.0f, 60000.0f, "%.0f");
        changed |= ImGui::SliderInt(u8"预热帧数", &hk.kalman_warmup_frames, 0, 20);
        changed |= ImGui::Checkbox(u8"补偿检测延迟", &hk.kalman_compensate_detection_delay);
        changed |= ImGui::SliderFloat(u8"额外预测 (ms)", &hk.kalman_additional_prediction_ms, -50.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat(u8"重置超时 (s)", &hk.kalman_reset_timeout_sec, 0.0f, 5.0f, "%.2f");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"准星找色"))
    {
        changed |= ImGui::Checkbox(u8"启用准星找色 (此热键)", &hk.crosshair_detect_enabled);
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"目标切换迟滞"))
    {
        changed |= ImGui::SliderFloat(u8"切换分差阈值", &hk.lock_switch_score_margin, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderInt(u8"最少连续帧", &hk.lock_switch_min_frames, 0, 20);
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"Y 偏移距离衰减"))
    {
        changed |= ImGui::Checkbox(u8"启用 Y 偏移距离衰减", &hk.y_offset_size_decay_enabled);
        ImGui::BeginDisabled(!hk.y_offset_size_decay_enabled);
        changed |= ImGui::SliderFloat(u8"近距开始比例", &hk.y_offset_size_decay_low_frac, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat(u8"近距完全比例", &hk.y_offset_size_decay_high_frac, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"智能扳机"))
    {
        changed |= ImGui::Checkbox(u8"启用智能扳机", &hk.smart_trigger_enabled);
        ImGui::BeginDisabled(!hk.smart_trigger_enabled);
        changed |= ImGui::SliderFloat(u8"命中半径比例", &hk.smart_trigger_hit_radius_frac, 0.05f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat(u8"最近 N 帧位置 RMS 上限 (px)", &hk.smart_trigger_variance_max_px, 0.0f, 50.0f, "%.1f");
        changed |= ImGui::SliderInt(u8"采样窗口 (帧)", &hk.smart_trigger_window_frames, 2, 60);
        changed |= ImGui::SliderFloat(u8"最低命中概率", &hk.smart_trigger_min_prob, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderInt(u8"开火持续时间 (ms)", &hk.smart_trigger_fire_duration_ms, 5, 500);
        ImGui::EndDisabled();
        if (hk.smart_trigger_enabled)
        {
            const bool ready = g_smart_trigger_ready.load();
            ImGui::TextColored(ready ? ImVec4(1.0f, 0.5f, 0.35f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                ready ? u8"正在开火" : u8"待机");
            ImGui::SameLine();
            ImGui::TextDisabled("prob=%.2f  rms=%.2fpx  hold=%dms",
                g_smart_trigger_hit_prob.load(),
                g_smart_trigger_recent_variance_px.load(),
                hk.smart_trigger_fire_duration_ms);
        }
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"威胁度优先级"))
    {
        changed |= ImGui::Checkbox(u8"启用威胁加权", &hk.threat_priority_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"启发式：类别档位 + 朝准星运动 + 历史命中帧数形成威胁分，并乘进锁定打分里。");
        ImGui::BeginDisabled(!hk.threat_priority_enabled);
        changed |= ImGui::SliderFloat(u8"威胁权重", &hk.threat_weight, 0.0f, 1.0f, "%.2f");
        changed |= draw_threat_class_combo(u8"头类别", hk.threat_head_class_id, hk.aim_classes);
        changed |= draw_threat_class_combo(u8"身子类别", hk.threat_body_class_id, hk.aim_classes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"头类别命中给最高威胁分，身子类别给中档，其它给低档；背对逃跑但靠近准星的目标仍会被认定为高威胁，这是可接受行为。");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    return changed;
}

bool draw_aim_class_list(HotkeyProfile& hk, const std::vector<int>& available_aim_classes)
{
    bool changed = false;

    std::unordered_set<int> used;
    for (const auto& c : hk.aim_classes)
        used.insert(c.class_id);

    for (size_t i = 0; i < hk.aim_classes.size();)
    {
        ImGui::PushID(static_cast<int>(i));

        const float rowAvail = ImGui::GetContentRegionAvail().x;
        const float btn = ImGui::GetFrameHeight();
        const float nameW = std::max(120.0f, rowAvail - btn * 3.0f - 140.0f - ImGui::GetStyle().ItemSpacing.x * 4.0f);

        ImGui::AlignTextToFramePadding();
        ImGui::Text("#%d", static_cast<int>(i + 1));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(nameW);
        const std::string label = class_display_name(hk.aim_classes[i].class_id);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();

        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat("Y##yoff", &hk.aim_classes[i].y_offset, 0.0f, 1.0f, "%.2f"))
            changed = true;
        ImGui::SameLine();
        if (ImGui::Button(u8"上", ImVec2(btn, 0.0f)) && i > 0)
        {
            std::swap(hk.aim_classes[i - 1], hk.aim_classes[i]);
            changed = true;
        }
        ImGui::SameLine(0.0f, 3.0f);
        if (ImGui::Button(u8"下", ImVec2(btn, 0.0f)) && i + 1 < hk.aim_classes.size())
        {
            std::swap(hk.aim_classes[i + 1], hk.aim_classes[i]);
            changed = true;
        }
        ImGui::SameLine(0.0f, 3.0f);
        bool removed = false;
        if (ImGui::Button("x", ImVec2(btn, 0.0f)))
        {
            hk.aim_classes.erase(hk.aim_classes.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            removed = true;
        }

        if (!removed)
        {
            auto& ac = hk.aim_classes[i];
            const std::string kalman_label = std::string(u8"Kalman 覆盖 ##k") + std::to_string(i);
            if (ImGui::TreeNode(kalman_label.c_str()))
            {
                changed |= ImGui::Checkbox(u8"启用此类别的 Kalman 覆盖", &ac.kalman_override_enabled);
                ImGui::BeginDisabled(!ac.kalman_override_enabled);
                changed |= ImGui::SliderFloat(u8"过程噪声 (位置)", &ac.kalman_process_noise_position, 0.001f, 5000.0f, "%.3f");
                changed |= ImGui::SliderFloat(u8"过程噪声 (速度)", &ac.kalman_process_noise_velocity, 0.001f, 50000.0f, "%.3f");
                changed |= ImGui::SliderFloat(u8"测量噪声", &ac.kalman_measurement_noise, 0.001f, 5000.0f, "%.3f");
                changed |= ImGui::SliderFloat(u8"速度阻尼", &ac.kalman_velocity_damping, 0.0f, 3.0f, "%.3f");
                changed |= ImGui::SliderFloat(u8"最大速度", &ac.kalman_max_velocity, 100.0f, 60000.0f, "%.0f");
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
        }

        ImGui::PopID();
        if (removed) continue;
        ++i;
    }

    std::vector<int> addable;
    addable.reserve(available_aim_classes.size());
    for (int id : available_aim_classes)
    {
        if (used.find(id) == used.end())
            addable.push_back(id);
    }

    if (addable.empty())
    {
        ImGui::TextDisabled(u8"没有可添加的\"瞄准\"类别（请先在目标面板把类别设为瞄准）。");
    }
    else
    {
        static int s_pick_index = 0;
        if (s_pick_index >= static_cast<int>(addable.size()))
            s_pick_index = 0;

        std::vector<std::string> display_names;
        display_names.reserve(addable.size());
        for (int id : addable)
            display_names.push_back(std::to_string(id) + ": " + class_display_name(id));

        std::vector<const char*> cstrs;
        cstrs.reserve(display_names.size());
        for (const auto& s : display_names)
            cstrs.push_back(s.c_str());

        ImGui::SetNextItemWidth(240.0f);
        ImGui::Combo("##hk_add_class", &s_pick_index, cstrs.data(), static_cast<int>(cstrs.size()));
        ImGui::SameLine();
        if (ImGui::Button(u8"添加类别"))
        {
            HotkeyAimClass ac;
            ac.class_id = addable[s_pick_index];
            ac.y_offset = 0.5f;
            hk.aim_classes.push_back(ac);
            changed = true;
        }
    }

    return changed;
}

} // namespace

void draw_hotkeys()
{
    bool any_changed = false;

    if (OverlayUI::BeginSection(u8"瞄准热键", "hotkeys_section_list"))
    {
        ImGui::TextWrapped(u8"可以创建多个瞄准热键；按下时第一个匹配的生效。每个热键可以单独选择瞄准类别、Y 偏移，以及覆盖鼠标参数。");
        ImGui::Separator();

        std::vector<int> aim_classes = collect_aim_bucket_classes();

        std::lock_guard<std::recursive_mutex> cfg(configMutex);

        for (size_t i = 0; i < config.hotkeys.size();)
        {
            auto& hk = config.hotkeys[i];
            ImGui::PushID(static_cast<int>(i));

            std::string section_label = std::string(u8"热键 #") + std::to_string(i + 1);
            if (!hk.name.empty())
                section_label += " - " + hk.name;

            if (OverlayUI::BeginSubsection(section_label.c_str()))
            {
                char name_buf[64];
                std::snprintf(name_buf, sizeof(name_buf), "%s", hk.name.c_str());
                if (ImGui::InputText(u8"名称", name_buf, sizeof(name_buf)))
                {
                    hk.name = name_buf;
                    any_changed = true;
                }

                ImGui::TextUnformatted(u8"按键（任一按下即触发）");
                if (draw_key_list(hk.keys))
                    any_changed = true;

                ImGui::Separator();
                ImGui::TextUnformatted(u8"瞄准类别（按序决定优先级）");
                if (draw_aim_class_list(hk, aim_classes))
                    any_changed = true;

                ImGui::Separator();
                if (draw_param_override_block(hk))
                    any_changed = true;

                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 60, 60, 255));
                if (ImGui::Button(u8"删除此热键"))
                {
                    config.hotkeys.erase(config.hotkeys.begin() + static_cast<std::ptrdiff_t>(i));
                    any_changed = true;
                    ImGui::PopStyleColor();
                    OverlayUI::EndSubsection();
                    ImGui::PopID();
                    continue;
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button(u8"上移") && i > 0)
                {
                    std::swap(config.hotkeys[i - 1], config.hotkeys[i]);
                    any_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(u8"下移") && i + 1 < config.hotkeys.size())
                {
                    std::swap(config.hotkeys[i + 1], config.hotkeys[i]);
                    any_changed = true;
                }

                OverlayUI::EndSubsection();
            }

            ImGui::PopID();
            ++i;
        }

        if (config.hotkeys.empty())
            ImGui::TextDisabled(u8"尚未配置任何瞄准热键。");

        ImGui::Separator();
        if (ImGui::Button(u8"新建热键"))
        {
            HotkeyProfile hk;
            hk.name = "Aim " + std::to_string(config.hotkeys.size() + 1);
            hk.keys = { "RightMouseButton" };
            config.hotkeys.push_back(std::move(hk));
            any_changed = true;
        }

        OverlayUI::EndSection();
    }

    if (any_changed)
        OverlayConfig_MarkDirty();
}
