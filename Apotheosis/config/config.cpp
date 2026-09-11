#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

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
    input_method = get_string("", "input_method", "MAKCU");
    if (input_method != "MAKCU" && input_method != "MAKCUNEW")
        input_method = "MAKCU";
    makcu_baudrate = get_long("", "makcu_baudrate", 115200);
    makcu_port = get_string("", "makcu_port", "COM0");
    // MAKCUNEW固件上电固定115200且不回任何二进制响应帧。
    // 目标速率 != 115200 时由 MakcuNewConnection 发 0x42 SET_BAUD(或 DE AD 转义帧)
    // 后自行重连; 协商失败会自动退回 115200, 因此这里默认取固件允许的上限 6000000。
    makcu_new_baudrate = std::clamp(
        static_cast<int>(get_long("", "makcu_new_baudrate", 6000000)),
        1200, 6000000);
    makcu_new_port = get_string("", "makcu_new_port", "COM0");
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
    crosshair_smooth           = std::clamp(static_cast<float>(get_double("", "crosshair_smooth", 0.5)), 0.0f, 1.0f);

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
            hk.pidf_kf_x = static_cast<float>(get_double(sec, "pidf_kf_x", hk.pidf_kf_x));
            hk.pidf_kf_y = static_cast<float>(get_double(sec, "pidf_kf_y", hk.pidf_kf_y));
            hk.pidf_lr_x = static_cast<float>(get_double(sec, "pidf_lr_x", hk.pidf_lr_x));
            hk.pidf_lr_y = static_cast<float>(get_double(sec, "pidf_lr_y", hk.pidf_lr_y));
            hk.pidf_deadzone_x = static_cast<int>(get_long(sec, "pidf_deadzone_x", hk.pidf_deadzone_x));
            hk.pidf_deadzone_y = static_cast<int>(get_long(sec, "pidf_deadzone_y", hk.pidf_deadzone_y));
            hk.pidf_limit_x = static_cast<int>(get_long(sec, "pidf_limit_x", hk.pidf_limit_x));
            hk.pidf_limit_y = static_cast<int>(get_long(sec, "pidf_limit_y", hk.pidf_limit_y));

            // 旧版界面把四行错误接成 Kp/Ki/Kd/Kf，并隐藏 LR。按用户
            // 看到的行值迁移为 AVA 的 Kp/Kd/Kf/LR，避免运行机沿用旧
            // config.ini 后仍然出现“预测调大只回到框中心”。
            if (hk.pidf_mapping_version < 2)
            {
                const float old_ki_x = hk.pidf_ki_x;
                const float old_ki_y = hk.pidf_ki_y;
                const float old_kd_x = hk.pidf_kd_x;
                const float old_kd_y = hk.pidf_kd_y;
                const float old_kf_x = hk.pidf_kf_x;
                const float old_kf_y = hk.pidf_kf_y;
                hk.pidf_ki_x = 0.0f;
                hk.pidf_ki_y = 0.0f;
                hk.pidf_kd_x = old_ki_x; // 旧“过冲控制”
                hk.pidf_kd_y = old_ki_y;
                hk.pidf_kf_x = old_kd_x; // 旧“锁定强度”
                hk.pidf_kf_y = old_kd_y;
                hk.pidf_lr_x = old_kf_x; // 旧“预测速度”
                hk.pidf_lr_y = old_kf_y;
                hk.pidf_mapping_version = 2;
            }

            // v3: 把"前馈整条关死"的老配置升级到可用值。
            //
            // 为什么需要: pidf_kf_x 默认 0, 而 ff_output = ff_state*dt*kf, 于是
            // kf=0 时前馈完全不出力, pidf_lr_x 调多少都没反应 —— 表现为"追不上
            // 横移目标、摆头后咬不住"。多场景模拟(aim_scenario_sim, 120Hz, 25ms
            // 链路延迟)实测: 出厂默认 kf=0/lr=0 综合分 37.8, 打开前馈后 20.6。
            // 只动这三个"必须配套"的值, 不碰用户自己调的 Kp(瞄准速度)与死区/限幅。
            if (hk.pidf_mapping_version < 3)
            {
                if (hk.pidf_kf_x <= 0.0f && hk.pidf_kf_y <= 0.0f)
                {
                    hk.pidf_kf_x = (hk.pidf_kf_x <= 0.0f) ? 1.0f : hk.pidf_kf_x;
                    hk.pidf_kf_y = (hk.pidf_kf_y <= 0.0f) ? 1.0f : hk.pidf_kf_y;
                    // lr 只在前馈本来就没开时才补, 避免覆盖用户已经选好的值
                    if (hk.pidf_lr_x <= 0.0f) hk.pidf_lr_x = 0.05f;
                    if (hk.pidf_lr_y <= 0.0f) hk.pidf_lr_y = 0.05f;
                    // 微分项在延迟下 kd>=0.1 会发散(实测), 老默认 0.01 又几乎没有
                    // 阻尼; 0.05 是实测的稳健值。
                    if (hk.pidf_kd_x < 0.02f) hk.pidf_kd_x = 0.05f;
                    if (hk.pidf_kd_y < 0.02f) hk.pidf_kd_y = 0.05f;
                }
                hk.pidf_mapping_version = 3;
            }

            hk.lost_target_cache_frames = static_cast<int>(get_long(
                sec, "lost_target_cache_frames", hk.lost_target_cache_frames));

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
        // AVA 的 PIDF 浮点编辑器沿用 QDoubleSpinBox 默认范围 0..99.99；
        // 不再把 Kp/Kd/Kf 截到 10、把 LR 截到 1。
        hk.pidf_kp_x = std::clamp(hk.pidf_kp_x, 0.0f, 99.99f); hk.pidf_kp_y = std::clamp(hk.pidf_kp_y, 0.0f, 99.99f);
        hk.pidf_ki_x = 0.0f; hk.pidf_ki_y = 0.0f;
        hk.pidf_kd_x = std::clamp(hk.pidf_kd_x, 0.0f, 99.99f); hk.pidf_kd_y = std::clamp(hk.pidf_kd_y, 0.0f, 99.99f);
        hk.pidf_kf_x = std::clamp(hk.pidf_kf_x, 0.0f, 99.99f); hk.pidf_kf_y = std::clamp(hk.pidf_kf_y, 0.0f, 99.99f);
        hk.pidf_lr_x = std::clamp(hk.pidf_lr_x, 0.0f, 99.99f); hk.pidf_lr_y = std::clamp(hk.pidf_lr_y, 0.0f, 99.99f);
        hk.pidf_deadzone_x = std::clamp(hk.pidf_deadzone_x, 0, 1000); hk.pidf_deadzone_y = std::clamp(hk.pidf_deadzone_y, 0, 1000);
        hk.pidf_limit_x = std::clamp(hk.pidf_limit_x, 0, 1000); hk.pidf_limit_y = std::clamp(hk.pidf_limit_y, 0, 1000);

        hk.lost_target_cache_frames = std::clamp(hk.lost_target_cache_frames, 0, 240);

        hk.aim_path_mode = std::clamp(hk.aim_path_mode, 0, 2);
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
        hk.trigger_y_percent     = std::clamp(hk.trigger_y_percent,     1, 500);
        hk.trigger_delay_jitter_ms    = std::clamp(hk.trigger_delay_jitter_ms,    0, 500);
        hk.trigger_duration_jitter_ms = std::clamp(hk.trigger_duration_jitter_ms, 0, 500);
        hk.trigger_interval_jitter_ms = std::clamp(hk.trigger_interval_jitter_ms, 0, 500);
        hk.trigger_switch_cooldown_ms = std::clamp(hk.trigger_switch_cooldown_ms, 0, 5000);


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

    file << "# Capture  (只有「采集卡」一种方式; 参数必须来自设备真实能力探测,\n"
            "# 组合对不上会直接报错, 不做任何替换)\n"
        << "capture_device = " << capture_device << "\n"
        << "capture_format = " << capture_format << "\n"
        << "capture_width = " << capture_width << "\n"
        << "capture_height = " << capture_height << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "capture_gpu_decode = " << to_bool_str(capture_gpu_decode) << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "circle_mask = " << to_bool_str(circle_mask) << "\n\n";

    file << "# Hardware / input device\n"
        << "# MAKCU | MAKCUNEW\n"
        << "input_method = " << input_method << "\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n"
        << "makcu_new_baudrate = " << makcu_new_baudrate << "\n"
        << "makcu_new_port = " << makcu_new_port << "\n\n";

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
        << "crosshair_close_radius = "    << crosshair_close_radius    << "\n"
        << "crosshair_smooth = "          << crosshair_smooth          << "\n\n";

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
        file << "pidf_mapping_version = 3\n";
        file << std::fixed << std::setprecision(4)
             << "pidf_kp_x = " << hk.pidf_kp_x << "\n" << "pidf_kp_y = " << hk.pidf_kp_y << "\n"
             << "pidf_ki_x = " << hk.pidf_ki_x << "\n" << "pidf_ki_y = " << hk.pidf_ki_y << "\n"
             << "pidf_kd_x = " << hk.pidf_kd_x << "\n" << "pidf_kd_y = " << hk.pidf_kd_y << "\n"
             << "pidf_kf_x = " << hk.pidf_kf_x << "\n" << "pidf_kf_y = " << hk.pidf_kf_y << "\n"
             << "pidf_lr_x = " << hk.pidf_lr_x << "\n" << "pidf_lr_y = " << hk.pidf_lr_y << "\n"
             << "pidf_deadzone_x = " << hk.pidf_deadzone_x << "\n" << "pidf_deadzone_y = " << hk.pidf_deadzone_y << "\n"
             << "pidf_limit_x = " << hk.pidf_limit_x << "\n" << "pidf_limit_y = " << hk.pidf_limit_y << "\n"
             << std::setprecision(0)
             << "lost_target_cache_frames = " << hk.lost_target_cache_frames << "\n"
             << "trigger_enabled = "       << to_bool_str(hk.trigger_enabled)       << "\n"
             << "trigger_fire_delay = "    << hk.trigger_fire_delay    << "\n"
             << "trigger_fire_duration = " << hk.trigger_fire_duration << "\n"
             << "trigger_fire_interval = " << hk.trigger_fire_interval << "\n"
             << "trigger_y_percent = "     << hk.trigger_y_percent     << "\n"
             << "trigger_delay_jitter_ms = "    << hk.trigger_delay_jitter_ms    << "\n"
             << "trigger_duration_jitter_ms = " << hk.trigger_duration_jitter_ms << "\n"
             << "trigger_interval_jitter_ms = " << hk.trigger_interval_jitter_ms << "\n"
              << "trigger_switch_cooldown_ms = " << hk.trigger_switch_cooldown_ms << "\n"
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
