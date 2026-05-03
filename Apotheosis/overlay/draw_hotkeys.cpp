#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
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
#include "capture/capture.h"
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
        changed |= ImGui::SliderInt("FOV X", &hk.fovX, 10, 640);
        changed |= ImGui::SliderInt("FOV Y", &hk.fovY, 10, 640);
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

    if (OverlayUI::BeginSubsection(u8"瞄准"))
    {
        changed |= ImGui::SliderFloat(u8"X 速度",   &hk.speed_x,       0.0f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"Y 速度",   &hk.speed_y,       0.0f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat(u8"锁死力度", &hk.lock_strength, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"= 0 时关闭动态锁死，纯 P 控制 (按文档先标定速度，再加锁死)。");
        changed |= ImGui::SliderFloat(u8"锁死范围 (px)", &hk.lock_radius_px, 4.0f, 80.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"动态锁死阈值 / 吸附半径 (检测像素)。文档建议 8-16 (吸附锁死) 或 40-65 (动态锁死)。\n值越小=吸附区越窄、越靠近准星才放大；越大=整张图都偏锁，开了力度更易抖。");
        ImGui::TextDisabled(u8"远距用速度，近距叠加锁死吸附；无 PID/无钳位");
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"瞄准曲线"))
    {
        // 模式
        int mode = static_cast<int>(hk.aim_trajectory_mode);
        const char* mode_items[] = { u8"直线 (Direct)", u8"贝塞尔轨迹 (Bezier)" };
        if (ImGui::Combo(u8"轨迹模式", &mode, mode_items, IM_ARRAYSIZE(mode_items)))
        {
            hk.aim_trajectory_mode = (mode == 1) ? AimTrajectoryMode::Bezier
                                                 : AimTrajectoryMode::Direct;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"Direct: 鼠标走直线靠近目标 (现有行为)。\n"
                              u8"Bezier: 鼠标按下方编辑器中的曲线轨迹靠近目标，更接近人手运动。");

        const bool bezier_on = (hk.aim_trajectory_mode == AimTrajectoryMode::Bezier);
        ImGui::BeginDisabled(!bezier_on);

        // 预设
        struct CurvePreset
        {
            const char* label;
            float cx1, cy1, cx2, cy2;
        };
        static const CurvePreset kPresets[] = {
            { u8"重置 (直线)",   0.33f,  0.00f, 0.66f,  0.00f },
            { u8"右弧",          0.30f,  0.30f, 0.70f,  0.20f },
            { u8"左弧",          0.30f, -0.30f, 0.70f, -0.20f },
            { u8"S 形 (右起)",   0.25f,  0.30f, 0.75f, -0.30f },
            { u8"Z 形 (左起)",   0.25f, -0.30f, 0.75f,  0.30f },
            { u8"先慢后快",      0.55f,  0.00f, 0.85f,  0.00f },
            { u8"先快后慢",      0.15f,  0.00f, 0.45f,  0.00f },
            { u8"高弧右抛",      0.20f,  0.55f, 0.55f,  0.40f },
            { u8"低弧左拐",      0.20f, -0.45f, 0.55f, -0.55f },
            { u8"轻微右偏",      0.30f,  0.10f, 0.70f,  0.05f },
            { u8"轻微左偏",      0.30f, -0.10f, 0.70f, -0.05f },
            { u8"过冲(略超调)",  0.40f,  0.00f, 0.95f,  0.20f },
        };
        ImGui::TextUnformatted(u8"曲线预设:");
        const int presets_per_row = 4;
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i)
        {
            if ((i % presets_per_row) != 0)
                ImGui::SameLine();
            if (ImGui::SmallButton(kPresets[i].label))
            {
                hk.bezier_cx1 = kPresets[i].cx1;
                hk.bezier_cy1 = kPresets[i].cy1;
                hk.bezier_cx2 = kPresets[i].cx2;
                hk.bezier_cy2 = kPresets[i].cy2;
                changed = true;
            }
        }

        // 编辑器画布
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 canvas_size(280.0f, 180.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##bezier_curve_canvas", canvas_size);
        const bool canvas_hovered = ImGui::IsItemHovered();
        const ImVec2 cmin = origin;
        const ImVec2 cmax = ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y);

        // 归一化坐标 → 屏幕坐标。x∈[0,1] 映射到画布宽,y∈[-1,1] 映射到画布高
        // (上 = +y,下 = -y)。
        auto normToScreen = [&](float nx, float ny) -> ImVec2
        {
            const float pad = 12.0f;
            const float w = canvas_size.x - 2.0f * pad;
            const float h = canvas_size.y - 2.0f * pad;
            const float cy = canvas_size.y * 0.5f;
            return ImVec2(cmin.x + pad + nx * w,
                          cmin.y + cy - ny * (h * 0.5f));
        };
        auto screenToNorm = [&](const ImVec2& s) -> ImVec2
        {
            const float pad = 12.0f;
            const float w = canvas_size.x - 2.0f * pad;
            const float h = canvas_size.y - 2.0f * pad;
            const float cy = canvas_size.y * 0.5f;
            return ImVec2((s.x - cmin.x - pad) / w,
                          (cmin.y + cy - s.y) / (h * 0.5f));
        };

        // 背景 + 边框 + 中线
        dl->AddRectFilled(cmin, cmax, IM_COL32(20, 20, 28, 200), 4.0f);
        dl->AddRect(cmin, cmax, IM_COL32(80, 80, 100, 200), 4.0f);
        const ImVec2 mid_l = ImVec2(cmin.x, (cmin.y + cmax.y) * 0.5f);
        const ImVec2 mid_r = ImVec2(cmax.x, (cmin.y + cmax.y) * 0.5f);
        dl->AddLine(mid_l, mid_r, IM_COL32(60, 60, 70, 200), 1.0f);

        // 起终点
        const ImVec2 p0 = normToScreen(0.0f, 0.0f);
        const ImVec2 p3 = normToScreen(1.0f, 0.0f);
        dl->AddCircleFilled(p0, 4.0f, IM_COL32(160, 160, 160, 255));
        dl->AddCircleFilled(p3, 4.0f, IM_COL32(160, 160, 160, 255));

        // 控制点 (可拖)
        ImVec2 p1 = normToScreen(hk.bezier_cx1, hk.bezier_cy1);
        ImVec2 p2 = normToScreen(hk.bezier_cx2, hk.bezier_cy2);

        // 拖拽: 鼠标按下时找到最近的可拖控制点 (P1/P2),拖动直至释放。
        static int drag_idx = -1; // 0=P1, 1=P2, -1=none
        static ImGuiID drag_owner = 0;
        const ImGuiID curve_id = ImGui::GetID("##bezier_curve_canvas");
        if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 m = ImGui::GetMousePos();
            const float d1 = std::hypot(m.x - p1.x, m.y - p1.y);
            const float d2 = std::hypot(m.x - p2.x, m.y - p2.y);
            const float pick = 16.0f;
            if (d1 <= pick && d1 <= d2)        { drag_idx = 0; drag_owner = curve_id; }
            else if (d2 <= pick)               { drag_idx = 1; drag_owner = curve_id; }
        }
        if (drag_idx >= 0 && drag_owner == curve_id)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                drag_idx = -1;
                drag_owner = 0;
            }
            else
            {
                const ImVec2 m = ImGui::GetMousePos();
                const ImVec2 n = screenToNorm(m);
                float nx = std::clamp(n.x, 0.0f, 1.0f);
                float ny = std::clamp(n.y, -1.0f, 1.0f);
                if (drag_idx == 0) { hk.bezier_cx1 = nx; hk.bezier_cy1 = ny; }
                else                { hk.bezier_cx2 = nx; hk.bezier_cy2 = ny; }
                p1 = normToScreen(hk.bezier_cx1, hk.bezier_cy1);
                p2 = normToScreen(hk.bezier_cx2, hk.bezier_cy2);
                changed = true;
            }
        }

        // 控制柄 (P0-P1, P3-P2 虚线)
        dl->AddLine(p0, p1, IM_COL32(120, 120, 120, 160), 1.0f);
        dl->AddLine(p3, p2, IM_COL32(120, 120, 120, 160), 1.0f);

        // 曲线本体
        dl->AddBezierCubic(p0, p1, p2, p3, IM_COL32(255, 165, 60, 255), 2.0f, 64);

        // 控制点圆点
        const ImU32 col_p1 = (drag_idx == 0) ? IM_COL32(120, 220, 120, 255)
                                             : IM_COL32(80, 200, 255, 255);
        const ImU32 col_p2 = (drag_idx == 1) ? IM_COL32(120, 220, 120, 255)
                                             : IM_COL32(255, 120, 200, 255);
        dl->AddCircleFilled(p1, 6.0f, col_p1);
        dl->AddCircleFilled(p2, 6.0f, col_p2);

        ImGui::TextDisabled(u8"X = 进度 (0=起点, 1=目标)。Y = 横向偏移 (单位=起终段长度)。");
        ImGui::TextDisabled(u8"拖动两个控制点塑形,或点上方预设按钮。");

        // 数值滑块 (备用,精确数值)
        if (ImGui::TreeNode(u8"数值微调"))
        {
            changed |= ImGui::SliderFloat("P1.x", &hk.bezier_cx1, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("P1.y", &hk.bezier_cy1, -1.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("P2.x", &hk.bezier_cx2, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("P2.y", &hk.bezier_cy2, -1.0f, 1.0f, "%.3f");
            ImGui::TreePop();
        }

        // 跟随 / 重锚
        changed |= ImGui::SliderFloat(u8"目标跟随系数", &hk.bezier_follow_alpha, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"锚定后曲线跟随最新目标的低通系数。\n"
                              u8"0 = 完全保形 (目标横移会脱靶),1 = 每帧跟到位 (退化为直线)。\n"
                              u8"默认 0.10~0.20 之间。");
        changed |= ImGui::SliderFloat(u8"重锚阈值 (px)", &hk.bezier_reanchor_threshold_px, 4.0f, 400.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"目标瞬移超过该阈值时强制重新锚定曲线起点。\n防止换目标 / 跳变时鼠标走出诡异轨迹。");

        ImGui::EndDisabled();
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
        changed |= ImGui::SliderFloat(u8"切换分差阈值", &hk.lock_switch_score_margin, 0.0f, 200.0f, "%.2f");
        changed |= ImGui::SliderInt(u8"最少连续帧", &hk.lock_switch_min_frames, 0, 4000);
        {
            const int fps = std::max(1, captureFps.load());
            const float ms_switch = 1000.0f * static_cast<float>(hk.lock_switch_min_frames) / static_cast<float>(fps);
            ImGui::SameLine();
            ImGui::TextDisabled(u8"\xe2\x89\x88 %.0f ms @ %d fps", ms_switch, fps);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"切换目标延迟 (文档建议 20-100 ms)。太小乱锁，太大不锁。");
        }
        changed |= ImGui::SliderInt(u8"锁定保持帧数", &hk.lock_hold_min_frames, 0, 2400);
        {
            const int fps = std::max(1, captureFps.load());
            const float ms_hold = 1000.0f * static_cast<float>(hk.lock_hold_min_frames) / static_cast<float>(fps);
            ImGui::SameLine();
            ImGui::TextDisabled(u8"\xe2\x89\x88 %.0f ms", ms_hold);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"锁定后强制保持目标的最少帧数。期间忽略所有切换条件,适合抑制抖枪 / 火焰造成的切换。0 关闭。");
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
