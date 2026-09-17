#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

constexpr std::uint32_t kCurveMagic = 0x31565243u; // "CRV1"

bool write_curve_asset(const std::filesystem::path& path, const std::vector<float>& samples)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::uint32_t count = static_cast<std::uint32_t>(samples.size());
    out.write(reinterpret_cast<const char*>(&kCurveMagic), sizeof(kCurveMagic));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (float value : samples)
    {
        const auto quantized = static_cast<std::int16_t>(std::lround(
            std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        out.write(reinterpret_cast<const char*>(&quantized), sizeof(quantized));
    }
    return static_cast<bool>(out);
}

std::vector<float> read_curve_asset(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::uint32_t magic = 0, count = 0;
    if (!in.read(reinterpret_cast<char*>(&magic), sizeof(magic)) ||
        !in.read(reinterpret_cast<char*>(&count), sizeof(count)) ||
        magic != kCurveMagic || count < 2 || count > 1000000)
        return {};
    std::vector<float> samples(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::int16_t quantized = 0;
        if (!in.read(reinterpret_cast<char*>(&quantized), sizeof(quantized))) return {};
        samples[i] = static_cast<float>(quantized) / 32767.0f;
    }
    samples.front() = 0.0f;
    samples.back() = 0.0f;
    return samples;
}

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
    max_detections = 100;
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
    use_double_buffer = false;
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
    //   注: 这一项不是"槽位语义变更", 老配置里的 MAKCU/MAKCUNEW 取值含义没变,
    //   所以 **pidf_mapping_version 不推进**。
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
    backend = get_string("", "backend", "TRT");
    dml_device_id = get_long("", "dml_device_id", 0);
    ai_model = get_string("", "ai_model", "sunxds_0.5.6.engine");
    confidence_threshold = static_cast<float>(get_double("", "confidence_threshold", 0.15));
    nms_threshold = static_cast<float>(get_double("", "nms_threshold", 0.50));
    max_detections = get_long("", "max_detections", 100);
    small_target_enabled = get_bool("", "small_target_enabled", false);
    small_target_area_frac = static_cast<float>(get_double("", "small_target_area_frac", 0.012));
    small_target_confidence = static_cast<float>(get_double("", "small_target_confidence", 0.06));
    fixed_input_size = get_bool("", "fixed_input_size", false);

    use_cuda_graph = get_bool("", "use_cuda_graph", true);
    use_double_buffer = get_bool("", "use_double_buffer", false);
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

            // AVA PIDF Mode 1
            hk.pidf_mapping_version = static_cast<int>(get_long(
                sec, "pidf_mapping_version", 1));
            hk.pidf_kp_x = static_cast<float>(get_double(sec, "pidf_kp_x", hk.pidf_kp_x));
            hk.pidf_kp_y = static_cast<float>(get_double(sec, "pidf_kp_y", hk.pidf_kp_y));
            hk.pidf_ki_x = static_cast<float>(get_double(sec, "pidf_ki_x", hk.pidf_ki_x));
            hk.pidf_ki_y = static_cast<float>(get_double(sec, "pidf_ki_y", hk.pidf_ki_y));
            hk.pidf_kd_x = static_cast<float>(get_double(sec, "pidf_kd_x", hk.pidf_kd_x));
            hk.pidf_kd_y = static_cast<float>(get_double(sec, "pidf_kd_y", hk.pidf_kd_y));
            // ★ pidf_kf_* / pidf_lr_* (提前量 / 延迟预测) 已于 2026-09-14 整条删除:
            //   它们在前馈移除后就是纯配置往返, 控制链 0 引用。老配置里的这两个键
            //   读进来会被忽略, 写回时也不再出现(loadFromIni 不做全量保留)。
            // P 项饱和阈值 (原「移动死区」改名, 2026-09-14)。
            // ★ 兼容: 老配置里这个键叫 pidf_deadzone_x/y, 存的是【死区宽度】(3~5px)。
            //   死区语义已删除, 旧值不能再当饱和阈值用: |e|>5px 就全被削成常量,
            //   等效增益变成 Kp*5/|e|, 回路直接软掉 —— 比有死区时更糟。
            //   所以先读旧键, 迁移时把 <=20 的旧值丢弃(见下面 v6 迁移块)。
            hk.pidf_psat_x = static_cast<int>(get_long(sec, "pidf_psat_x",
                static_cast<long>(get_long(sec, "pidf_deadzone_x", hk.pidf_psat_x))));
            hk.pidf_psat_y = static_cast<int>(get_long(sec, "pidf_psat_y",
                static_cast<long>(get_long(sec, "pidf_deadzone_y", hk.pidf_psat_y))));
            hk.pidf_limit_x = static_cast<int>(get_long(sec, "pidf_limit_x", hk.pidf_limit_x));
            hk.pidf_limit_y = static_cast<int>(get_long(sec, "pidf_limit_y", hk.pidf_limit_y));
            // 预测补偿(2026-09-13 重做, 对齐 AimMagic 1.0.30):
            //   系数 0 = 该轴不预测, 与 AM 的 UI 默认值一致, 所以旧配置读进来行为不变。
            hk.pidf_predict_x = static_cast<float>(get_double(sec, "pidf_predict_x", hk.pidf_predict_x));
            hk.pidf_predict_y = static_cast<float>(get_double(sec, "pidf_predict_y", hk.pidf_predict_y));
            hk.pidf_predict_min_w = static_cast<int>(get_long(
                sec, "pidf_predict_min_w", hk.pidf_predict_min_w));
            hk.pidf_predict_max_w = static_cast<int>(get_long(
                sec, "pidf_predict_max_w", hk.pidf_predict_max_w));
            hk.pidf_predict_damp = static_cast<float>(get_double(
                sec, "pidf_predict_damp", hk.pidf_predict_damp));
            // 提前量硬上限与速度噪声门 (2026-09-14 新增, 见 config.h 的长注释)。
            hk.pidf_predict_max_px = static_cast<int>(get_long(
                sec, "pidf_predict_max_px", hk.pidf_predict_max_px));
            hk.pidf_predict_vel_floor = static_cast<int>(get_long(
                sec, "pidf_predict_vel_floor", hk.pidf_predict_vel_floor));
            // 在途补偿强度 beta (无量纲, 2026-09-14 重新启用)。缺键 = 用默认 1.6
            // (结构体默认是 -1 哨兵, 见 config.h)。
            hk.pidf_inflight_x = static_cast<float>(get_double(sec, "pidf_inflight_x", hk.pidf_inflight_x));
            hk.pidf_inflight_y = static_cast<float>(get_double(sec, "pidf_inflight_y", hk.pidf_inflight_y));
            // PID-EventSync (本档唯一链路, 见 config.h 的长注释)。
            // ★ 2026-09-16: 原先的 aim_mode 键已删除 —— 现在只有这一条链路。
            //   老配置里若还写着 aim_mode = 0, 读入时【忽略】(不报错): 那个档位
            //   已不存在, 无法"回到"它。
            hk.esync_min_hits = static_cast<int>(get_long(
                sec, "esync_min_hits", hk.esync_min_hits));
            hk.esync_max_age = static_cast<int>(get_long(
                sec, "esync_max_age", hk.esync_max_age));
            hk.esync_assoc_iou = static_cast<float>(get_double(
                sec, "esync_assoc_iou", hk.esync_assoc_iou));
            hk.esync_vel_sample_ms = static_cast<int>(get_long(
                sec, "esync_vel_sample_ms", hk.esync_vel_sample_ms));
            // 预测补偿 (AM 的 AimKey 作用域)。
            hk.esync_pred_factor_x = static_cast<float>(get_double(
                sec, "esync_pred_factor_x", hk.esync_pred_factor_x));
            hk.esync_pred_factor_y = static_cast<float>(get_double(
                sec, "esync_pred_factor_y", hk.esync_pred_factor_y));
            hk.esync_pred_min_w = static_cast<int>(get_long(
                sec, "esync_pred_min_w", hk.esync_pred_min_w));
            hk.esync_pred_max_w = static_cast<int>(get_long(
                sec, "esync_pred_max_w", hk.esync_pred_max_w));
            // ★★ 迁移 (2026-09-16): 下面这些键在 AM 里【没有对应】, 是此前"按思路
            //    适配"进来的, 已整条删除。老配置里若还写着它们, 读入时【忽略】
            //    (不报错) —— 它们已经没有任何消费者, 无法"回到"那个行为:
            //      esync_assoc_radius_px   (AM 的关联没有距离半径)
            //      esync_vel_window_ms     (AM 键名是 tracking_velocity_sample_ms)
            //      esync_counts_per_pixel_x/y (AM 的 k̂ 只被 FrameSync/EventSync 档消费,
            //                                  且本项目这条链已删除 —— 见 ground-truth §6)
            //      esync_inflight_window_ms / esync_inflight_beta (整条像素域在途链已删)
            //      esync_self_motion_gain  (AM 没有"额外增益", 平台位移直接乘用户系数)
            //    ★ 注意 esync_vel_window_ms 的【旧值 100】不能迁到新键
            //      esync_vel_sample_ms 上 —— 两者语义不同(旧的是"累加窗", 新的是
            //      "沿用旧值的窗"), 直接搬会把默认 20 变成 100。所以新键从默认值起。
            (void)get_long(sec, "esync_assoc_radius_px", 0);
            (void)get_long(sec, "esync_vel_window_ms", 0);
            (void)get_double(sec, "esync_counts_per_pixel_x", 0.0);
            (void)get_double(sec, "esync_counts_per_pixel_y", 0.0);
            (void)get_long(sec, "esync_inflight_window_ms", 0);
            (void)get_double(sec, "esync_inflight_beta", 0.0);
            (void)get_double(sec, "esync_self_motion_gain", 0.0);
            // ★★ 迁移 (2026-09-14): pidf_mapping_version < 5 的配置里, pidf_inflight_x/y
            //    存的是【旧语义】(像素/拍的补偿系数, 或早期"停用"阶段留下的占位 0)。
            //    这两种老值放到新语义(无量纲 beta)下都是错的:
            //      · 老配置普遍是 0.0 —— 新语义下 0 表示"明确关闭补偿" = 拆掉主刹车,
            //        实测尾段会变成几百像素的自持极限环(60fps 直接发散)。
            //      · 即便不是 0, 老值也是"像素/拍"量纲, 与无量纲 beta 差着量级。
            //    所以旧版本一律【重置为哨兵 -1】, 让 pid_params() 回落到生产默认 1.6。
            //    这正是 pidf_mapping_version 存在的意义(与 kf/lr 那两个槽位的先例一致)。
            if (hk.pidf_mapping_version < 5)
            {
                hk.pidf_inflight_x = -1.0f;
                hk.pidf_inflight_y = -1.0f;
            }
            // 尺度增益调度 (2026-09-14)。老配置没有这些键, 取结构体默认(启用/1.5倍)。
            hk.aim_scale_enabled = static_cast<int>(get_long(
                sec, "aim_scale_enabled", hk.aim_scale_enabled));
            hk.aim_scale_max = static_cast<float>(get_double(
                sec, "aim_scale_max", hk.aim_scale_max));
            hk.aim_scale_min = static_cast<float>(get_double(
                sec, "aim_scale_min", hk.aim_scale_min));
            hk.aim_scale_base_h = static_cast<float>(get_double(
                sec, "aim_scale_base_h", hk.aim_scale_base_h));
            // ★ aim_scale_near_h / aim_scale_far_h (两个绝对框高阈值) 已于
            //   2026-09-14 删除 —— 用户测不出当前框高、换个游戏/分辨率就失效。
            //   取代它们的是 aim_scale_base_h(自动学的基准)。老的键不再读取,
            //   配置里的残留值被忽略(见 pidf_mapping_version 的迁移说明)。
            hk.pidf_inflight_window_ms = static_cast<int>(get_long(
                sec, "pidf_inflight_window_ms", hk.pidf_inflight_window_ms));
            // ★ aim_px_per_count_x/y (每计数像素 k̂) 已于 2026-09-14 整条删除:
            //   它是前馈的输入, 前馈删除后就只剩配置往返, 控制链 0 引用。
            //   ★ 更重要的是: 保留这个键会【诱导】后来的人重新引入 k̂ —— 而本项目
            //   是双机架构(游戏机→采集卡→采集机), k̂ 是游戏机的属性, 两条反推路径
            //   都已实测证伪(见 docs/aimmagic-comparison.md §6.7)。控制器必须在
            //   完全不依赖 k̂ 的前提下工作, 配置里根本不该存在这个量。
            // (anchor_filter_ms 已于 2026-09-12 移除: 位置不再平滑, 速度低通由观测器
            //  内部承担。旧 ini 里的这个键会被忽略。)

            // v4: 控制器整条换成 mouse/aim_pid.h 的新 PID —— 这几个槽位的【单位与
            // 含义全变了】, 旧值直接沿用会给出离谱的回路增益:
            //   Kp  旧"每拍增益"        -> 新 计数/(像素*秒)
            //   Ki  旧固定 0            -> 新 1/秒 的积分速率
            //   Kd  旧 计数*秒/像素      -> 新 秒(微分时间)
            //   Kf  旧"前馈强度"(默认 1) -> 新 提前量 秒(默认 0 = 关)
            //   LR  旧"学习率"(默认 0.08)-> 新 延迟预测 秒
            // 所以旧版本配置的这五个值一律回到新默认(旧 Kf=1 会变成 1 秒的提前量, 直接
            // 把准星推到目标前面去); 死区与限幅语义没变, 保留用户设置。
            // v2/v3 那两段"前馈/预测"迁移随旧管线一起删掉了。
            if (hk.pidf_mapping_version < 4)
            {
                const HotkeyProfile fresh;  // 只为取新默认值
                hk.pidf_kp_x = fresh.pidf_kp_x;
                hk.pidf_kp_y = fresh.pidf_kp_y;
                hk.pidf_ki_x = fresh.pidf_ki_x;
                hk.pidf_ki_y = fresh.pidf_ki_y;
                hk.pidf_kd_x = fresh.pidf_kd_x;
                hk.pidf_kd_y = fresh.pidf_kd_y;
                hk.pidf_mapping_version = 4;
            }

            // v4 -> v5 (2026-09-14): 在途补偿槽位的语义从"像素/拍系数"换成无量纲 beta。
            // 具体的重置逻辑写在上面读 pidf_inflight_x/y 的地方; 这里只负责推进版本号,
            // 让下次启动不必再走一遍迁移。★ 顺序上必须在 v4 块【之后】, 否则 v3 的老配置
            // 会被 v4 块把版本号写回 4, 再被这里的 < 5 判断重新迁移一次(结果一样, 但不干净)。
            if (hk.pidf_mapping_version < 5)
                hk.pidf_mapping_version = 5;

            // v5 -> v6 (2026-09-14): 两处语义变更。
            //   ① pidf_deadzone_x/y (死区宽度) -> pidf_psat_x/y (P 项饱和阈值):
            //      老值是 3~5px 量级, 当成饱和阈值用会【静默把回路变软】(见上面读取处
            //      的说明), 所以 <=20 的旧值一律丢弃, 回落到"关闭"(0)。
            //   ② pidf_predict_x/y 的范围从 0..100 收到 0..0.2:
            //      老配置里若存过 >0.2 的值, 那是按旧量纲填的, 直接按新范围夹取会让
            //      它变成 0.2 上限还是别的都无意义 —— 一律【重置为 0(关闭)】更安全,
            //      因为那种配置本来就是错的(会让准星偏出几百像素)。
            if (hk.pidf_mapping_version < 6)
            {
                if (hk.pidf_psat_x > 0 && hk.pidf_psat_x <= 20) hk.pidf_psat_x = 0;
                if (hk.pidf_psat_y > 0 && hk.pidf_psat_y <= 20) hk.pidf_psat_y = 0;
                if (hk.pidf_predict_x > 0.2f || hk.pidf_predict_x < -0.2f) hk.pidf_predict_x = 0.0f;
                if (hk.pidf_predict_y > 0.2f || hk.pidf_predict_y < -0.2f) hk.pidf_predict_y = 0.0f;
                hk.pidf_mapping_version = 6;
            }

            // v6 -> v7 (2026-09-14): 尺度调度的槽位语义变更。
            //   旧: aim_scale_near_h / aim_scale_far_h = 两个【绝对】框高阈值(160 / 45),
            //       把框高映射到 [1.0, s_max] 的对数区间。
            //   新: aim_scale_base_h = 一个【自动学】的基准框高, 公式改成
            //       s = clamp((h/基准)^γ, s_min, s_max)。
            //
            //   旧的两个值是"绝对值阈值", 新的是"用户整定时目标有多大"的记录, 两者
            //   语义完全不同, 不能互换。而且 v6 的 s_min 被硬钉在 1.0(只放大不缩小),
            //   新设计允许远处真的降增益 —— 那个行为差异也该让老配置回到中性。
            //   所以 v6 及更早一律: 基准清 0(= 还没学到 -> 整条链路中性 1.0),
            //   s_min 回默认。用户下次开 agent 调参时基准会被自动学出来。
            //   ★ 不迁移旧阈值: 拿它去当基准是错的(160px 是个拍出来的常数, 不是
            //     用户那个距离上真实的框高)。
            if (hk.pidf_mapping_version < 7)
            {
                hk.aim_scale_base_h = 0.0f;
                hk.aim_scale_min = 1.00f;
                hk.pidf_mapping_version = 7;
            }

            // 扳机
            hk.trigger_enabled        = get_bool(sec, "trigger_enabled",        hk.trigger_enabled);
            hk.trigger_fire_delay     = static_cast<int>(get_long(sec, "trigger_fire_delay",     hk.trigger_fire_delay));
            hk.trigger_fire_duration  = static_cast<int>(get_long(sec, "trigger_fire_duration",  hk.trigger_fire_duration));
            hk.trigger_fire_interval  = static_cast<int>(get_long(sec, "trigger_fire_interval",  hk.trigger_fire_interval));
            hk.trigger_y_percent      = static_cast<int>(get_long(sec, "trigger_y_percent",      hk.trigger_y_percent));
            hk.trigger_delay_jitter_ms    = static_cast<int>(get_long(sec, "trigger_delay_jitter_ms",    hk.trigger_delay_jitter_ms));
            hk.trigger_duration_jitter_ms = static_cast<int>(get_long(sec, "trigger_duration_jitter_ms", hk.trigger_duration_jitter_ms));
            hk.trigger_interval_jitter_ms = static_cast<int>(get_long(sec, "trigger_interval_jitter_ms", hk.trigger_interval_jitter_ms));
            hk.trigger_switch_cooldown_ms = static_cast<int>(get_long(sec, "trigger_switch_cooldown_ms", hk.trigger_switch_cooldown_ms));
            // 自动开镜 (仿 AimMagic 的「开火方式」Click Right / Hold Right)
            hk.trigger_auto_scope = std::clamp(static_cast<int>(
                get_long(sec, "trigger_auto_scope", hk.trigger_auto_scope)), 0, 2);
            hk.trigger_scope_delay_ms = std::clamp(static_cast<int>(
                get_long(sec, "trigger_scope_delay_ms", hk.trigger_scope_delay_ms)), 0, 1000);
            // 自动急停 (开火时补一个反方向键; 只有 MAKCUNEW 有键盘通道)
            hk.trigger_auto_stop = std::clamp(static_cast<int>(
                get_long(sec, "trigger_auto_stop", hk.trigger_auto_stop)), 0, 1);
            hk.trigger_stop_ms = std::clamp(static_cast<int>(
                get_long(sec, "trigger_stop_ms", hk.trigger_stop_ms)), 20, 300);

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

            // 瞄准轨迹曲线
            hk.aim_path_mode        = get_long(sec, "aim_path_mode", hk.aim_path_mode);
            hk.aim_path_influence   = static_cast<int>(get_long(
                sec, "aim_path_influence", hk.aim_path_influence));
            hk.aim_path_bezier_cx1  = static_cast<float>(get_double(sec, "aim_path_bezier_cx1", hk.aim_path_bezier_cx1));
            hk.aim_path_bezier_cy1  = static_cast<float>(get_double(sec, "aim_path_bezier_cy1", hk.aim_path_bezier_cy1));
            hk.aim_path_bezier_cx2  = static_cast<float>(get_double(sec, "aim_path_bezier_cx2", hk.aim_path_bezier_cx2));
            hk.aim_path_bezier_cy2  = static_cast<float>(get_double(sec, "aim_path_bezier_cy2", hk.aim_path_bezier_cy2));
            // WindMouse 曲线 (aim_path_mode = 3) —— 仿 AimMagic enable_mouse_curve。
            hk.aim_path_wind_gravity = static_cast<float>(std::clamp(
                get_double(sec, "aim_path_wind_gravity", hk.aim_path_wind_gravity), 0.0, 200.0));
            hk.aim_path_wind_wind = static_cast<float>(std::clamp(
                get_double(sec, "aim_path_wind_wind", hk.aim_path_wind_wind), 0.0, 200.0));
            hk.aim_path_wind_step = static_cast<float>(std::clamp(
                get_double(sec, "aim_path_wind_step", hk.aim_path_wind_step), 0.1, 200.0));
            hk.aim_path_wind_distance = static_cast<float>(std::clamp(
                get_double(sec, "aim_path_wind_distance", hk.aim_path_wind_distance), 0.1, 200.0));
            hk.aim_path_wind_threshold = std::clamp(static_cast<int>(
                get_long(sec, "aim_path_wind_threshold", hk.aim_path_wind_threshold)), 0, 100);
            {
                std::vector<float> samples;
                const std::string asset = get_string(sec, "aim_path_custom_file", "");
                if (!asset.empty())
                {
                    const auto base = std::filesystem::u8path(target).parent_path();
                    samples = read_curve_asset(base / std::filesystem::u8path(asset));
                }
                const std::string raw = get_string(sec, "aim_path_custom_samples", "");
                if (samples.empty() && !raw.empty())
                {
                    for (const auto& tok : splitString(raw, ','))
                    {
                        try {
                            float v = std::stof(tok);
                            v = std::clamp(v, -1.0f, 1.0f);
                            samples.push_back(v);
                        } catch (...) { /* ignore malformed sample */ }
                    }
                    // Endpoints are pinned to zero so the path actually
                    // ends ON the target — load-time enforcement guards
                    // against hand-edited ini files.
                    if (!samples.empty())
                    {
                        samples.front() = 0.0f;
                        samples.back()  = 0.0f;
                    }
                }
                // 旧密度在加载边界一次性升采样，运行时不再每帧转换。
                if (samples.size() >= 2 &&
                    samples.size() != static_cast<size_t>(HotkeyProfile::kAimPathSampleCount))
                {
                    std::vector<float> expanded(HotkeyProfile::kAimPathSampleCount);
                    for (int i = 0; i < HotkeyProfile::kAimPathSampleCount; ++i)
                    {
                        const double pos = static_cast<double>(i) * (samples.size() - 1)
                                         / (HotkeyProfile::kAimPathSampleCount - 1);
                        const size_t i0 = static_cast<size_t>(std::floor(pos));
                        const size_t i1 = std::min(i0 + 1, samples.size() - 1);
                        const double f = pos - static_cast<double>(i0);
                        expanded[static_cast<size_t>(i)] = static_cast<float>(
                            samples[i0] + (samples[i1] - samples[i0]) * f);
                    }
                    samples = std::move(expanded);
                }
                hk.aim_path_custom_samples =
                    std::make_shared<const std::vector<float>>(std::move(samples));
            }
            hk.aim_path_neural_enabled = get_bool(
                sec, "aim_path_neural_enabled", hk.aim_path_neural_enabled);
            {
                const std::string raw = get_string(sec, "aim_path_neural_weights", "");
                size_t wi = 0;
                for (const auto& token : splitString(raw, ','))
                {
                    if (wi >= hk.aim_path_neural_weights.size()) break;
                    try { hk.aim_path_neural_weights[wi++] = std::stof(token); }
                    catch (...) { hk.aim_path_neural_weights.fill(0.0f); wi = 0; break; }
                }
                if (wi != hk.aim_path_neural_weights.size())
                    hk.aim_path_neural_enabled = false;
            }

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

    auto clamp_aim_fields = [](HotkeyProfile& hk) {
        // 新控制器(mouse/aim_pid.h)的单位与范围: Kp 计数/(像素*秒)、Ki 1/秒、
        // Kd 秒、延迟预测与提前量 秒。上面的 D 盒子上限与这里保持一致。
        hk.pidf_kp_x = std::clamp(hk.pidf_kp_x, 0.0f, 400.0f); hk.pidf_kp_y = std::clamp(hk.pidf_kp_y, 0.0f, 400.0f);
        hk.pidf_ki_x = std::clamp(hk.pidf_ki_x, 0.0f, 60.0f); hk.pidf_ki_y = std::clamp(hk.pidf_ki_y, 0.0f, 60.0f);
        hk.pidf_kd_x = std::clamp(hk.pidf_kd_x, 0.0f, 0.3f); hk.pidf_kd_y = std::clamp(hk.pidf_kd_y, 0.0f, 0.3f);
        // P 项饱和阈值 (原死区): 0 = 关闭; 上限 1000px 远超可用带, 只防手抖填错。
        hk.pidf_psat_x = std::clamp(hk.pidf_psat_x, 0, 1000); hk.pidf_psat_y = std::clamp(hk.pidf_psat_y, 0, 1000);
        hk.pidf_limit_x = std::clamp(hk.pidf_limit_x, 0, 1000); hk.pidf_limit_y = std::clamp(hk.pidf_limit_y, 0, 1000);
        // 预测补偿(2026-09-14 收紧): 系数 ±0.2。合理量级就是 0.05~0.2
        // (提前量 = 系数 × 尺寸权重 × 屏幕速度, 想要"死区 46ms 内目标走的距离"),
        // 旧范围 ±100 会让准星偏出几百到上万像素 —— 那正是 §4.2 禁止的线性瞄偏。
        hk.pidf_predict_x = std::clamp(hk.pidf_predict_x, -0.2f, 0.2f);
        hk.pidf_predict_y = std::clamp(hk.pidf_predict_y, -0.2f, 0.2f);
        // 尺寸区间: 1..2000 像素。保持 minW < maxW, 否则区间为空、预测恒不生效。
        hk.pidf_predict_min_w = std::clamp(hk.pidf_predict_min_w, 1, 2000);
        hk.pidf_predict_max_w = std::clamp(hk.pidf_predict_max_w, 1, 2000);
        if (hk.pidf_predict_max_w <= hk.pidf_predict_min_w)
            hk.pidf_predict_max_w = hk.pidf_predict_min_w + 1;
        // 方向翻转阻尼: 0..1, 1 = 不阻尼(AM 内部常数, 这里给可调)。
        hk.pidf_predict_damp = std::clamp(hk.pidf_predict_damp, 0.0f, 1.0f);
        // 提前量硬上限: 0..64px。0 = 用内置默认(12px)。★ 这是"从物理量导出的硬上限",
        // 不是可调力度旋钮 —— 放开它就等于允许稳态瞄偏随速度增长。
        hk.pidf_predict_max_px = std::clamp(hk.pidf_predict_max_px, 0, 64);
        // 速度噪声门: 0..1000 px/s。实测静止目标 v̂ 噪声 p99=46 / max=52, 默认 60。
        hk.pidf_predict_vel_floor = std::clamp(hk.pidf_predict_vel_floor, 0, 1000);
        // 在途补偿强度 beta (无量纲, 2026-09-14 重新启用)。
        // ★ 允许负值通过: 负值是"没填, 用默认 1.6"的哨兵, 被 clamp 到 0 就会
        //   静默关掉补偿(= 拆掉主刹车, 实测尾段会变成几百像素的极限环)。
        //   只挡掉非有限值; 合法范围的上限交给 AimPid::configure 去夹。
        if (!std::isfinite(hk.pidf_inflight_x)) hk.pidf_inflight_x = -1.0f;
        if (!std::isfinite(hk.pidf_inflight_y)) hk.pidf_inflight_y = -1.0f;
        // 窗口: 只要是个合理正数就留着(运行时不用它, 直接用 kAimDeadTimeS)。
        hk.pidf_inflight_window_ms = std::clamp(hk.pidf_inflight_window_ms, 1, 500);
        // 尺度增益调度 (2026-09-14)。
        // ★ 2026-09-14 改版: s_min 现在【允许 < 1.0】。上一版把它钉在 1.0, 理由是
        //   "降增益会让远处更跟不上"; 那个论证隐含假设了像素速度固定, 而远处目标的
        //   像素速度本来就小(v_px ≈ f·v_world/d), 按框高等比缩放才是正确的距离补偿
        //   (用户纠正)。所以下界放开到 0.30, 默认仍是 1.0(不改变现有行为)。
        //   s_max 上界 2.0 是界面范围, 真正的稳定性上限由 Kp x s_max <= g_crit 决定。
        hk.aim_scale_enabled = (hk.aim_scale_enabled != 0) ? 1 : 0;
        if (!std::isfinite(hk.aim_scale_max)) hk.aim_scale_max = 1.50f;
        if (!std::isfinite(hk.aim_scale_min)) hk.aim_scale_min = 1.00f;
        // ★ 顺序: 先各自夹到合法域, 再处理两端交叉。
        //   s_min 的域是 [0.30, 1.0], s_max 的域是 [1.0, 2.0], 两者只在 1.0 相接,
        //   所以夹完之后 min <= max 几乎总是成立; 唯一交叉的情形是 min 夹到 1.0
        //   而 max 也是 1.0(不算交叉, 退化成常数 1.0 = 中性), 或者用户填了
        //   min=1.0 / max=1.0。此时区间退化成一点, 等价于关闭尺度 —— 安全。
        //   ★ 反过来写(先比大小再夹取)才是错的: 那样 min=1.8/max=1.2 会先被
        //   "压到 max", 再被夹到 [0.30,1.0] 变成 1.0, 得到一个用户没填过的区间。
        hk.aim_scale_max = std::clamp(hk.aim_scale_max, 1.0f, 2.0f);
        hk.aim_scale_min = std::clamp(hk.aim_scale_min, 0.30f, 1.0f);
        // 交叉保护: 夹取之后仍可能出现 min > max 吗? 不会(两域只相接于 1.0)。
        // 这里纯属防御, 保证区间恒非空 —— 空区间会让映射函数退化成常数。
        if (hk.aim_scale_min > hk.aim_scale_max)
            hk.aim_scale_min = hk.aim_scale_max;
        // 基准框高: 0 是合法的("还没学到" -> 中性)。非法值一律回落到 0。
        if (!std::isfinite(hk.aim_scale_base_h)
            || hk.aim_scale_base_h < 0.0f || hk.aim_scale_base_h > 4000.0f)
            hk.aim_scale_base_h = 0.0f;
        // 太小的基准没意义(噪声框), 会让倍率爆掉; 当作"还没学到"。
        if (hk.aim_scale_base_h > 0.0f && hk.aim_scale_base_h < 4.0f)
            hk.aim_scale_base_h = 0.0f;

        // ─ PID-EventSync (本档唯一链路) ─────────────────────────────────────
        // ★★ 2026-09-16: 夹取域已按 AM 1.0.30 重写(ground-truth §2.1/§2.2)。
        //    原则: AM 的解析器【没有夹取】的键, 这里也不夹(只挡 NaN/Inf);
        //    AM 有夹取的键, 用 AM 的域。
        if (!std::isfinite(hk.esync_assoc_iou))
            hk.esync_assoc_iou = 0.30f;
        // IoU 阈值: 0..1(AM 的 UI from/to 就是 0..1)。0 = 关联那道闸关掉。
        hk.esync_assoc_iou = std::clamp(hk.esync_assoc_iou, 0.0f, 1.0f);
        // min_hits: 1..30。AM 默认 3, 本项目沿用同值。
        hk.esync_min_hits = std::clamp(hk.esync_min_hits, 1, 30);
        // max_age: 1..60 帧。AM 默认 5。
        hk.esync_max_age = std::clamp(hk.esync_max_age, 1, 60);
        // 速度采样窗: ★ AM 的解析器夹取域就是 [1, 1000](anchors.txt L21702-21711),
        //   照抄。默认 20ms(= AM)。
        hk.esync_vel_sample_ms = std::clamp(hk.esync_vel_sample_ms, 1, 1000);

        // 预测补偿: ★ AM 对 prediction_factor_x/y 【无夹取】(ground-truth §2.2),
        //   所以这里只挡非有限值, 不夹 ±0.2(那是此前自加的, 已删除)。
        //   ★★ 但本项目【有意】保留一道 ±1.0 的宽夹取 —— 任务书 §4.2 要求提前量
        //   有界, 而 1.0 是"提前量 = 平台位移"这个物理意义的自然上限(再大就是
        //   放大, 不再是补偿)。默认 0(关闭)时与 AM 逐位一致, 所以这不是行为偏差,
        //   是一个只在用户主动填超范围值时才生效的安全网。
        if (!std::isfinite(hk.esync_pred_factor_x)) hk.esync_pred_factor_x = 0.0f;
        if (!std::isfinite(hk.esync_pred_factor_y)) hk.esync_pred_factor_y = 0.0f;
        hk.esync_pred_factor_x = std::clamp(hk.esync_pred_factor_x, -1.0f, 1.0f);
        hk.esync_pred_factor_y = std::clamp(hk.esync_pred_factor_y, -1.0f, 1.0f);
        // 尺寸区间: 只保证 max > min(AM 原文 iVar16 = max(min+1, max), 跟踪器里再兜)。
        // ★ 下限 ≥ 1: 权重公式的分母是 (max - min), 且 min 参与 `min < h` 比较,
        //   取 0 时语义上等于"任何正框高都进区间", 不是 AM 的用法。
        hk.esync_pred_min_w = std::clamp(hk.esync_pred_min_w, 1, 4000);
        hk.esync_pred_max_w = std::clamp(hk.esync_pred_max_w, 1, 4000);
        if (hk.esync_pred_max_w <= hk.esync_pred_min_w)
            hk.esync_pred_max_w = hk.esync_pred_min_w + 1;

        // 0 直线 / 1 贝塞尔 / 2 自定义 / 3 WindMouse。★ 上限必须跟着枚举一起改,
        // 否则填了 3 的方案会在加载时被静默降级回"自定义手绘"。
        hk.aim_path_mode = std::clamp(hk.aim_path_mode, 0, 3);
        hk.aim_path_influence = std::clamp(hk.aim_path_influence, 0, 100);
        hk.aim_path_bezier_cx1 = std::clamp(hk.aim_path_bezier_cx1, 0.0f, 1.0f);
        hk.aim_path_bezier_cx2 = std::clamp(hk.aim_path_bezier_cx2, 0.0f, 1.0f);
        hk.aim_path_bezier_cy1 = std::clamp(hk.aim_path_bezier_cy1, -1.0f, 1.0f);
        hk.aim_path_bezier_cy2 = std::clamp(hk.aim_path_bezier_cy2, -1.0f, 1.0f);
        if (hk.aim_path_bezier_cx1 > hk.aim_path_bezier_cx2)
        {
            std::swap(hk.aim_path_bezier_cx1, hk.aim_path_bezier_cx2);
            std::swap(hk.aim_path_bezier_cy1, hk.aim_path_bezier_cy2);
        }

        // 扳机 clamp (duration 允许 0 = 长按模式, 见 config.h)
        hk.trigger_fire_delay    = std::clamp(hk.trigger_fire_delay,    0, 5000);
        hk.trigger_fire_duration = std::clamp(hk.trigger_fire_duration, 0, 5000);
        hk.trigger_fire_interval = std::clamp(hk.trigger_fire_interval, 0, 5000);
        hk.trigger_y_percent     = std::clamp(hk.trigger_y_percent,     1, 1000);
        hk.trigger_delay_jitter_ms    = std::clamp(hk.trigger_delay_jitter_ms,    0, 500);
        hk.trigger_duration_jitter_ms = std::clamp(hk.trigger_duration_jitter_ms, 0, 500);
        hk.trigger_interval_jitter_ms = std::clamp(hk.trigger_interval_jitter_ms, 0, 500);
        hk.trigger_switch_cooldown_ms = std::clamp(hk.trigger_switch_cooldown_ms, 0, 5000);
        hk.trigger_auto_scope         = std::clamp(hk.trigger_auto_scope, 0, 2);
        hk.trigger_scope_delay_ms     = std::clamp(hk.trigger_scope_delay_ms, 0, 1000);
        hk.trigger_auto_stop          = std::clamp(hk.trigger_auto_stop, 0, 1);
        hk.trigger_stop_ms            = std::clamp(hk.trigger_stop_ms, 20, 300);


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
    };
    for (auto& hk : hotkeys)
        clamp_aim_fields(hk);

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

    file << "# AI\n"
        << "backend = " << backend << "\n"
        << "dml_device_id = " << dml_device_id << "\n"
        << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << std::setprecision(0)
        << "max_detections = " << max_detections << "\n"
        << "small_target_enabled = " << to_bool_str(small_target_enabled) << "\n"
        << std::setprecision(3)
        << "small_target_area_frac = " << small_target_area_frac << "\n"
        << std::setprecision(2)
        << "small_target_confidence = " << small_target_confidence << "\n"
        << std::setprecision(0)
        << "fixed_input_size = " << to_bool_str(fixed_input_size) << "\n\n";

    file << "# CUDA / system\n"
        << "use_cuda_graph = " << to_bool_str(use_cuda_graph) << "\n"
        << "use_double_buffer = " << to_bool_str(use_double_buffer) << "\n"
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
        // 必须写当前版本号: 加载侧按它判断要不要跑迁移。这里写死 3 而加载侧要求 <4 时
        // 迁移, 结果就是"每次启动都把用户调好的 Kp/Ki/Kd/提前量/延迟预测重置成默认"
        // (2026-09-12 实测: 配置文件里 Kp_x=100, 重启后会被改成 20)。
        file << "pidf_mapping_version = 7\n";
        file << std::fixed << std::setprecision(4)
             << "pidf_kp_x = " << hk.pidf_kp_x << "\n" << "pidf_kp_y = " << hk.pidf_kp_y << "\n"
             << "pidf_ki_x = " << hk.pidf_ki_x << "\n" << "pidf_ki_y = " << hk.pidf_ki_y << "\n"
             << "pidf_kd_x = " << hk.pidf_kd_x << "\n" << "pidf_kd_y = " << hk.pidf_kd_y << "\n"
             // P 项饱和阈值 (原「移动死区」, 2026-09-14 改名)
             << "pidf_psat_x = " << hk.pidf_psat_x << "\n" << "pidf_psat_y = " << hk.pidf_psat_y << "\n"
             << "pidf_limit_x = " << hk.pidf_limit_x << "\n" << "pidf_limit_y = " << hk.pidf_limit_y << "\n"
             << "pidf_predict_x = " << hk.pidf_predict_x << "\n"
             << "pidf_predict_y = " << hk.pidf_predict_y << "\n"
             << "pidf_predict_min_w = " << hk.pidf_predict_min_w << "\n"
             << "pidf_predict_max_w = " << hk.pidf_predict_max_w << "\n"
             << "pidf_predict_damp = " << hk.pidf_predict_damp << "\n"
             << "pidf_predict_max_px = " << hk.pidf_predict_max_px << "\n"
             << "pidf_predict_vel_floor = " << hk.pidf_predict_vel_floor << "\n"
             << "pidf_inflight_x = " << hk.pidf_inflight_x << "\n"
             << "pidf_inflight_y = " << hk.pidf_inflight_y << "\n"
             << "pidf_inflight_window_ms = " << hk.pidf_inflight_window_ms << "\n"
             // PID-EventSync (本档唯一链路; 原 aim_mode 键已于 2026-09-16 删除)。
             << "esync_min_hits = " << hk.esync_min_hits << "\n"
             << "esync_max_age = " << hk.esync_max_age << "\n"
             << "esync_assoc_iou = " << hk.esync_assoc_iou << "\n"
             << "esync_vel_sample_ms = " << hk.esync_vel_sample_ms << "\n"
             << "esync_pred_factor_x = " << hk.esync_pred_factor_x << "\n"
             << "esync_pred_factor_y = " << hk.esync_pred_factor_y << "\n"
             << "esync_pred_min_w = " << hk.esync_pred_min_w << "\n"
             << "esync_pred_max_w = " << hk.esync_pred_max_w << "\n"
             // 尺度增益调度 (2026-09-14 新增; 同日改为"单基准"设计)
             << "aim_scale_enabled = " << hk.aim_scale_enabled << "\n"
             << "aim_scale_max = " << hk.aim_scale_max << "\n"
             << "aim_scale_min = " << hk.aim_scale_min << "\n"
             << "aim_scale_base_h = " << hk.aim_scale_base_h << "\n"
             << std::setprecision(0)
             << "trigger_enabled = "       << to_bool_str(hk.trigger_enabled)       << "\n"
             << "trigger_fire_delay = "    << hk.trigger_fire_delay    << "\n"
             << "trigger_fire_duration = " << hk.trigger_fire_duration << "\n"
             << "trigger_fire_interval = " << hk.trigger_fire_interval << "\n"
             << "trigger_y_percent = "     << hk.trigger_y_percent     << "\n"
             << "trigger_delay_jitter_ms = "    << hk.trigger_delay_jitter_ms    << "\n"
             << "trigger_duration_jitter_ms = " << hk.trigger_duration_jitter_ms << "\n"
             << "trigger_interval_jitter_ms = " << hk.trigger_interval_jitter_ms << "\n"
              << "trigger_switch_cooldown_ms = " << hk.trigger_switch_cooldown_ms << "\n"
             // 自动开镜: 0 关 / 1 点按右键(切换) / 2 长按右键(按住)
             << "trigger_auto_scope = "      << hk.trigger_auto_scope      << "\n"
             << "trigger_scope_delay_ms = "  << hk.trigger_scope_delay_ms  << "\n"
             // 自动急停: 0 关 / 1 开; stop_ms = 反方向键短按时长
             << "trigger_auto_stop = "       << hk.trigger_auto_stop       << "\n"
             << "trigger_stop_ms = "         << hk.trigger_stop_ms         << "\n"
              << "aim_classes = "       << serialize_aim_classes(hk.aim_classes) << "\n"
             << std::setprecision(0)
             << "crosshair_detect_enabled = "  << to_bool_str(hk.crosshair_detect_enabled)  << "\n"
             << "dynamic_fov_enabled = " << to_bool_str(hk.dynamic_fov_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "dynamic_fov_strength = " << hk.dynamic_fov_strength << "\n"
             << "aim_path_mode = " << hk.aim_path_mode << "\n"
             << "aim_path_influence = " << hk.aim_path_influence << "\n"
             << std::fixed << std::setprecision(4)
             << "aim_path_bezier_cx1 = " << hk.aim_path_bezier_cx1 << "\n"
             << "aim_path_bezier_cy1 = " << hk.aim_path_bezier_cy1 << "\n"
             << "aim_path_bezier_cx2 = " << hk.aim_path_bezier_cx2 << "\n"
             << "aim_path_bezier_cy2 = " << hk.aim_path_bezier_cy2 << "\n";
        // WindMouse 曲线 (aim_path_mode = 3)。AM 的 wind_mouse_* 同名同量纲。
        file << std::fixed << std::setprecision(3)
             << "aim_path_wind_gravity = "   << hk.aim_path_wind_gravity   << "\n"
             << "aim_path_wind_wind = "      << hk.aim_path_wind_wind      << "\n"
             << "aim_path_wind_step = "      << hk.aim_path_wind_step      << "\n"
             << "aim_path_wind_distance = "  << hk.aim_path_wind_distance  << "\n"
             << std::setprecision(0)
             << "aim_path_wind_threshold = " << hk.aim_path_wind_threshold << "\n"
             << std::setprecision(4);
        if (hk.aim_path_custom_samples && !hk.aim_path_custom_samples->empty())
        {
            const std::string assetName = "hotkey_" + std::to_string(i) + ".curve";
            const std::string assetDirName = targetPath.stem().u8string() + ".curves";
            const auto assetPath = targetPath.parent_path()
                / std::filesystem::u8path(assetDirName)
                / std::filesystem::u8path(assetName);
            if (write_curve_asset(assetPath, *hk.aim_path_custom_samples))
            {
                file << "aim_path_custom_file = " << assetDirName << '/' << assetName << "\n";
            }
            else
            {
                // 二进制资产写入失败时保留文本回退，不丢用户曲线。
                file << "aim_path_custom_samples = ";
                for (size_t si = 0; si < hk.aim_path_custom_samples->size(); ++si)
                {
                    if (si > 0) file << ',';
                    file << (*hk.aim_path_custom_samples)[si];
                }
                file << "\n";
            }
        }
        file << "aim_path_neural_enabled = " << to_bool_str(hk.aim_path_neural_enabled) << "\n";
        if (hk.aim_path_neural_enabled)
        {
            file << "aim_path_neural_weights = ";
            for (size_t wi = 0; wi < hk.aim_path_neural_weights.size(); ++wi)
            {
                if (wi > 0) file << ',';
                file << std::setprecision(9) << hk.aim_path_neural_weights[wi];
            }
            file << std::setprecision(4) << "\n";
        }

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
