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
        "MiddleMouseButton",
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
        OverlayUI::Tooltip(u8"瞄准生效的水平视野直径(检测像素)。\n"
                           u8"值越大越远的目标也能吸到,越小越只锁靠近准星的目标。\n"
                           u8"近战/喷子建议小,狙击/中远距离建议大。");
        changed |= ImGui::SliderInt("FOV Y", &hk.fovY, 10, 640);
        OverlayUI::Tooltip(u8"瞄准生效的垂直视野直径(检测像素)。一般略小于 FOV X 即可。");
        changed |= ImGui::Checkbox(u8"启用动态 FOV", &hk.dynamic_fov_enabled);
        OverlayUI::Tooltip(u8"开启后:有锁定目标时 FOV 会根据距离/目标大小自动收缩;\n"
                           u8"目标远 -> 用大 FOV(可换目标);目标近 -> 收紧到 bbox 周围,防止其他目标抢锁。");
        ImGui::BeginDisabled(!hk.dynamic_fov_enabled);
        changed |= ImGui::SliderFloat(u8"内边距系数", &hk.dynamic_fov_margin_frac, 1.00f, 2.00f, "%.2f");
        OverlayUI::Tooltip(u8"动态 FOV 收缩到目标 bbox 时的外扩比例。1.0 = 紧贴 bbox,1.5 = 比 bbox 大 50%。\n"
                           u8"值偏大可保留一点容错,偏小更专注当前目标。");
        changed |= ImGui::SliderFloat(u8"最小半径系数", &hk.dynamic_fov_min_radius_frac, 0.05f, 1.00f, "%.2f");
        OverlayUI::Tooltip(u8"动态 FOV 收缩的下限,占基础 FOV 的比例。\n"
                           u8"防止 FOV 缩到一点点导致目标稍稍偏移就脱锁。");
        ImGui::EndDisabled();
        if (hk.dynamic_fov_enabled)
        {
            const float rx = g_dynamic_fov_radius_x_px.load();
            const float ry = g_dynamic_fov_radius_y_px.load();
            ImGui::TextDisabled(u8"实时有效 FOV: %.0f x %.0f px", rx * 2.0f, ry * 2.0f);
        }
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"PID 瞄准"))
    {
        ImGui::TextDisabled(u8"cmd = P·err + I·∫err + D·d(err)/dt,误差→输出直接 lround 后送驱动。");
        ImGui::TextDisabled(u8"X / Y 轴各一套独立 P/I/D。P=跟枪力度,D=阻尼(抑制过冲/抖),I 一般保持 0。");
        ImGui::TextDisabled(u8"鲁棒处理(不完全微分/反算抗饱和/积分分离/微分限幅)全部内部自适应。");
        ImGui::Spacing();

        ImGui::SeparatorText(u8"X 轴(横向 / 跟随平移)");
        changed |= ImGui::SliderFloat(u8"P##pidx", &hk.pid_x_p, 0.0f, 5.0f, "%.3f");
        OverlayUI::Tooltip(u8"X 轴比例增益:横向跟枪力度。越大越快咬住目标,过大会过冲/抖。常用 0.4~1.0。");
        changed |= ImGui::SliderFloat(u8"I##pidx", &hk.pid_x_i, 0.0f, 2.0f, "%.3f");
        OverlayUI::Tooltip(u8"X 轴积分增益:消除横向常驻偏置。一般保持 0;长期差一点点固定偏移再小量加(0.02~0.1)。");
        changed |= ImGui::SliderFloat(u8"D##pidx", &hk.pid_x_d, 0.0f, 2.0f, "%.3f");
        OverlayUI::Tooltip(u8"X 轴微分增益:阻尼。抑制横向过冲与抖动。已内部加强低通,对检测噪声更钝、更线性好调;给一点点不再乱抖,常用 0.1~0.6。");

        ImGui::SeparatorText(u8"Y 轴(纵向 / 对抗后坐力)");
        changed |= ImGui::SliderFloat(u8"P##pidy", &hk.pid_y_p, 0.0f, 5.0f, "%.3f");
        OverlayUI::Tooltip(u8"Y 轴比例增益:纵向跟枪/压枪力度。打有后坐力的枪时通常比 X 略大一点。常用 0.4~1.2。");
        changed |= ImGui::SliderFloat(u8"I##pidy", &hk.pid_y_i, 0.0f, 2.0f, "%.3f");
        OverlayUI::Tooltip(u8"Y 轴积分增益:消除纵向常驻偏置(后坐力会造成持续向下的稳态误差)。需要稳定压枪时可小量加(0.02~0.15)。");
        changed |= ImGui::SliderFloat(u8"D##pidy", &hk.pid_y_d, 0.0f, 2.0f, "%.3f");
        OverlayUI::Tooltip(u8"Y 轴微分增益:阻尼。抑制纵向过冲与抖动。已内部加强低通,对检测噪声更钝、更线性好调;给一点点不再乱抖,常用 0.1~0.6。");

        ImGui::TextDisabled(u8"实时: err=%.1fpx", g_pid_last_err_px.load());

        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"卡尔曼预测(强跟踪 STF)"))
    {
        changed |= ImGui::Checkbox(u8"启用卡尔曼", &hk.kalman_enabled);
        OverlayUI::Tooltip(u8"开启后用强跟踪卡尔曼滤波(STF)平滑+预测目标位置:减少检测抖动导致的鼠标抽搐,并领先移动目标。\n"
                           u8"仍是单一卡尔曼,内置自适应渐消因子——目标急转/跳变时自动放大增益瞬间重捕,静止/匀速时退化为普通平滑。\n"
                           u8"只需调两个旋钮,机动自适应/延迟补偿/尺寸缩放全部内部自动。关闭则直接用每帧检测中心。");
        ImGui::BeginDisabled(!hk.kalman_enabled);
        changed |= ImGui::SliderFloat(u8"平滑度", &hk.kalman_smoothness, 0.0f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"卡尔曼对检测抖动的平滑程度(同时联动强跟踪的温和度)。\n"
                           u8"  越大(0.7~1.0)= 越平滑稳定(静止/小目标更稳),渐消更温和,反应略慢;\n"
                           u8"  越小(0.1~0.3)= 越紧跟检测、反应快,但检测抖会传一点到准星。\n"
                           u8"建议起步 0.5。注意:目标跳变/急转时强跟踪会自动短暂放大增益快速重捕,无需手动。");
        changed |= ImGui::SliderFloat(u8"预测提前量", &hk.kalman_lead, 0.0f, 2.0f, "%.2f");
        OverlayUI::Tooltip(u8"向目标运动方向提前多少。\n"
                           u8"  0   = 不预测(只平滑当前位置);\n"
                           u8"  1.0 = 物理正确的提前量(打移动/流水线目标更跟手);\n"
                           u8"  1.5~2.0 = 主动多领先,适合高速横移。\n"
                           u8"鼠标总在目标后面拉就调大,总冲到前面就调小。建议起步 1.0。");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"准星找色"))
    {
        changed |= ImGui::Checkbox(u8"启用准星找色 (此热键)", &hk.crosshair_detect_enabled);
        OverlayUI::Tooltip(u8"开启后,本热键瞄准时会用'准星找色'页定义的颜色作为参考点(取代画面中心);\n"
                           u8"适合'瞄具/狙镜抬到屏幕中央偏上'之类的非中心准星情况。颜色配置在'准星找色'面板。");

        changed |= ImGui::Checkbox(u8"启用镭射找色 (此热键)", &hk.laser_detect_enabled);
        OverlayUI::Tooltip(u8"独立于准星找色,可与其同时开启。两者都命中时'准星找色'优先,镭射末端仅作为\n"
                           u8"准星没找到时的补充参考点。镭射的颜色/取样框/参数在'准星找色'页的'镭射找色'区单独配置。");
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"目标切换迟滞"))
    {
        changed |= ImGui::SliderFloat(u8"切换分差阈值", &hk.lock_switch_score_margin, 0.0f, 200.0f, "%.2f");
        OverlayUI::Tooltip(u8"新目标必须比当前锁定目标的得分高出此分才会抢锁。\n"
                           u8"值大 = 锁更稳,不会随便切换;值 0 = 来一个比当前略好的就切。");
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
        OverlayUI::Tooltip(u8"开启后,目标越靠近(bbox 越大),Y 偏移会被自动拉回 0.5(几何中心)。\n"
                           u8"原因:同样 0.3 的 Y 偏移在远距离才几像素,近距离要几十像素 -> 抖。\n"
                           u8"用于解决'同一个 Y 偏移近战飘头、远距离稳头'的矛盾。");
        ImGui::BeginDisabled(!hk.y_offset_size_decay_enabled);
        changed |= ImGui::SliderFloat(u8"近距开始比例", &hk.y_offset_size_decay_low_frac, 0.0f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"目标 bbox 占画面比例超过这个值时,开始把 Y 偏移拉向中心。0.10 = bbox 占 10% 起拉。");
        changed |= ImGui::SliderFloat(u8"近距完全比例", &hk.y_offset_size_decay_high_frac, 0.0f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"目标 bbox 占画面比例达到这个值时,Y 偏移完全等于 0.5(中心);\n"
                           u8"应当大于'近距开始比例',中间是平滑过渡区间。");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"智能扳机"))
    {
        changed |= ImGui::Checkbox(u8"启用智能扳机", &hk.smart_trigger_enabled);
        OverlayUI::Tooltip(u8"几何扳机:准星落入锁定目标的命中框、并停留满'反应时间'后自动按下左键,\n"
                           u8"按住'开火时长'再松开,然后冷却。判定用目标的真实位置(非预测点),\n"
                           u8"只在按住瞄准热键时生效。硬件输出走当前设备(Win32/GHUB/Arduino/KMBOX/MAKCU)。");
        ImGui::BeginDisabled(!hk.smart_trigger_enabled);
        changed |= ImGui::SliderFloat(u8"横向命中比例", &hk.smart_trigger_hit_scale_x, 0.05f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"准星横向容差,按目标框半宽的比例。1.0=整框宽,越小越严格(只在更靠中心才开火)。");
        changed |= ImGui::SliderFloat(u8"纵向命中比例", &hk.smart_trigger_hit_scale_y, 0.05f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"准星纵向容差,按目标框半高的比例。瞄头时可调小;打身体可调大。");
        changed |= ImGui::SliderInt(u8"反应时间 (ms)", &hk.smart_trigger_reaction_ms, 0, 1000);
        OverlayUI::Tooltip(u8"准星必须在命中框内连续停留这么久才会触发第一枪(拟人延迟,也能过滤瞬间误判)。\n"
                           u8"0=瞬发;常用 20~80ms。");
        changed |= ImGui::SliderInt(u8"开火时长 (ms)", &hk.smart_trigger_hold_ms, 5, 5000);
        OverlayUI::Tooltip(u8"每次自动开火按住左键的时间。短(20~60ms)适合点射,长适合连狙/压枪。");
        changed |= ImGui::SliderInt(u8"冷却时间 (ms)", &hk.smart_trigger_cooldown_ms, 0, 5000);
        OverlayUI::Tooltip(u8"一枪松开后,下一枪之前强制等待的时间。控制连发节奏 / 占空比。\n"
                           u8"全自动可设小(30~60ms);半自动 / 狙击可设大。");
        ImGui::EndDisabled();
        if (hk.smart_trigger_enabled)
        {
            const bool ready = g_smart_trigger_ready.load();
            ImGui::TextColored(ready ? ImVec4(1.0f, 0.5f, 0.35f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                ready ? u8"正在开火" : u8"待机");
            ImGui::SameLine();
            ImGui::TextDisabled(u8"命中度=%.2f  停留=%.0fms",
                g_smart_trigger_hit_prob.load(),
                g_smart_trigger_recent_variance_px.load());
        }
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"威胁度优先级"))
    {
        changed |= ImGui::Checkbox(u8"启用威胁加权", &hk.threat_priority_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"威胁分 = 深度(越近越高) 与 头类别检测置信度 的加权混合,乘进锁定打分。");
        ImGui::BeginDisabled(!hk.threat_priority_enabled);
        changed |= ImGui::SliderFloat(u8"威胁权重", &hk.threat_weight, 0.0f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"威胁分对最终锁定打分的影响倍率。0=不参与,1=完全乘进去。\n"
                           u8"建议从 0.3~0.5 起,如果觉得乱抢锁就调小。");
        changed |= ImGui::SliderFloat(u8"深度↔头部 比例", &hk.threat_depth_head_ratio, 0.0f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"0=完全看深度(越近越优先),1=完全看头部识别度(越高越优先),中间线性混合。\n"
                           u8"未启用 depth_anything 时深度自动取中性 0.5;头类别未选时头部置信度自动取中性 0.5。");
        changed |= draw_threat_class_combo(u8"头类别", hk.threat_head_class_id, hk.aim_classes);
        OverlayUI::Tooltip(u8"哪个类别算'头'(只有该类别的检测置信度参与威胁分)。未选则头部置信度信号失效。");
        ImGui::EndDisabled();
        OverlayUI::EndSubsection();
    }

    if (OverlayUI::BeginSubsection(u8"近距离瞄头"))
    {
        changed |= ImGui::Checkbox(u8"启用近距离瞄头", &hk.close_range_head_aim_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"贴脸大目标(body 框)自动把瞄点吸附到框内上半区的 head 框,优先瞄头/上半身。\n只移动瞄点、不切换锁定目标。与上面的'Y 偏移距离衰减'(把大框拉向中心)方向相反,建议二选一。");
        ImGui::BeginDisabled(!hk.close_range_head_aim_enabled);
        changed |= draw_threat_class_combo(u8"头类别##closerange", hk.close_range_head_class_id, hk.aim_classes);
        OverlayUI::Tooltip(u8"哪个类别算'头'。未选则功能不生效。head 类别不必加入瞄准类别列表。");
        changed |= ImGui::SliderFloat(u8"触发框高比例", &hk.close_range_trigger_height_frac, 0.05f, 1.0f, "%.2f");
        OverlayUI::Tooltip(u8"目标框高 ÷ 检测画面高 ≥ 此值才触发吸附(即多近算'近距离')。越小越早触发。");
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

        // 每类别卡尔曼覆盖已移除(精简)。卡尔曼参数统一在热键级。

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

// 复制粘贴(cv)用的剪贴板。只复制"参数"部分,不复制名称/分组/按键/类别绑定 ——
// 这样把 A 热键的"手感参数"贴到 B 热键时不会破坏 B 的触发键和瞄准类别。
struct HotkeyClipboard
{
    bool valid = false;
    HotkeyProfile data;
};
HotkeyClipboard& hotkey_clipboard()
{
    static HotkeyClipboard cb;
    return cb;
}

void copy_params_into(HotkeyProfile& dst, const HotkeyProfile& src)
{
    std::string keep_name = std::move(dst.name);
    std::string keep_group = std::move(dst.group);
    std::vector<std::string> keep_keys = std::move(dst.keys);
    std::vector<HotkeyAimClass> keep_aim_classes = std::move(dst.aim_classes);

    dst = src;

    dst.name = std::move(keep_name);
    dst.group = std::move(keep_group);
    dst.keys = std::move(keep_keys);
    dst.aim_classes = std::move(keep_aim_classes);
}

// 按出现顺序收集所有用到的分组名(去重)。
std::vector<std::string> collect_groups_in_order(const std::vector<HotkeyProfile>& hotkeys)
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& h : hotkeys)
    {
        const std::string g = h.group.empty() ? std::string(u8"默认") : h.group;
        if (seen.insert(g).second)
            out.push_back(g);
    }
    return out;
}

int find_prev_in_group(const std::vector<HotkeyProfile>& hotkeys, size_t i, const std::string& group)
{
    for (int j = static_cast<int>(i) - 1; j >= 0; --j)
        if (hotkeys[j].group == group) return j;
    return -1;
}
int find_next_in_group(const std::vector<HotkeyProfile>& hotkeys, size_t i, const std::string& group)
{
    for (size_t j = i + 1; j < hotkeys.size(); ++j)
        if (hotkeys[j].group == group) return static_cast<int>(j);
    return -1;
}

// 把整个分组的所有热键作为一个块抽出,放回到指定位置(用于"分组上移/下移")。
void move_group_block_to(std::vector<HotkeyProfile>& hotkeys,
                         const std::string& group, size_t insert_at_after_extract)
{
    std::vector<HotkeyProfile> moved;
    for (size_t k = 0; k < hotkeys.size();)
    {
        if (hotkeys[k].group == group)
        {
            moved.push_back(std::move(hotkeys[k]));
            hotkeys.erase(hotkeys.begin() + k);
        }
        else ++k;
    }
    if (insert_at_after_extract > hotkeys.size())
        insert_at_after_extract = hotkeys.size();
    hotkeys.insert(hotkeys.begin() + insert_at_after_extract,
                   std::make_move_iterator(moved.begin()),
                   std::make_move_iterator(moved.end()));
}

} // namespace

void draw_hotkeys()
{
    bool any_changed = false;

    if (OverlayUI::BeginSection(u8"瞄准热键", "hotkeys_section_list"))
    {
        ImGui::TextWrapped(u8"可以创建多个瞄准热键并组织成分组(职业/武器/打法预设)。\n"
                           u8"按下任一热键时,按分组顺序逐个匹配,第一个键被按住的热键生效。\n"
                           u8"每个热键有独立的鼠标参数,可用'复制参数 / 粘贴参数'快速复用调好的手感。");
        ImGui::Separator();

        std::vector<int> aim_classes = collect_aim_bucket_classes();

        std::lock_guard<std::recursive_mutex> cfg(configMutex);

        // 兜底:任何空 group 字段都修成"默认",让分组扫描稳定。
        for (auto& h : config.hotkeys)
            if (h.group.empty()) h.group = u8"默认";

        const std::vector<std::string> groups = collect_groups_in_order(config.hotkeys);

        for (size_t gi = 0; gi < groups.size(); ++gi)
        {
            const std::string group_name = groups[gi];
            int count_in_group = 0;
            for (const auto& h : config.hotkeys)
                if (h.group == group_name) ++count_in_group;

            ImGui::PushID(("group::" + group_name).c_str());

            std::string section_title = std::string(u8"分组: ") + group_name
                                        + u8"  (" + std::to_string(count_in_group) + u8" 个热键)";
            if (OverlayUI::BeginSubsection(section_title.c_str()))
            {
                // ---- 分组操作行: 改名 / 上下移 / 删除整组 ----
                char gname_buf[96];
                std::snprintf(gname_buf, sizeof(gname_buf), "%s", group_name.c_str());
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::InputText(u8"分组名", gname_buf, sizeof(gname_buf)))
                {
                    std::string new_name = gname_buf;
                    if (new_name.empty()) new_name = u8"默认";
                    if (new_name != group_name)
                    {
                        for (auto& h : config.hotkeys)
                            if (h.group == group_name) h.group = new_name;
                        any_changed = true;
                    }
                }
                OverlayUI::Tooltip(u8"修改分组名,本组所有热键都会跟着迁移到新名字下。\n如果新名字和已存在的分组重名,两个分组会合并。");

                ImGui::SameLine();
                ImGui::BeginDisabled(gi == 0);
                if (ImGui::Button(u8"分组上移"))
                {
                    const std::string& prev_group = groups[gi - 1];
                    int first_prev = -1;
                    for (size_t k = 0; k < config.hotkeys.size(); ++k)
                        if (config.hotkeys[k].group == prev_group) { first_prev = (int)k; break; }
                    if (first_prev >= 0)
                    {
                        move_group_block_to(config.hotkeys, group_name, (size_t)first_prev);
                        any_changed = true;
                    }
                }
                OverlayUI::Tooltip(u8"把本组整体提到上一个分组之前。会改变运行时按下时的'优先匹配顺序'。");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(gi + 1 >= groups.size());
                if (ImGui::Button(u8"分组下移"))
                {
                    const std::string& next_group = groups[gi + 1];
                    // 取出本组后,下一组最后一个的位置 + 1 即为插入点。
                    move_group_block_to(config.hotkeys, group_name, config.hotkeys.size()); // 临时挪到末尾
                    int last_next = -1;
                    for (size_t k = 0; k < config.hotkeys.size(); ++k)
                        if (config.hotkeys[k].group == next_group) last_next = (int)k;
                    if (last_next >= 0)
                        move_group_block_to(config.hotkeys, group_name, (size_t)last_next + 1);
                    any_changed = true;
                }
                OverlayUI::Tooltip(u8"把本组整体放到下一个分组之后。");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 60, 60, 255));
                bool group_deleted = false;
                if (ImGui::Button(u8"删除整组"))
                {
                    for (size_t k = 0; k < config.hotkeys.size();)
                    {
                        if (config.hotkeys[k].group == group_name)
                            config.hotkeys.erase(config.hotkeys.begin() + k);
                        else ++k;
                    }
                    any_changed = true;
                    group_deleted = true;
                }
                OverlayUI::Tooltip(u8"删除本组以及组内所有热键(不可撤销)。");
                ImGui::PopStyleColor();

                if (group_deleted)
                {
                    OverlayUI::EndSubsection();
                    ImGui::PopID();
                    continue;
                }

                ImGui::Separator();

                // ---- 渲染本组中的每个热键 ----
                int local_no = 0;
                for (size_t i = 0; i < config.hotkeys.size();)
                {
                    if (config.hotkeys[i].group != group_name) { ++i; continue; }

                    auto& hk = config.hotkeys[i];
                    ++local_no;

                    ImGui::PushID(static_cast<int>(i));
                    std::string card_label = std::string(u8"热键 #") + std::to_string(local_no);
                    if (!hk.name.empty()) card_label += " - " + hk.name;

                    bool removed_here = false;
                    if (OverlayUI::BeginSubsection(card_label.c_str()))
                    {
                        // 名称
                        char name_buf[64];
                        std::snprintf(name_buf, sizeof(name_buf), "%s", hk.name.c_str());
                        if (ImGui::InputText(u8"名称", name_buf, sizeof(name_buf)))
                        {
                            hk.name = name_buf;
                            any_changed = true;
                        }
                        OverlayUI::Tooltip(u8"任意名字,只用于在 UI 里显示和区分。");

                        // 移动到其它分组
                        if (groups.size() > 1)
                        {
                            int cur = 0;
                            for (size_t k = 0; k < groups.size(); ++k)
                                if (groups[k] == hk.group) { cur = static_cast<int>(k); break; }
                            std::vector<const char*> labels;
                            labels.reserve(groups.size());
                            for (const auto& g : groups) labels.push_back(g.c_str());
                            ImGui::SetNextItemWidth(180.0f);
                            if (ImGui::Combo(u8"所属分组", &cur, labels.data(), static_cast<int>(labels.size())))
                            {
                                if (cur >= 0 && cur < (int)groups.size() && groups[cur] != hk.group)
                                {
                                    hk.group = groups[cur];
                                    any_changed = true;
                                }
                            }
                            OverlayUI::Tooltip(u8"切换此热键所属的分组,会立刻迁移到目标分组的末尾。\n要新建分组请到本面板底部'新建分组'按钮。");
                        }

                        ImGui::TextUnformatted(u8"按键(任一按下即触发)");
                        if (draw_key_list(hk.keys))
                            any_changed = true;

                        ImGui::Separator();
                        ImGui::TextUnformatted(u8"瞄准类别(按序决定优先级)");
                        if (draw_aim_class_list(hk, aim_classes))
                            any_changed = true;

                        ImGui::Separator();
                        if (draw_param_override_block(hk))
                            any_changed = true;

                        ImGui::Separator();

                        // ---- CV: 复制 / 粘贴参数 ----
                        if (ImGui::Button(u8"复制参数"))
                        {
                            hotkey_clipboard().data = hk;
                            hotkey_clipboard().valid = true;
                        }
                        OverlayUI::Tooltip(u8"把本热键的'参数'(FOV/速度/锁死/Kalman/瞄准曲线/智能扳机...)拷到剪贴板。\n名称、所属分组、按键、类别绑定不会被复制。");

                        ImGui::SameLine();
                        ImGui::BeginDisabled(!hotkey_clipboard().valid);
                        if (ImGui::Button(u8"粘贴参数"))
                        {
                            copy_params_into(hk, hotkey_clipboard().data);
                            any_changed = true;
                        }
                        OverlayUI::Tooltip(u8"把剪贴板里的参数贴到此热键,保留本热键的名称/分组/按键/类别绑定。\n用于把调好的手感快速复用到其它热键。");
                        ImGui::EndDisabled();

                        ImGui::SameLine();
                        ImGui::TextDisabled(hotkey_clipboard().valid ? u8"(剪贴板:有)" : u8"(剪贴板:空)");

                        ImGui::SameLine(0.0f, 16.0f);
                        const int prev_in_group = find_prev_in_group(config.hotkeys, i, group_name);
                        ImGui::BeginDisabled(prev_in_group < 0);
                        if (ImGui::Button(u8"上移"))
                        {
                            std::swap(config.hotkeys[(size_t)prev_in_group], config.hotkeys[i]);
                            any_changed = true;
                        }
                        OverlayUI::Tooltip(u8"在本组内上移一位(影响本组内的优先匹配顺序)。");
                        ImGui::EndDisabled();

                        ImGui::SameLine();
                        const int next_in_group = find_next_in_group(config.hotkeys, i, group_name);
                        ImGui::BeginDisabled(next_in_group < 0);
                        if (ImGui::Button(u8"下移"))
                        {
                            std::swap(config.hotkeys[(size_t)next_in_group], config.hotkeys[i]);
                            any_changed = true;
                        }
                        OverlayUI::Tooltip(u8"在本组内下移一位。");
                        ImGui::EndDisabled();

                        ImGui::SameLine(0.0f, 16.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 60, 60, 255));
                        if (ImGui::Button(u8"删除此热键"))
                        {
                            config.hotkeys.erase(config.hotkeys.begin() + i);
                            any_changed = true;
                            removed_here = true;
                        }
                        OverlayUI::Tooltip(u8"删除此热键(不可撤销)。");
                        ImGui::PopStyleColor();

                        OverlayUI::EndSubsection();
                    }

                    ImGui::PopID();
                    if (removed_here) continue;
                    ++i;
                }

                if (count_in_group == 0)
                    ImGui::TextDisabled(u8"本组暂无热键,点击下方按钮添加。");

                if (ImGui::Button(u8"在本组新建热键"))
                {
                    HotkeyProfile hk;
                    hk.name = "Aim " + std::to_string(config.hotkeys.size() + 1);
                    hk.group = group_name;
                    hk.keys = { "RightMouseButton" };
                    int last_idx = -1;
                    for (size_t k = 0; k < config.hotkeys.size(); ++k)
                        if (config.hotkeys[k].group == group_name) last_idx = (int)k;
                    if (last_idx < 0)
                        config.hotkeys.push_back(std::move(hk));
                    else
                        config.hotkeys.insert(config.hotkeys.begin() + last_idx + 1, std::move(hk));
                    any_changed = true;
                }
                OverlayUI::Tooltip(u8"在本组末尾追加一个新热键(默认右键触发)。");

                OverlayUI::EndSubsection();
            }
            ImGui::PopID();
        }

        if (config.hotkeys.empty())
            ImGui::TextDisabled(u8"尚未配置任何瞄准热键,可用下方按钮新建分组并添加热键。");

        ImGui::Separator();

        // ---- 新建一个全新分组 ----
        static char s_new_group_name[64] = "";
        if (s_new_group_name[0] == '\0')
        {
            std::string proposed = u8"分组" + std::to_string(groups.size() + 1);
            std::snprintf(s_new_group_name, sizeof(s_new_group_name), "%s", proposed.c_str());
        }
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText(u8"新分组名##new_group_name", s_new_group_name, sizeof(s_new_group_name));
        OverlayUI::Tooltip(u8"输入一个新分组名,然后点'新建分组'。新分组里会自动放一个空白热键作为初始成员。");
        ImGui::SameLine();
        if (ImGui::Button(u8"新建分组"))
        {
            std::string new_g = s_new_group_name;
            if (new_g.empty())
                new_g = u8"分组" + std::to_string(groups.size() + 1);
            bool exists = false;
            for (const auto& g : groups) if (g == new_g) { exists = true; break; }
            if (!exists)
            {
                HotkeyProfile hk;
                hk.name = "Aim " + std::to_string(config.hotkeys.size() + 1);
                hk.group = new_g;
                hk.keys = { "RightMouseButton" };
                config.hotkeys.push_back(std::move(hk));
                any_changed = true;
                std::string next = u8"分组" + std::to_string(groups.size() + 2);
                std::snprintf(s_new_group_name, sizeof(s_new_group_name), "%s", next.c_str());
            }
        }

        OverlayUI::EndSection();
    }

    if (any_changed)
        OverlayConfig_MarkDirty();
}
