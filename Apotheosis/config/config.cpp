#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "config.h"
#define SI_NO_CONVERSION
#include "modules/SimpleIni.h"

namespace
{

// ── 【2026-09-17 删除】曲线资产读写 (kCurveMagic / write_curve_asset /
// read_curve_asset) ────────────────────────────────────────────────────────
// 它们只服务于 aim_path 的自定义手绘曲线(把 32768 个采样量化成 .curve 二进制)。
// aim_path 连同整个瞄准控制链已被删除, 这两个函数再没有调用者 —— 留着就是
// 内部链接的未使用函数(MSVC C4505 / GCC -Wunused-function), 所以一并删除。
// 老配置目录里的 <方案名>.curves/*.curve 文件不再被读取, 也不会被删除或重写。

std::string to_bool_str(bool v)
{
    return v ? "true" : "false";
}

std::string bucket_to_str(ClassBucket b)
{
    switch (b)
    {
    case ClassBucket::Filter: return "filter";
    case ClassBucket::Aim:    return "aim";
    case ClassBucket::Delete: // fall through
    default:                  return "delete";
    }
}

ClassBucket bucket_from_str(const std::string& s, ClassBucket fallback = ClassBucket::Delete)
{
    if (s == "filter" || s == "1") return ClassBucket::Filter;
    if (s == "aim"    || s == "2") return ClassBucket::Aim;
    if (s == "delete" || s == "0") return ClassBucket::Delete;
    return fallback;
}

void apply_default_hotkey(HotkeyProfile& hk)
{
    hk.name = "Aim";
    hk.group = u8"默认";
    hk.keys = { "RightMouseButton" };
    hk.aim_classes.clear();
}

// Aim classes 序列化为分号分隔列表;
// 每条 "class_id:y_min:y_max:min_conf"。旧的 2/3 段格式仍能读回，
// 并把旧 y_offset 同时作为上下限，避免升级后锁点位置发生变化。
std::string serialize_aim_classes(const std::vector<HotkeyAimClass>& classes)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    for (size_t i = 0; i < classes.size(); ++i)
    {
        if (i) oss << ';';
        oss << classes[i].class_id
            << ':' << classes[i].y_offset
            << ':' << classes[i].y_offset_max
            << ':' << classes[i].min_conf;
    }
    return oss.str();
}

std::vector<HotkeyAimClass> parse_aim_classes(const std::string& raw)
{
    std::vector<HotkeyAimClass> out;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ';'))
    {
        auto p1 = tok.find(':');
        if (p1 == std::string::npos) continue;
        try
        {
            HotkeyAimClass c;
            c.class_id = std::stoi(tok.substr(0, p1));
            std::vector<float> values;
            std::stringstream value_stream(tok.substr(p1 + 1));
            std::string value;
            while (std::getline(value_stream, value, ':'))
                values.push_back(std::stof(value));
            if (values.empty()) continue;

            c.y_offset = values[0];
            if (values.size() >= 3)
            {
                c.y_offset_max = values[1];
                c.min_conf = values[2];
            }
            else
            {
                // 旧格式: id:y 或 id:y:min_conf。
                c.y_offset_max = c.y_offset;
                c.min_conf = (values.size() == 2) ? values[1] : 0.0f;
            }
            c.y_offset = std::clamp(c.y_offset, 0.0f, 1.0f);
            c.y_offset_max = std::clamp(c.y_offset_max, 0.0f, 1.0f);
            if (c.y_offset > c.y_offset_max)
                std::swap(c.y_offset, c.y_offset_max);
            c.min_conf = std::clamp(c.min_conf, 0.0f, 1.0f);
            out.push_back(c);
        }
        catch (...) { /* skip malformed */ }
    }
    return out;
}

} // namespace

std::vector<std::string> Config::splitString(const std::string& str, char delimiter) const
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter))
    {
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();
        tokens.push_back(item);
    }
    return tokens;
}

std::string Config::joinStrings(const std::vector<std::string>& vec, const std::string& delimiter) const
{
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (i != 0) oss << delimiter;
        oss << vec[i];
    }
    return oss.str();
}

void Config::writeDefaultsInPlace()
{
    // Most members already initialized via C++ default initializers in the
    // header; this routine only fixes up the fields that want non-default
    // values when a brand-new config.ini is generated.
    // (capture_age_offset_ms 已删除 2026-09-13)
    capture_device = "";
    capture_format = "";
    capture_width = 0;
    capture_height = 0;
    capture_fps = 0;
    capture_gpu_decode = true;
    detection_resolution = 320;
    circle_mask = true;

    backend = "TRT";
    ai_model = "sunxds_0.5.6.engine";
    confidence_threshold = 0.10f;
    nms_threshold = 0.50f;
    max_detections = kFixedMaxDetections;
    small_target_enabled = false;
    small_target_area_frac = 0.012f;
    small_target_confidence = 0.06f;
    fixed_input_size = false;

    use_cuda_graph = true;
    use_spin_wait_sync = true;
    spin_wait_timeout_ms = 50;
    use_process_boost = true;
    use_mmcss = true;
    mmcss_task_name = "Games";
    gpuMemoryReserveMB = 2048;
    enableGpuExclusiveMode = true;

    cpuCoreReserveCount = 4;
    systemMemoryReserveMB = 2048;


    screenshot_button = splitString("None");
    screenshot_delay = 500;

    class_filters.clear();

    hotkeys.clear();
    HotkeyProfile hk;
    apply_default_hotkey(hk);
    hotkeys.push_back(std::move(hk));
    active_hotkey_group = u8"\xe9\xbb\x98\xe8\xae\xa4";

    macro_enabled = false;
    macro_script_path.clear();
    macro_primary_button_events = false;
}

bool Config::loadConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    std::filesystem::path absPath = std::filesystem::absolute(std::filesystem::u8path(target), absEc);
    config_path = absEc ? target : absPath.u8string();

    if (!std::filesystem::exists(std::filesystem::u8path(target)))
    {
        std::cerr << "[Config] Config file does not exist, creating default config: " << target << std::endl;
        writeDefaultsInPlace();
        saveConfig(target);
        return true;
    }

    CSimpleIniA ini;
    SI_Error rc = ini.LoadFile(std::filesystem::u8path(target).wstring().c_str());
    if (rc < 0)
    {
        std::cerr << "[Config] Error parsing INI file: " << target << std::endl;
        return false;
    }

    auto get_string = [&](const char* section, const char* key, const std::string& defval) {
        const char* val = ini.GetValue(section, key, defval.c_str());
        return val ? std::string(val) : defval;
    };
    auto get_bool = [&](const char* section, const char* key, bool defval) {
        return ini.GetBoolValue(section, key, defval);
    };
    auto get_long = [&](const char* section, const char* key, long defval) {
        return static_cast<int>(ini.GetLongValue(section, key, defval));
    };
    auto get_double = [&](const char* section, const char* key, double defval) {
        return ini.GetDoubleValue(section, key, defval);
    };

    // ---------- Capture ----------
    // ---------- Capture: 只有「采集卡」一种方式 ----------
    // 旧配置里的 capture_method / udp_* / tcp_* / eth_* / opencv_capture_* /
    // capture_crop / capture_mf_gpu 一律不再读取; 老 config.ini 里这些键会被
    // 静默忽略, 并在下次保存时自动消失。
    //
    // 这里【不做任何校验或修正】: 组合是否真被设备支持, 要等探测完才知道。
    // 校验分两处: UI 侧(把无效项从下拉里去掉) + 采集侧(对不上直接报错)。
    // 之所以不在这里"顺手改成合法值", 是因为那正是用户明令禁止的回退路线。
    capture_device = get_string("", "capture_device", "");
    capture_format = get_string("", "capture_format", "");
    capture_width  = static_cast<int>(get_long("", "capture_width", 0));
    capture_height = static_cast<int>(get_long("", "capture_height", 0));
    capture_fps    = static_cast<int>(get_long("", "capture_fps", 0));
    if (capture_width  < 0) capture_width  = 0;
    if (capture_height < 0) capture_height = 0;
    if (capture_fps    < 0) capture_fps    = 0;
    capture_gpu_decode = get_bool("", "capture_gpu_decode", true);
    // detection_resolution 只是模型输入边长的缓存值, 用户不可设。
    // 真值在启动 / 换模型时由模型元数据写入 (publish_model_metadata),
    // 这里先读旧值垫底, 探测不到输入形状时才用得上。
    detection_resolution = std::clamp(static_cast<int>(get_long("", "detection_resolution", 320)), 32, 2048);

    // 圆形遮罩不再是用户选项 —— 中心裁切 + 圆形遮罩是本项目的固定设计,
    // 所以直接忽略 ini 里可能残留的 false, 避免升级后遮罩被静默关掉。
    circle_mask = true;

    // ---------- Hardware ----------
    // ★ 三档由 mouse/mouse_driver.h 的工厂按名字字符串分派(形状同 AimMagic
    //   的 FUN_140040ff0)。这里做一次白名单校验: **不认识的档位回落到 MAKCU**,
    //   而不是原样带下去 —— 带下去的话工厂会拒绝打开、用户看到的是"鼠标不动",
    //   而真正的原因(名字拼错了)要翻日志才知道。
    //   注: 老配置里的 MAKCU/MAKCUNEW 取值含义没变, 所以这里只做白名单校验。
    //   (原文这里还有一句"所以 pidf_mapping_version 不推进" —— 那个版本号已于
    //    2026-09-17 随瞄准控制链一起删除, 不再是需要维护的东西。)
    input_method = get_string("", "input_method", "MAKCU");
    if (input_method != "MAKCU" && input_method != "MAKCUNEW" && input_method != "KMBOXNET")
        input_method = "MAKCU";
    const auto finiteSetting = [&](const char* key, double fallback, double low, double high) {
        const double value = get_double("", key, fallback);
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    // (capture_age_offset_ms 已删除 2026-09-13: 旧 ini 里的这个键会被忽略。)
    makcu_baudrate = get_long("", "makcu_baudrate", 115200);
    makcu_port = get_string("", "makcu_port", "COM0");
    // MAKCUNEW固件上电固定115200且不回任何二进制响应帧。
    // 目标速率 != 115200 时由 MakcuNewConnection 发 0x42 SET_BAUD(或 DE AD 转义帧)
    // 后自行重连; 协商失败会自动退回 115200, 因此这里默认取固件允许的上限 6000000。
    makcu_new_baudrate = std::clamp(
        static_cast<int>(get_long("", "makcu_new_baudrate", 6000000)),
        1200, 6000000);
    makcu_new_port = get_string("", "makcu_new_port", "COM0");
    // ── KMBox Net (以太网 UDP) ──
    // 三个值都从盒子屏幕上抄。**不做格式校验** —— 格式对不对只有连一次才知道,
    // 而连不上的具体理由由 mouse_driver 的 lastError() 给出(带 IP/端口/UUID),
    // 比在这里猜"IP 长得像不像"有用得多。
    kmbox_net_ip = get_string("", "kmbox_net_ip", "192.168.2.88");
    kmbox_net_port = get_string("", "kmbox_net_port", "6234");
    kmbox_net_uuid = get_string("", "kmbox_net_uuid", "12345");
    // ---------- AI ----------
    // ★ 2026-09-17: backend 不再从配置读 —— DirectML 已整条移除, TensorRT 是唯一
    //   后端。老配置里的 backend / dml_device_id 键被安全忽略 (读都不读),
    //   下次保存时不再写出。
    backend = "TRT";
    ai_model = get_string("", "ai_model", "sunxds_0.5.6.engine");
    confidence_threshold = static_cast<float>(get_double("", "confidence_threshold", 0.15));
    nms_threshold = static_cast<float>(get_double("", "nms_threshold", 0.50));
    // ★ 2026-09-17: max_detections 固定 20, 不再从配置读(不允许用户设置)。
    max_detections = kFixedMaxDetections;
    small_target_enabled = get_bool("", "small_target_enabled", false);
    small_target_area_frac = static_cast<float>(get_double("", "small_target_area_frac", 0.012));
    small_target_confidence = static_cast<float>(get_double("", "small_target_confidence", 0.06));
    fixed_input_size = get_bool("", "fixed_input_size", false);

    use_cuda_graph = get_bool("", "use_cuda_graph", true);
    use_spin_wait_sync = get_bool("", "use_spin_wait_sync", true);
    spin_wait_timeout_ms = static_cast<int>(std::clamp<long>(get_long("", "spin_wait_timeout_ms", 50), 1L, 1000L));
    use_process_boost = get_bool("", "use_process_boost", true);
    use_mmcss = get_bool("", "use_mmcss", true);
    mmcss_task_name = get_string("", "mmcss_task_name", "Games");
    gpuMemoryReserveMB = get_long("", "gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("", "enableGpuExclusiveMode", true);

    cpuCoreReserveCount = get_long("", "cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("", "systemMemoryReserveMB", 2048);


    // ---------- Debug ----------
    show_window = get_bool("", "show_window", true);
    show_fps = get_bool("", "show_fps", false);
    screenshot_button = splitString(get_string("", "screenshot_button", "None"));
    screenshot_delay = get_long("", "screenshot_delay", 500);
    verbose = get_bool("", "verbose", false);

    replay_record_enabled  = get_bool("", "replay_record_enabled", false);
    replay_seconds         = std::clamp(get_long("", "replay_seconds", 10), 1, 60);
    replay_playback_speed  = std::clamp(
        static_cast<float>(get_double("", "replay_playback_speed", 0.25)),
        0.05f, 2.0f);

    // ── Auto capture (data collection) ──
    auto_capture_enabled    = get_bool("",   "auto_capture_enabled",    false);
    auto_capture_use_high   = get_bool("",   "auto_capture_use_high",   true);
    auto_capture_high_conf  = std::clamp(
        static_cast<float>(get_double("", "auto_capture_high_conf", 0.85)), 0.0f, 1.0f);
    auto_capture_use_low    = get_bool("",   "auto_capture_use_low",    false);
    auto_capture_low_conf   = std::clamp(
        static_cast<float>(get_double("", "auto_capture_low_conf", 0.30)), 0.0f, 1.0f);
    auto_capture_any_detection   = get_bool("", "auto_capture_any_detection",   false);
    auto_capture_cooldown_ms = std::max(0,
        static_cast<int>(get_long("", "auto_capture_cooldown_ms", 200)));
    auto_capture_force_keys = splitString(
        get_string("", "auto_capture_force_keys", "X2MouseButton"));
    auto_capture_output_dir = get_string("", "auto_capture_output_dir",
                                         "screenshots/auto");
    auto_capture_save_label = get_bool("",   "auto_capture_save_label", true);

    // ---------- Crosshair color detector (palette + rect + area) ----------
    crosshair_rect_w           = std::clamp(get_long("", "crosshair_rect_w",  40), 4, 512);
    crosshair_rect_h           = std::clamp(get_long("", "crosshair_rect_h",  40), 4, 512);
    crosshair_min_pixel_count  = std::clamp(get_long("", "crosshair_min_pixel_count", 4), 1, 10000);
    crosshair_close_radius     = std::clamp(get_long("", "crosshair_close_radius",    1), 0, 7);
    // (crosshair_smooth 已删除 2026-09-13: 旧 ini 里的这个键会被忽略。)

    crosshair_colors.clear();
    {
        // Each [crosshair_color.N] section = one HSV band in the palette.
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> cc_sections;
        const std::string prefix = "crosshair_color.";
        for (const auto& s : sections)
        {
            std::string sname = s.pItem;
            if (sname.rfind(prefix, 0) != 0) continue;
            int idx = 0;
            try { idx = std::stoi(sname.substr(prefix.size())); }
            catch (...) { continue; }
            cc_sections.emplace_back(idx, std::move(sname));
        }
        std::sort(cc_sections.begin(), cc_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : cc_sections)
        {
            const char* sec = entry.second.c_str();
            CrosshairColorProfileConfig c;
            c.name    = get_string(sec, "name", "Color");
            c.enabled = get_bool(sec, "enabled", true);
            c.h_low   = std::clamp(get_long(sec, "h_low",   0),   0, 179);
            c.h_high  = std::clamp(get_long(sec, "h_high",  10),  0, 179);
            c.s_min   = std::clamp(get_long(sec, "s_min",   120), 0, 255);
            c.s_max   = std::clamp(get_long(sec, "s_max",   255), 0, 255);
            c.v_min   = std::clamp(get_long(sec, "v_min",   120), 0, 255);
            c.v_max   = std::clamp(get_long(sec, "v_max",   255), 0, 255);
            crosshair_colors.push_back(std::move(c));
        }
        if (crosshair_colors.empty())
        {
            // Seed with the red double-band so upgrading users don't see an
            // empty palette the first time.
            CrosshairColorProfileConfig low;
            low.name = "Red-Low";  low.h_low = 0;   low.h_high = 10;
            CrosshairColorProfileConfig hi;
            hi.name  = "Red-High"; hi.h_low  = 160; hi.h_high  = 179;
            crosshair_colors.push_back(std::move(low));
            crosshair_colors.push_back(std::move(hi));
        }
    }


    // ---------- Macro (Lua / G HUB-compatible) ----------
    macro_enabled = get_bool("", "macro_enabled", false);
    macro_script_path = get_string("", "macro_script_path", "");
    macro_primary_button_events = get_bool("", "macro_primary_button_events", false);

    // ---------- Class filters ----------
    class_filters.clear();
    {
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys("classes", keys);
        for (const auto& k : keys)
        {
            int class_id = 0;
            try { class_id = std::stoi(k.pItem); }
            catch (...) { continue; }

            std::string val = ini.GetValue("classes", k.pItem, "");
            auto parts = splitString(val, ',');
            ClassFilterState st;
            st.class_id = class_id;
            if (!parts.empty())
                st.bucket = bucket_from_str(parts[0]);
            if (parts.size() >= 2)
                st.class_name = parts[1];
            class_filters.push_back(st);
        }
        std::sort(class_filters.begin(), class_filters.end(),
            [](const ClassFilterState& a, const ClassFilterState& b) {
                return a.class_id < b.class_id;
            });
    }

    // ---------- Hotkeys ----------
    hotkeys.clear();
    {
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> hk_sections;
        for (const auto& s : sections)
        {
            std::string name = s.pItem;
            const std::string prefix = "hotkey.";
            if (name.rfind(prefix, 0) != 0)
                continue;
            int idx = 0;
            try { idx = std::stoi(name.substr(prefix.size())); }
            catch (...) { continue; }
            hk_sections.emplace_back(idx, std::move(name));
        }
        std::sort(hk_sections.begin(), hk_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& entry : hk_sections)
        {
            const char* sec = entry.second.c_str();
            HotkeyProfile hk; // defaults come from the struct initializer
            hk.name = get_string(sec, "name", hk.name);
            hk.group = get_string(sec, "group", hk.group);
            if (hk.group.empty())
                hk.group = u8"默认";
            hk.keys = splitString(get_string(sec, "keys", "RightMouseButton"));

            hk.fovX = get_long(sec, "fovX", hk.fovX);
            hk.fovY = get_long(sec, "fovY", hk.fovY);

            // ── 【2026-09-17 整条删除】瞄准控制链的槽位与迁移机制 ─────────────
            // 整个瞄准控制链(aim_pid / boss_aim / aim_tracker / aim_scale / aim_path /
            // anchor_filter / auto_stop / trigger_scope / autotune_*)连同
            // runtime/mouse_thread_loop.cpp 一起被删除, 程序现在只做【采集 → 推理】。
            //
            // 所以下面这些键【读都不读】了(不报错), 写回时也不再出现:
            //   pidf_mapping_version / pidf_kp_* / pidf_ki_* / pidf_kd_* /
            //   pidf_psat_* (含更老的 pidf_deadzone_*) / pidf_limit_* /
            //   pidf_predict_* / pidf_inflight_* / esync_* / aim_scale_* /
            //   aim_path_* / trigger_* / aim_px_per_count_* / pidf_kf_* / pidf_lr_*
            //
            // ★ `pidf_mapping_version` 那套迁移机制【一并删除】: 它的用途是"槽位语义
            //   变了就把老配置里的槽位重置掉", 而现在那些槽位本身已经不存在 ——
            //   没有东西可以迁移, 版本号也不再有任何读者。留着它只会让人以为
            //   "老配置会被正确迁移", 而实际上根本没有槽位需要迁移。
            // ★ 这【不是】"把迁移逻辑删掉、让老配置被误读": 老配置里那些键的消费者
            //   (控制器)已经不存在了, 读出来也无处可去。用户升级后那些键被静默丢弃
            //   才是唯一正确的行为 —— 它们描述的是一套已经不在的程序。

            // 目标选择 — 优先级列表
            {
                const std::string aim_raw = get_string(sec, "aim_classes", "");
                hk.aim_classes = parse_aim_classes(aim_raw);
                // 从旧三槽 target_class_N/y_top_N/y_bot_N 平滑迁移。
                // 旧值是“距框顶比例”，新 UI 是 1=框顶/0=框底，因此反转。
                if (aim_raw.empty())
                {
                    for (int slot = 1; slot <= 3; ++slot)
                    {
                        const std::string suffix = std::to_string(slot);
                        const int class_id = static_cast<int>(get_long(
                            sec, ("target_class_" + suffix).c_str(), -1));
                        if (class_id < 0) continue;

                        const float y_top = static_cast<float>(get_double(
                            sec, ("target_y_top_" + suffix).c_str(), 0.0));
                        const float y_bot = static_cast<float>(get_double(
                            sec, ("target_y_bot_" + suffix).c_str(), 1.0));
                        HotkeyAimClass migrated;
                        migrated.class_id = class_id;
                        migrated.y_offset = std::clamp(1.0f - y_bot, 0.0f, 1.0f);
                        migrated.y_offset_max = std::clamp(1.0f - y_top, 0.0f, 1.0f);
                        if (migrated.y_offset > migrated.y_offset_max)
                            std::swap(migrated.y_offset, migrated.y_offset_max);
                        migrated.min_conf = static_cast<float>(get_double(
                            sec, ("target_min_conf_" + suffix).c_str(), 0.0));
                        hk.aim_classes.push_back(migrated);
                    }
                }
            }

            hk.crosshair_detect_enabled  = get_bool(sec, "crosshair_detect_enabled", false);

            // 动态 FOV:优先读 strength;否则从旧 margin_frac 反推算。
            {
                hk.dynamic_fov_enabled = get_bool(sec, "dynamic_fov_enabled", hk.dynamic_fov_enabled);
                const float legacy_margin = static_cast<float>(
                    get_double(sec, "dynamic_fov_margin_frac", 2.0 - hk.dynamic_fov_strength));
                const float legacy_strength = std::clamp(2.0f - legacy_margin, 0.0f, 1.0f);
                hk.dynamic_fov_strength = static_cast<float>(
                    get_double(sec, "dynamic_fov_strength", legacy_strength));
            }

            // ★ 瞄准轨迹曲线 (aim_path_*: 直线/贝塞尔/手绘/WindMouse) 已于
            //   2026-09-17 随 mouse/aim_path.h 一起整条删除 —— 没有消费者了。
            //   老配置里的这些键(含 aim_path_custom_file / aim_path_custom_samples /
            //   aim_path_neural_*)读都不读, 写回时也不再出现。
            //   注: 以前这里会在加载边界把自定义曲线升采样到 kAimPathSampleCount,
            //   那个常数也随字段一起删掉了。

            // ── ★★ 通用控制器层 (2026-09-17 第三轮重建) ───────────────────
            // 六个增益全部分方向; 设计见 docs/generic-controller-layer.md §4.3。
            // ★ 默认值取自 HotkeyProfile 的成员初值(等价历史单套行为),
            //   所以老配置里没有这些键时行为不变。
            hk.ctl_kp_x = get_double(sec, "ctl_kp_x", hk.ctl_kp_x);
            hk.ctl_kp_y = get_double(sec, "ctl_kp_y", hk.ctl_kp_y);
            hk.ctl_ki_x = get_double(sec, "ctl_ki_x", hk.ctl_ki_x);
            hk.ctl_ki_y = get_double(sec, "ctl_ki_y", hk.ctl_ki_y);
            hk.ctl_kd_x = get_double(sec, "ctl_kd_x", hk.ctl_kd_x);
            hk.ctl_kd_y = get_double(sec, "ctl_kd_y", hk.ctl_kd_y);
            hk.ctl_tau_unwind_sec = get_double(sec, "ctl_tau_unwind_sec", hk.ctl_tau_unwind_sec);
            hk.ctl_tau_deriv_sec  = get_double(sec, "ctl_tau_deriv_sec",  hk.ctl_tau_deriv_sec);
            hk.ctl_i_max          = get_double(sec, "ctl_i_max",          hk.ctl_i_max);
            hk.ctl_max_output_counts =
                static_cast<int>(get_double(sec, "ctl_max_output_counts", hk.ctl_max_output_counts));
            hk.ctl_p_full_scale_px = get_double(sec, "ctl_p_full_scale_px", hk.ctl_p_full_scale_px);
            hk.ctl_y_offset     = get_double(sec, "ctl_y_offset",     hk.ctl_y_offset);
            hk.ctl_y_offset_max = get_double(sec, "ctl_y_offset_max", hk.ctl_y_offset_max);
            hk.ctl_hysteresis_ratio = get_double(sec, "ctl_hysteresis_ratio", hk.ctl_hysteresis_ratio);
            // ★★ 总开关默认 false —— 新增的控制链在真机验证过之前不该自己动鼠标。
            hk.ctl_enabled = get_bool(sec, "ctl_enabled", false);
            hk.ctl_max_distance_px = get_double(sec, "ctl_max_distance_px", hk.ctl_max_distance_px);
            hk.ctl_match_center_ratio = get_double(sec, "ctl_match_center_ratio", hk.ctl_match_center_ratio);
            hk.ctl_area_ratio_tol = get_double(sec, "ctl_area_ratio_tol", hk.ctl_area_ratio_tol);
            hk.ctl_k_snap_mult = get_double(sec, "ctl_k_snap_mult", hk.ctl_k_snap_mult);
            hk.ctl_min_aspect = get_double(sec, "ctl_min_aspect", hk.ctl_min_aspect);
            hk.ctl_max_aspect = get_double(sec, "ctl_max_aspect", hk.ctl_max_aspect);
            hk.ctl_random_seed =
                static_cast<int>(get_double(sec, "ctl_random_seed", hk.ctl_random_seed));

            // ── 自动扳机 (2026-09-17 恢复) ────────────────────────────────
            // ★ 这些键在 2026-09-17 那轮被连同后端一起删掉过。老配置里若还留着
            //   它们(旧版本写出去的), 现在【重新生效】—— 这是有意的:
            //   用户要求把扳机加回来, 那旧值就该继续可用。
            hk.trigger_enabled = get_bool(sec, "trigger_enabled", false);
            hk.trigger_fire_delay = static_cast<int>(get_double(sec, "trigger_fire_delay", hk.trigger_fire_delay));
            hk.trigger_fire_duration = static_cast<int>(get_double(sec, "trigger_fire_duration", hk.trigger_fire_duration));
            hk.trigger_fire_interval = static_cast<int>(get_double(sec, "trigger_fire_interval", hk.trigger_fire_interval));
            hk.trigger_y_percent = static_cast<int>(get_double(sec, "trigger_y_percent", hk.trigger_y_percent));
            hk.trigger_delay_jitter_ms = static_cast<int>(get_double(sec, "trigger_delay_jitter_ms", hk.trigger_delay_jitter_ms));
            hk.trigger_duration_jitter_ms = static_cast<int>(get_double(sec, "trigger_duration_jitter_ms", hk.trigger_duration_jitter_ms));
            hk.trigger_interval_jitter_ms = static_cast<int>(get_double(sec, "trigger_interval_jitter_ms", hk.trigger_interval_jitter_ms));
            hk.trigger_switch_cooldown_ms = static_cast<int>(get_double(sec, "trigger_switch_cooldown_ms", hk.trigger_switch_cooldown_ms));
            hk.trigger_auto_scope = static_cast<int>(get_double(sec, "trigger_auto_scope", hk.trigger_auto_scope));
            hk.trigger_scope_delay_ms = static_cast<int>(get_double(sec, "trigger_scope_delay_ms", hk.trigger_scope_delay_ms));
            hk.trigger_auto_stop = static_cast<int>(get_double(sec, "trigger_auto_stop", hk.trigger_auto_stop));
            hk.trigger_stop_ms = static_cast<int>(get_double(sec, "trigger_stop_ms", hk.trigger_stop_ms));

            // ── 瞄准轨迹曲线 (2026-09-17 恢复) ────────────────────────────
            hk.aim_path_mode = static_cast<int>(get_double(sec, "aim_path_mode", hk.aim_path_mode));
            hk.aim_path_influence = static_cast<int>(get_double(sec, "aim_path_influence", hk.aim_path_influence));
            hk.aim_path_bezier_cx1 = static_cast<float>(get_double(sec, "aim_path_bezier_cx1", hk.aim_path_bezier_cx1));
            hk.aim_path_bezier_cy1 = static_cast<float>(get_double(sec, "aim_path_bezier_cy1", hk.aim_path_bezier_cy1));
            hk.aim_path_bezier_cx2 = static_cast<float>(get_double(sec, "aim_path_bezier_cx2", hk.aim_path_bezier_cx2));
            hk.aim_path_bezier_cy2 = static_cast<float>(get_double(sec, "aim_path_bezier_cy2", hk.aim_path_bezier_cy2));
            hk.aim_path_wind_gravity = static_cast<float>(get_double(sec, "aim_path_wind_gravity", hk.aim_path_wind_gravity));
            hk.aim_path_wind_wind = static_cast<float>(get_double(sec, "aim_path_wind_wind", hk.aim_path_wind_wind));
            hk.aim_path_wind_step = static_cast<float>(get_double(sec, "aim_path_wind_step", hk.aim_path_wind_step));
            hk.aim_path_wind_distance = static_cast<float>(get_double(sec, "aim_path_wind_distance", hk.aim_path_wind_distance));
            hk.aim_path_wind_threshold = static_cast<int>(get_double(sec, "aim_path_wind_threshold", hk.aim_path_wind_threshold));

            hotkeys.push_back(std::move(hk));
        }

        if (hotkeys.empty())
        {
            HotkeyProfile hk;
            apply_default_hotkey(hk);
            hotkeys.push_back(std::move(hk));
        }
    }

    active_hotkey_group = get_string("", "active_hotkey_group", u8"\xe9\xbb\x98\xe8\xae\xa4");

    // Guard against double-encoded UTF-8 group names: if the loaded
    // active_hotkey_group doesn't match any hotkey's group, fall back
    // to the group of the first hotkey (or the default).
    {
        bool matched = false;
        for (const auto& hk : hotkeys)
            if (hk.group == active_hotkey_group) { matched = true; break; }
        if (!matched && !hotkeys.empty())
            active_hotkey_group = hotkeys[0].group;
    }

    // ── 【2026-09-17】瞄准控制链的字段夹取整段删除 ───────────────────────────
    // 这里原本是 clamp_aim_fields(): 把 pidf_* / esync_* / aim_scale_* / aim_path_* /
    // trigger_* 全部夹到各自的合法域。整个瞄准控制链(连同它的槽位)已被删除, 所以
    // 这些夹取也一并删除 —— 夹取一个不存在的字段既编不过, 也没有意义。
    //
    // ★ 保留下来的只剩"目标选择"与"动态 FOV"两项: 它们服务的是【检测/瞄准点选择】
    //   这一侧(aim_classes 由 TargetPage 维护, dynamic_fov 由动态 FOV 门控区域),
    //   与控制链无关, 仍然活着。
    auto clamp_target_fields = [](HotkeyProfile& hk) {

        // 目标选择 clamp
        for (auto& ac : hk.aim_classes)
        {
            ac.y_offset = std::clamp(ac.y_offset, 0.0f, 1.0f);
            ac.y_offset_max = std::clamp(ac.y_offset_max, 0.0f, 1.0f);
            if (ac.y_offset > ac.y_offset_max)
                std::swap(ac.y_offset, ac.y_offset_max);
            ac.min_conf = std::clamp(ac.min_conf, 0.0f, 1.0f);
        }

        hk.dynamic_fov_strength = std::clamp(hk.dynamic_fov_strength, 0.0f, 1.0f);

        // ── ★★ 通用控制器层 (2026-09-17 第三轮重建) ───────────────────────
        // 只夹到"物理上说得通"的范围, 不做"推荐值"式夹取 ——
        // 具体该填多少是用户在实机上调的(方案 §7 第 10 条)。
        // ★ 负增益会反转控制方向(正反馈), 直接夹死。
        hk.ctl_kp_x = std::max(0.0, hk.ctl_kp_x);
        hk.ctl_kp_y = std::max(0.0, hk.ctl_kp_y);
        hk.ctl_ki_x = std::max(0.0, hk.ctl_ki_x);
        hk.ctl_ki_y = std::max(0.0, hk.ctl_ki_y);
        hk.ctl_kd_x = std::max(0.0, hk.ctl_kd_x);
        hk.ctl_kd_y = std::max(0.0, hk.ctl_kd_y);
        // ★ 时间常数必须为正 —— 0 或负会让 exp(-dt/τ) 退化(除零 / 发散)。
        //   给一个下限而不是夹到 0, 因为 0 在公式里是奇点。
        hk.ctl_tau_unwind_sec = std::clamp(hk.ctl_tau_unwind_sec, 1e-4, 10.0);
        // ★ D 项低通允许 0(= 不做低通)。方案里 Kd 默认就是 0, 所以 0 是合法值。
        hk.ctl_tau_deriv_sec = std::clamp(hk.ctl_tau_deriv_sec, 0.0, 10.0);
        hk.ctl_i_max = std::max(0.0, hk.ctl_i_max);
        hk.ctl_p_full_scale_px = std::max(0.0, hk.ctl_p_full_scale_px);
        // ★ 限幅必须 >= 1: 它是量化出口的硬上限, 0 会让控制器永远发不出位移。
        hk.ctl_max_output_counts = std::clamp(hk.ctl_max_output_counts, 1, 1000);
        hk.ctl_y_offset = std::clamp(hk.ctl_y_offset, 0.0, 1.0);
        hk.ctl_y_offset_max = std::clamp(hk.ctl_y_offset_max, 0.0, 1.0);
        if (hk.ctl_y_offset > hk.ctl_y_offset_max)
            std::swap(hk.ctl_y_offset, hk.ctl_y_offset_max);
        // ★ 滞回倍数 <= 1 等于没有滞回(每帧重选最近) —— 那是合法的调试配置,
        //   但要 >= 1 才符合"滞回"的定义, 所以夹到 [1, 10]。
        hk.ctl_hysteresis_ratio = std::clamp(hk.ctl_hysteresis_ratio, 1.0, 10.0);

        // 稳定器 / 选靶距离
        hk.ctl_max_distance_px = std::max(0.0, hk.ctl_max_distance_px);
        // ★ 认目标系数必须 > 0: 0 会让"中心距离 < 0"永不成立 ⇒ 永远认不出目标。
        hk.ctl_match_center_ratio = std::clamp(hk.ctl_match_center_ratio, 1e-3, 10.0);
        // ★ 面积容差必须 >= 1: < 1 是自相矛盾的区间(下界 > 上界)。
        hk.ctl_area_ratio_tol = std::clamp(hk.ctl_area_ratio_tol, 1.0, 100.0);
        // ★ 突变系数必须 > 0: 它是"多远算瞬移"的乘子, 0 会让任何位移都算瞬移。
        hk.ctl_k_snap_mult = std::clamp(hk.ctl_k_snap_mult, 1e-3, 100.0);
        // 宽高比: min <= max, 且都为正。
        hk.ctl_min_aspect = std::clamp(hk.ctl_min_aspect, 1e-3, 100.0);
        hk.ctl_max_aspect = std::clamp(hk.ctl_max_aspect, 1e-3, 100.0);
        if (hk.ctl_min_aspect > hk.ctl_max_aspect)
            std::swap(hk.ctl_min_aspect, hk.ctl_max_aspect);
        // 种子: 负值无意义(0 已经是"用固定常数"), 夹到非负。
        hk.ctl_random_seed = std::max(0, hk.ctl_random_seed);

        // ── 自动扳机 (2026-09-17 恢复) ────────────────────────────────────
        // 三个延迟/时长都夹到非负; interval 至少 1ms —— 0 会让 Cooldown
        // 立刻结束, 在命中区里退化成每拍 press/release 的抖动。
        hk.trigger_fire_delay    = std::max(0, hk.trigger_fire_delay);
        hk.trigger_fire_duration = std::max(0, hk.trigger_fire_duration);
        hk.trigger_fire_interval = std::max(1, hk.trigger_fire_interval);
        hk.trigger_delay_jitter_ms    = std::max(0, hk.trigger_delay_jitter_ms);
        hk.trigger_duration_jitter_ms = std::max(0, hk.trigger_duration_jitter_ms);
        hk.trigger_interval_jitter_ms = std::max(0, hk.trigger_interval_jitter_ms);
        hk.trigger_switch_cooldown_ms = std::max(0, hk.trigger_switch_cooldown_ms);
        hk.trigger_scope_delay_ms = std::max(0, hk.trigger_scope_delay_ms);
        // ★ 命中区百分比下限 10: 比 bbox 小太多的"命中区"几乎不可能命中,
        //   等于把扳机变成静默失效。上限 300 允许"预开火"(框上方也算)。
        hk.trigger_y_percent = std::clamp(hk.trigger_y_percent, 10, 300);
        hk.trigger_auto_scope = std::clamp(hk.trigger_auto_scope, 0, 2);
        hk.trigger_auto_stop = hk.trigger_auto_stop > 0 ? 1 : 0;
        // 与旧实现一致: 20~300ms。太短固件来不及弹起, 太长玩家被推着走。
        hk.trigger_stop_ms = std::clamp(hk.trigger_stop_ms, 20, 300);

        // ── 瞄准轨迹曲线 (2026-09-17 恢复) ────────────────────────────────
        hk.aim_path_mode = std::clamp(hk.aim_path_mode, 0, 3);
        hk.aim_path_influence = std::clamp(hk.aim_path_influence, 0, 100);
        // Bezier 控制点: X 夹到 [0,1] 保证不出现折返; Y 夹到 [-1,1] 是
        // 弦长的比例 —— 超出会让路径横向甩出去。
        hk.aim_path_bezier_cx1 = std::clamp(hk.aim_path_bezier_cx1, 0.0f, 1.0f);
        hk.aim_path_bezier_cx2 = std::clamp(hk.aim_path_bezier_cx2, 0.0f, 1.0f);
        hk.aim_path_bezier_cy1 = std::clamp(hk.aim_path_bezier_cy1, -1.0f, 1.0f);
        hk.aim_path_bezier_cy2 = std::clamp(hk.aim_path_bezier_cy2, -1.0f, 1.0f);
        // WindMouse: 重力/风力/步长/距离都必须为正, 否则物理模型退化
        // (G=0 或 M=0 会让路径根本走不动)。
        hk.aim_path_wind_gravity = std::clamp(hk.aim_path_wind_gravity, 0.1f, 100.0f);
        hk.aim_path_wind_wind    = std::clamp(hk.aim_path_wind_wind, 0.0f, 100.0f);
        hk.aim_path_wind_step    = std::clamp(hk.aim_path_wind_step, 1.0f, 200.0f);
        hk.aim_path_wind_distance = std::clamp(hk.aim_path_wind_distance, 1.0f, 200.0f);
        hk.aim_path_wind_threshold = std::max(0, hk.aim_path_wind_threshold);
    };
    for (auto& hk : hotkeys)
        clamp_target_fields(hk);

    // Aim hotkey triggers are restricted to the four mouse buttons. Anything
    // else (old keyboard bindings, typos) is rewritten to "None" so the UI
    // combo stays in sync with the allowed set.
    static const std::unordered_set<std::string> kAllowedAimKeys = {
        "None", "LeftMouseButton", "RightMouseButton",
        "X1MouseButton", "X2MouseButton",
    };
    for (auto& hk : hotkeys)
    {
        for (auto& k : hk.keys)
        {
            if (kAllowedAimKeys.find(k) == kAllowedAimKeys.end())
                k = "None";
        }
        if (hk.keys.empty())
            hk.keys.push_back("None");
    }

    return true;
}

void Config::retargetConfigPath(const std::string& filename)
{
    const std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    const std::filesystem::path absPath =
        std::filesystem::absolute(std::filesystem::u8path(target), absEc);
    config_path = absEc ? target : absPath.u8string();
}

bool Config::saveConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    if (target == "config.ini" && !config_path.empty())
        target = config_path;

    // Use the wide-char path so non-ASCII directories (e.g. Chinese
    // user folders) open correctly. The narrow ofstream ctor on MSVC
    // interprets the string as the system ANSI codepage and fails when
    // the UTF-8 path contains characters outside it.
    std::filesystem::path targetPath = std::filesystem::u8path(target);
    std::error_code mkEc;
    if (targetPath.has_parent_path())
        std::filesystem::create_directories(targetPath.parent_path(), mkEc);

    std::ofstream file(targetPath.wstring().c_str(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
    {
        DWORD winErr = ::GetLastError();
        std::cerr << "[Config] Error opening config for writing: " << target
                  << " (errno=" << errno << ", GetLastError=" << winErr << ")" << std::endl;
        return false;
    }

    file << "# Apotheosis configuration.\n";
    file << "# Generated automatically; hand-edit with care.\n\n";

    file << u8"# Capture  (只有「采集卡」一种方式; 参数必须来自设备真实能力探测,\n"
            u8"# 组合对不上会直接报错, 不做任何替换)\n"
        << "capture_device = " << capture_device << "\n"
        << "capture_format = " << capture_format << "\n"
        << "capture_width = " << capture_width << "\n"
        << "capture_height = " << capture_height << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "capture_gpu_decode = " << to_bool_str(capture_gpu_decode) << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "circle_mask = " << to_bool_str(circle_mask) << "\n\n";

    file << "# Hardware / input device\n"
        << "# MAKCU | MAKCUNEW | KMBOXNET  (三档共用 mouse/mouse_driver.h 的驱动抽象)\n"
        << "input_method = " << input_method << "\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n"
        << "makcu_new_baudrate = " << makcu_new_baudrate << "\n"
        << "makcu_new_port = " << makcu_new_port << "\n"
        << "# KMBox Net: 三个值照抄盒子屏幕上显示的 ip / port / uuid\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n\n";

    // ★ 2026-09-17: backend / dml_device_id / max_detections 不再落盘 ——
    //   前两者随 DirectML 后端一起删除, 后者固定为 kFixedMaxDetections。
    //   注意 backend 仍是 Config 成员(会话启动要读), 只是不再写进 config.ini。
    file << "# AI\n"
        << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << "small_target_enabled = " << to_bool_str(small_target_enabled) << "\n"
        << std::setprecision(3)
        << "small_target_area_frac = " << small_target_area_frac << "\n"
        << std::setprecision(2)
        << "small_target_confidence = " << small_target_confidence << "\n"
        << std::setprecision(0)
        << "fixed_input_size = " << to_bool_str(fixed_input_size) << "\n\n";

    file << "# CUDA / system\n"
        << "use_cuda_graph = " << to_bool_str(use_cuda_graph) << "\n"
        << "use_spin_wait_sync = " << to_bool_str(use_spin_wait_sync) << "\n"
        << "spin_wait_timeout_ms = " << spin_wait_timeout_ms << "\n"
        << "use_process_boost = " << to_bool_str(use_process_boost) << "\n"
        << "use_mmcss = " << to_bool_str(use_mmcss) << "\n"
        << "mmcss_task_name = " << mmcss_task_name << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << to_bool_str(enableGpuExclusiveMode) << "\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    file << "# Replay\n"
        << "replay_record_enabled = " << to_bool_str(replay_record_enabled) << "\n"
        << "replay_seconds = " << replay_seconds << "\n"
        << "replay_playback_speed = " << replay_playback_speed << "\n\n";

    file << "# Crosshair color detector (palette + ROI; per-hotkey toggle lives on each [hotkey.N])\n"
        << "crosshair_rect_w = "          << crosshair_rect_w          << "\n"
        << "crosshair_rect_h = "          << crosshair_rect_h          << "\n"
        << "crosshair_min_pixel_count = " << crosshair_min_pixel_count << "\n"
        << "crosshair_close_radius = "    << crosshair_close_radius    << "\n\n";

    file << "# Debug\n"
        << "show_window = " << to_bool_str(show_window) << "\n"
        << "show_fps = " << to_bool_str(show_fps) << "\n"
        << "screenshot_button = " << joinStrings(screenshot_button) << "\n"
        << "screenshot_delay = " << screenshot_delay << "\n"
        << "verbose = " << to_bool_str(verbose) << "\n\n";

    file << "# Auto capture (data collection harness)\n"
        << "auto_capture_enabled = "    << to_bool_str(auto_capture_enabled) << "\n"
        << "auto_capture_use_high = "   << to_bool_str(auto_capture_use_high) << "\n"
        << "auto_capture_high_conf = "  << auto_capture_high_conf << "\n"
        << "auto_capture_use_low = "    << to_bool_str(auto_capture_use_low) << "\n"
        << "auto_capture_low_conf = "   << auto_capture_low_conf << "\n"
        << "auto_capture_any_detection = "  << to_bool_str(auto_capture_any_detection)  << "\n"
        << "auto_capture_cooldown_ms = " << auto_capture_cooldown_ms << "\n"
        << "auto_capture_force_keys = " << joinStrings(auto_capture_force_keys) << "\n"
        << "auto_capture_output_dir = " << auto_capture_output_dir << "\n"
        << "auto_capture_save_label = " << to_bool_str(auto_capture_save_label) << "\n\n";

    file << "# Macro (G HUB-compatible Lua). Drop a .lua script path into\n"
            "# macro_script_path; runtime loads it on startup when macro_enabled\n"
            "# is true. macro_primary_button_events mirrors the script-side\n"
            "# EnablePrimaryMouseButtonEvents default.\n"
        << "macro_enabled = " << to_bool_str(macro_enabled) << "\n"
        << "macro_script_path = " << macro_script_path << "\n"
        << "macro_primary_button_events = " << to_bool_str(macro_primary_button_events) << "\n\n";

    // Class filter table.
    file << "[classes]\n";
    file << "# Format: <class_id> = <bucket>,<display_name>\n";
    file << "# bucket in { delete, filter, aim }\n";
    for (const auto& cf : class_filters)
    {
        file << cf.class_id << " = " << bucket_to_str(cf.bucket);
        if (!cf.class_name.empty())
            file << "," << cf.class_name;
        file << "\n";
    }
    file << "\n";

    file << "active_hotkey_group = " << active_hotkey_group << "\n\n";

    // Hotkey profiles.
    for (size_t i = 0; i < hotkeys.size(); ++i)
    {
        const auto& hk = hotkeys[i];
        file << "[hotkey." << i << "]\n";
        file << "name = " << hk.name << "\n";
        file << "group = " << hk.group << "\n";
        file << "keys = " << joinStrings(hk.keys) << "\n";
        file << "fovX = " << hk.fovX << "\n";
        file << "fovY = " << hk.fovY << "\n";
        // ── 【2026-09-17 整条删除】瞄准控制链的槽位 ─────────────────────────
        // 这里原本写出 pidf_mapping_version / pidf_* / esync_* / aim_scale_* /
        // trigger_* / aim_path_* 几十个键。整个瞄准控制链(连同 mouse_thread_loop.cpp)
        // 已被删除, 程序现在只做【采集 → 推理】, 那些键没有任何消费者 —— 所以
        // 不再写出。老方案文件里残留的它们会在下一次保存时自动消失。
        //   ★ pidf_mapping_version 也不再写: 它的唯一用途是"槽位语义变更时重置
        //     老槽位", 而槽位已经不存在, 版本号没有读者, 写了反而像还有迁移机制。
        //
        // 下面这些【保留】: 它们服务的是检测/瞄准点选择与准星找色, 与控制链无关。
        file << std::setprecision(0)
             << "aim_classes = "       << serialize_aim_classes(hk.aim_classes) << "\n"
             << "crosshair_detect_enabled = "  << to_bool_str(hk.crosshair_detect_enabled)  << "\n"
             << "dynamic_fov_enabled = " << to_bool_str(hk.dynamic_fov_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "dynamic_fov_strength = " << hk.dynamic_fov_strength << "\n"
             << std::setprecision(4);

        // ── ★★ 通用控制器层 (2026-09-17 第三轮重建) ───────────────────────
        // 六个增益全部分方向。数值全部由用户实调 —— 设计里没有任何"最优值"
        // (方案 §7 第 10 条)。这里只负责落盘。
        file << std::fixed << std::setprecision(4)
             << "ctl_enabled = "          << to_bool_str(hk.ctl_enabled) << "\n"
             << "ctl_kp_x = "             << hk.ctl_kp_x << "\n"
             << "ctl_kp_y = "             << hk.ctl_kp_y << "\n"
             << "ctl_ki_x = "             << hk.ctl_ki_x << "\n"
             << "ctl_ki_y = "             << hk.ctl_ki_y << "\n"
             << "ctl_kd_x = "             << hk.ctl_kd_x << "\n"
             << "ctl_kd_y = "             << hk.ctl_kd_y << "\n"
             << "ctl_tau_unwind_sec = "   << hk.ctl_tau_unwind_sec << "\n"
             << "ctl_tau_deriv_sec = "    << hk.ctl_tau_deriv_sec << "\n"
             << "ctl_i_max = "            << hk.ctl_i_max << "\n"
             << "ctl_p_full_scale_px = "  << hk.ctl_p_full_scale_px << "\n"
             << "ctl_y_offset = "         << hk.ctl_y_offset << "\n"
             << "ctl_y_offset_max = "     << hk.ctl_y_offset_max << "\n"
             << "ctl_hysteresis_ratio = " << hk.ctl_hysteresis_ratio << "\n"
             // ★ 它是 int —— `std::fixed` 对整数不起作用, 直接写就是 "200",
             //   不必为它单独切 setprecision。
             << "ctl_max_output_counts = " << hk.ctl_max_output_counts << "\n"
             << "ctl_max_distance_px = "    << hk.ctl_max_distance_px << "\n"
             << "ctl_match_center_ratio = " << hk.ctl_match_center_ratio << "\n"
             << "ctl_area_ratio_tol = "     << hk.ctl_area_ratio_tol << "\n"
             << "ctl_k_snap_mult = "        << hk.ctl_k_snap_mult << "\n"
             << "ctl_min_aspect = "         << hk.ctl_min_aspect << "\n"
             << "ctl_max_aspect = "         << hk.ctl_max_aspect << "\n"
             << "ctl_random_seed = "        << hk.ctl_random_seed << "\n";

        // ── ★★ 自动扳机 (2026-09-17 恢复) ────────────────────────────────
        // 后端在 mouse/trigger_fsm.h + mouse/trigger_scope.h + mouse/auto_stop.h,
        // 接线在 runtime/aim_loop.cpp。
        file << "trigger_enabled = "        << to_bool_str(hk.trigger_enabled) << "\n"
             << "trigger_fire_delay = "     << hk.trigger_fire_delay << "\n"
             << "trigger_fire_duration = "  << hk.trigger_fire_duration << "\n"
             << "trigger_fire_interval = "  << hk.trigger_fire_interval << "\n"
             << "trigger_y_percent = "      << hk.trigger_y_percent << "\n"
             << "trigger_delay_jitter_ms = "    << hk.trigger_delay_jitter_ms << "\n"
             << "trigger_duration_jitter_ms = " << hk.trigger_duration_jitter_ms << "\n"
             << "trigger_interval_jitter_ms = " << hk.trigger_interval_jitter_ms << "\n"
             << "trigger_switch_cooldown_ms = " << hk.trigger_switch_cooldown_ms << "\n"
             << "trigger_auto_scope = "     << hk.trigger_auto_scope << "\n"
             << "trigger_scope_delay_ms = " << hk.trigger_scope_delay_ms << "\n"
             << "trigger_auto_stop = "      << hk.trigger_auto_stop << "\n"
             << "trigger_stop_ms = "        << hk.trigger_stop_ms << "\n";

        // ── ★★ 瞄准轨迹曲线 (2026-09-17 恢复) ────────────────────────────
        // 后端在 mouse/aim_path.h, 接线在 runtime/aim_loop.cpp。
        file << "aim_path_mode = "          << hk.aim_path_mode << "\n"
             << "aim_path_influence = "     << hk.aim_path_influence << "\n"
             << "aim_path_bezier_cx1 = "    << hk.aim_path_bezier_cx1 << "\n"
             << "aim_path_bezier_cy1 = "    << hk.aim_path_bezier_cy1 << "\n"
             << "aim_path_bezier_cx2 = "    << hk.aim_path_bezier_cx2 << "\n"
             << "aim_path_bezier_cy2 = "    << hk.aim_path_bezier_cy2 << "\n"
             << "aim_path_wind_gravity = "  << hk.aim_path_wind_gravity << "\n"
             << "aim_path_wind_wind = "     << hk.aim_path_wind_wind << "\n"
             << "aim_path_wind_step = "     << hk.aim_path_wind_step << "\n"
             << "aim_path_wind_distance = " << hk.aim_path_wind_distance << "\n"
             << "aim_path_wind_threshold = " << hk.aim_path_wind_threshold << "\n";

        file << "\n";
    }

    // Crosshair color palette: one section per entry so users can edit by
    // hand. Order is preserved (matters for the UI list but not detection).
    for (size_t i = 0; i < crosshair_colors.size(); ++i)
    {
        const auto& c = crosshair_colors[i];
        file << "[crosshair_color." << i << "]\n"
             << "name = "    << c.name    << "\n"
             << "enabled = " << to_bool_str(c.enabled) << "\n"
             << "h_low = "   << c.h_low   << "\n"
             << "h_high = "  << c.h_high  << "\n"
             << "s_min = "   << c.s_min   << "\n"
             << "s_max = "   << c.s_max   << "\n"
             << "v_min = "   << c.v_min   << "\n"
             << "v_max = "   << c.v_max   << "\n\n";
    }


    file.close();
    return true;
}

void Config::sync_class_filters_from_model(int class_count,
                                           const std::vector<std::string>& class_names)
{
    std::unordered_map<int, ClassFilterState> keep;
    keep.reserve(class_filters.size());
    for (const auto& cf : class_filters)
        keep[cf.class_id] = cf;

    class_filters.clear();
    class_filters.reserve(static_cast<size_t>(std::max(0, class_count)));

    for (int id = 0; id < class_count; ++id)
    {
        ClassFilterState st;
        auto it = keep.find(id);
        if (it != keep.end())
            st = it->second;
        st.class_id = id;

        if (id < static_cast<int>(class_names.size()) && !class_names[static_cast<size_t>(id)].empty())
            st.class_name = class_names[static_cast<size_t>(id)];
        else if (st.class_name.empty())
            st.class_name = "class_" + std::to_string(id);

        class_filters.push_back(std::move(st));
    }

}
