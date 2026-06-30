#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
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
    capture_method = "udp_capture";
    udp_ip = "0.0.0.0";
    udp_port = 1234;
    tcp_ip = "0.0.0.0";
    tcp_port = 1235;
    eth_adapter = "";
    eth_ethertype = 0x88B5;
    capture_crop = 0;
    capture_format = "MJPG";
    capture_mf_gpu = true;
    detection_resolution = 320;
    capture_fps = 60;
    circle_mask = true;

    backend = "TRT";
    ai_model = "sunxds_0.5.6.engine";
    confidence_threshold = 0.10f;
    nms_threshold = 0.50f;
    max_detections = 100;
    small_target_enabled = false;
    small_target_area_frac = 0.012f;
    small_target_confidence = 0.06f;
    export_enable_fp8 = false;
    export_enable_fp16 = true;
    fixed_input_size = false;

    use_cuda_graph = true;
    use_double_buffer = true;
    use_pinned_memory = false;
    gpuMemoryReserveMB = 2048;
    enableGpuExclusiveMode = true;
    capture_use_cuda = true;

    cpuCoreReserveCount = 4;
    systemMemoryReserveMB = 2048;

    overlay_opacity = 240;
    overlay_ui_scale = 1.0f;

    auth_require_online = true;
    auth_server_url = "http://110.42.232.243:8787";

    depth_inference_enabled = true;
    depth_model_path = "depth_anything_v2.engine";
    depth_opt_input_size = 224;
    depth_norm_clip_low_pct = 0.0f;
    depth_norm_clip_high_pct = 100.0f;
    depth_mask_fps = 5;

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
    capture_method = get_string("", "capture_method", "udp_capture");
    // 旧版本持久化的各种采集卡后端迁移到当前两套实现:
    //   裸 capture_card / _cv / _ds -> opencv_capture(cv::VideoCapture)
    //   capture_card_mf            -> mf_capture(自写 Media Foundation)
    if (capture_method == "capture_card"
        || capture_method == "capture_card_cv"
        || capture_method == "capture_card_ds")
        capture_method = "opencv_capture";
    if (capture_method == "capture_card_mf")
        capture_method = "mf_capture";
    if (capture_method != "udp_capture" && capture_method != "tcp_capture"
        && capture_method != "eth_capture"
        && capture_method != "opencv_capture" && capture_method != "mf_capture")
        capture_method = "udp_capture";
    udp_ip = get_string("", "udp_ip", "0.0.0.0");
    udp_port = get_long("", "udp_port", 1234);
    if (udp_port < 1 || udp_port > 65535) udp_port = 1234;
    tcp_ip = get_string("", "tcp_ip", "0.0.0.0");
    tcp_port = get_long("", "tcp_port", 1235);
    if (tcp_port < 1 || tcp_port > 65535) tcp_port = 1235;
    eth_adapter = get_string("", "eth_adapter", "");
    eth_ethertype = (int)get_long("", "eth_ethertype", 0x88B5);
    if (eth_ethertype < 0x0600 || eth_ethertype > 0xFFFF) eth_ethertype = 0x88B5;
    opencv_capture_index = get_long("", "opencv_capture_index", 0);
    if (opencv_capture_index < 0) opencv_capture_index = 0;
    opencv_capture_api = get_string("", "opencv_capture_api", "DSHOW");
    if (opencv_capture_api != "DSHOW" && opencv_capture_api != "MSMF"
        && opencv_capture_api != "FFMPEG" && opencv_capture_api != "ANY")
        opencv_capture_api = "DSHOW";
    opencv_capture_url = get_string("", "opencv_capture_url", "");
    opencv_capture_width = get_long("", "opencv_capture_width", 0);
    opencv_capture_height = get_long("", "opencv_capture_height", 0);
    opencv_capture_fps = get_long("", "opencv_capture_fps", 0);
    if (opencv_capture_width < 0) opencv_capture_width = 0;
    if (opencv_capture_height < 0) opencv_capture_height = 0;
    if (opencv_capture_fps < 0) opencv_capture_fps = 0;
    capture_crop = get_long("", "capture_crop", 0);
    if (capture_crop < 0) capture_crop = 0;
    if (capture_crop > 0) capture_crop = std::clamp(capture_crop, 32, 2048);
    capture_format = get_string("", "capture_format", "MJPG");
    if (capture_format != "NV12" && capture_format != "MJPG"
        && capture_format != "YUY2" && capture_format != "RGB32")
        capture_format = "MJPG";
    capture_mf_gpu = get_bool("", "capture_mf_gpu", true);
    detection_resolution = std::clamp(get_long("", "detection_resolution", 320), 32, 2048);
    capture_fps = get_long("", "capture_fps", 60);
    circle_mask = get_bool("", "circle_mask", true);

    // ---------- Hardware ----------
    input_method = get_string("", "input_method", "WIN32");
    arduino_baudrate = get_long("", "arduino_baudrate", 115200);
    arduino_port = get_string("", "arduino_port", "COM0");
    arduino_16_bit_mouse = get_bool("", "arduino_16_bit_mouse", false);
    arduino_enable_keys = get_bool("", "arduino_enable_keys", false);
    kmbox_net_ip = get_string("", "kmbox_net_ip", "10.42.42.42");
    kmbox_net_port = get_string("", "kmbox_net_port", "1984");
    kmbox_net_uuid = get_string("", "kmbox_net_uuid", "DEADC0DE");
    kmbox_a_pidvid = get_string("", "kmbox_a_pidvid", "");
    makcu_baudrate = get_long("", "makcu_baudrate", 115200);
    makcu_port = get_string("", "makcu_port", "COM0");
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
    export_enable_fp8 = get_bool("", "export_enable_fp8", false);
    export_enable_fp16 = get_bool("", "export_enable_fp16", true);
    fixed_input_size = get_bool("", "fixed_input_size", false);

    use_cuda_graph = get_bool("", "use_cuda_graph", true);
    use_double_buffer = get_bool("", "use_double_buffer", true);
    use_pinned_memory = get_bool("", "use_pinned_memory", true);
    gpuMemoryReserveMB = get_long("", "gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("", "enableGpuExclusiveMode", true);
    capture_use_cuda = get_bool("", "capture_use_cuda", true);

    cpuCoreReserveCount = get_long("", "cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("", "systemMemoryReserveMB", 2048);

    // ---------- Overlay ----------
    overlay_opacity = get_long("", "overlay_opacity", 240);
    overlay_ui_scale = static_cast<float>(get_double("", "overlay_ui_scale", 1.0));

    // ---------- Authorization ----------
    auth_require_online = get_bool("", "auth_require_online", true);
    auth_server_url = "http://110.42.232.243:8787";

    // ---------- Depth ----------
    depth_inference_enabled = get_bool("", "depth_inference_enabled", true);
    depth_model_path = get_string("", "depth_model_path", "depth_anything_v2.engine");
    depth_opt_input_size = std::clamp(get_long("", "depth_opt_input_size", 224), 160, 640);
    depth_norm_clip_low_pct = std::clamp(
        static_cast<float>(get_double("", "depth_norm_clip_low_pct", 0.0)),
        0.0f, 50.0f);
    depth_norm_clip_high_pct = std::clamp(
        static_cast<float>(get_double("", "depth_norm_clip_high_pct", 100.0)),
        50.0f, 100.0f);
    depth_mask_fps = std::max(0, get_long("", "depth_mask_fps", 5));

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
    laser_rect_w               = std::clamp(get_long("", "laser_rect_w",  160), 4, 4096);
    laser_rect_h               = std::clamp(get_long("", "laser_rect_h",  240), 4, 4096);
    laser_center_x             = std::clamp(get_long("", "laser_center_x", 160), 0, 8192);
    laser_center_y             = std::clamp(get_long("", "laser_center_y", 200), 0, 8192);
    laser_min_pixel_count      = std::clamp(get_long("", "laser_min_pixel_count", 10), 1, 10000);
    laser_close_radius         = std::clamp(get_long("", "laser_close_radius",     1), 0, 9);
    laser_min_elongation       = std::clamp(static_cast<float>(get_double("", "laser_min_elongation", 3.0)), 1.0f, 30.0f);
    laser_smooth               = std::clamp(static_cast<float>(get_double("", "laser_smooth", 0.5)), 0.0f, 1.0f);
    laser_target_center_x      = std::clamp(get_long("", "laser_target_center_x", 160), 0, 8192);
    laser_target_center_y      = std::clamp(get_long("", "laser_target_center_y", 160), 0, 8192);
    laser_target_rect_w        = std::clamp(get_long("", "laser_target_rect_w",    60), 4, 4096);
    laser_target_rect_h        = std::clamp(get_long("", "laser_target_rect_h",    60), 4, 4096);

    // ---- Flashlight halo detector ----
    flashlight_show_preview      = get_bool("",   "flashlight_show_preview", false);
    flashlight_sensitivity       = std::clamp(get_long("", "flashlight_sensitivity",     50), 0, 100);
    flashlight_reject_strength   = std::clamp(get_long("", "flashlight_reject_strength", 50), 0, 100);
    flashlight_spot_size         = std::clamp(get_long("", "flashlight_spot_size",       50), 0, 100);

    // ---- Glass filter ----
    glass_filter_show_preview  = get_bool("",   "glass_filter_show_preview", false);
    glass_filter_strength      = std::clamp(static_cast<int>(get_long("", "glass_filter_strength", 50)), 0, 100);

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

    laser_colors.clear();
    {
        // Each [laser_color.N] section = one HSV band in the laser palette
        // (independent from crosshair_color.*).
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> lc_sections;
        const std::string prefix = "laser_color.";
        for (const auto& s : sections)
        {
            std::string sname = s.pItem;
            if (sname.rfind(prefix, 0) != 0) continue;
            int idx = 0;
            try { idx = std::stoi(sname.substr(prefix.size())); }
            catch (...) { continue; }
            lc_sections.emplace_back(idx, std::move(sname));
        }
        std::sort(lc_sections.begin(), lc_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : lc_sections)
        {
            const char* sec = entry.second.c_str();
            CrosshairColorProfileConfig c;
            c.name    = get_string(sec, "name", "Color");
            c.enabled = get_bool(sec, "enabled", true);
            c.h_low   = std::clamp(get_long(sec, "h_low",   0),   0, 179);
            c.h_high  = std::clamp(get_long(sec, "h_high",  10),  0, 179);
            c.s_min   = std::clamp(get_long(sec, "s_min",   45),  0, 255);
            c.s_max   = std::clamp(get_long(sec, "s_max",   255), 0, 255);
            c.v_min   = std::clamp(get_long(sec, "v_min",   50),  0, 255);
            c.v_max   = std::clamp(get_long(sec, "v_max",   255), 0, 255);
            laser_colors.push_back(std::move(c));
        }
        if (laser_colors.empty())
        {
            // Seed with a red double-band (laser sights are usually red); the
            // user can recolour / add bands on the laser panel.
            CrosshairColorProfileConfig low;
            low.name = "Laser-Red-Low";  low.h_low = 0;   low.h_high = 10;
            low.s_min = 45; low.v_min = 50;
            CrosshairColorProfileConfig hi;
            hi.name  = "Laser-Red-High"; hi.h_low  = 160; hi.h_high  = 179;
            hi.s_min = 45; hi.v_min = 50;
            laser_colors.push_back(std::move(low));
            laser_colors.push_back(std::move(hi));
        }
    }

    glass_colors.clear();
    {
        // Each [glass_color.N] section = one HSV band in the glass-film
        // palette (独立于 crosshair / laser palette)。
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> gc_sections;
        const std::string prefix = "glass_color.";
        for (const auto& s : sections)
        {
            std::string sname = s.pItem;
            if (sname.rfind(prefix, 0) != 0) continue;
            int idx = 0;
            try { idx = std::stoi(sname.substr(prefix.size())); }
            catch (...) { continue; }
            gc_sections.emplace_back(idx, std::move(sname));
        }
        std::sort(gc_sections.begin(), gc_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : gc_sections)
        {
            const char* sec = entry.second.c_str();
            CrosshairColorProfileConfig c;
            c.name    = get_string(sec, "name", "Glass");
            c.enabled = get_bool(sec, "enabled", true);
            c.h_low   = std::clamp(get_long(sec, "h_low",   90),  0, 179);
            c.h_high  = std::clamp(get_long(sec, "h_high",  115), 0, 179);
            c.s_min   = std::clamp(get_long(sec, "s_min",   5),   0, 255);
            c.s_max   = std::clamp(get_long(sec, "s_max",   90),  0, 255);
            c.v_min   = std::clamp(get_long(sec, "v_min",   170), 0, 255);
            c.v_max   = std::clamp(get_long(sec, "v_max",   255), 0, 255);
            glass_colors.push_back(std::move(c));
        }
        if (glass_colors.empty())
        {
            // 默认双带:浅蓝 + 浅绿薄膜。低 S 高 V 是玻璃膜区别于人物 /
            // 背景物体的关键特征。
            CrosshairColorProfileConfig blue;
            blue.name = "Glass-Blue";
            blue.h_low = 90; blue.h_high = 115;
            blue.s_min = 5;  blue.s_max  = 90;
            blue.v_min = 170;blue.v_max  = 255;
            CrosshairColorProfileConfig green;
            green.name = "Glass-Green";
            green.h_low = 55; green.h_high = 85;
            green.s_min = 5;  green.s_max  = 90;
            green.v_min = 170;green.v_max  = 255;
            glass_colors.push_back(std::move(blue));
            glass_colors.push_back(std::move(green));
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
    // Synthetic flashlight aim class — keep it present even on a fresh config
    // or one saved before this feature existed.
    ensure_flashlight_class();

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

            // ART 瞄准参数
            hk.speed_x         = static_cast<float>(get_double(sec, "speed_x",         hk.speed_x));
            hk.speed_y         = static_cast<float>(get_double(sec, "speed_y",         hk.speed_y));
            hk.dead_zone_px    = static_cast<float>(get_double(sec, "dead_zone_px",    hk.dead_zone_px));
            hk.mover_kind      = static_cast<int>(get_long(sec, "mover_kind",          hk.mover_kind));

            // 疾风 (Predictive)。旧 ini 里的 predictive_pred_weight_x 自动迁移成 pred_weight。
            hk.predictive_kp_x        = static_cast<float>(get_double(sec, "predictive_kp_x",        hk.predictive_kp_x));
            hk.predictive_kp_y        = static_cast<float>(get_double(sec, "predictive_kp_y",        hk.predictive_kp_y));
            hk.predictive_kd          = static_cast<float>(get_double(sec, "predictive_kd",          hk.predictive_kd));
            hk.predictive_pred_weight = static_cast<float>(
                get_double(sec, "predictive_pred_weight",
                           get_double(sec, "predictive_pred_weight_x", hk.predictive_pred_weight)));

            // 天枢 (Classic)
            hk.classic_aim_mode = static_cast<int>(get_long(sec, "classic_aim_mode", hk.classic_aim_mode));
            hk.classic_simple_start_speed  = static_cast<float>(get_double(sec, "classic_simple_start_speed",  hk.classic_simple_start_speed));
            hk.classic_simple_end_speed    = static_cast<float>(get_double(sec, "classic_simple_end_speed",    hk.classic_simple_end_speed));
            hk.classic_simple_transition_ms= static_cast<int>(get_long(sec, "classic_simple_transition_ms",    hk.classic_simple_transition_ms));
            hk.classic_simple_ki           = static_cast<float>(get_double(sec, "classic_simple_ki",           hk.classic_simple_ki));
            hk.classic_simple_kd           = static_cast<float>(get_double(sec, "classic_simple_kd",           hk.classic_simple_kd));
            hk.classic_adv_kpmin_x   = static_cast<float>(get_double(sec, "classic_adv_kpmin_x",   hk.classic_adv_kpmin_x));
            hk.classic_adv_kpmax_x   = static_cast<float>(get_double(sec, "classic_adv_kpmax_x",   hk.classic_adv_kpmax_x));
            hk.classic_adv_ki_x      = static_cast<float>(get_double(sec, "classic_adv_ki_x",      hk.classic_adv_ki_x));
            hk.classic_adv_kd_x      = static_cast<float>(get_double(sec, "classic_adv_kd_x",      hk.classic_adv_kd_x));
            hk.classic_adv_imax_x    = static_cast<float>(get_double(sec, "classic_adv_imax_x",    hk.classic_adv_imax_x));
            hk.classic_adv_pfactor_x = static_cast<float>(get_double(sec, "classic_adv_pfactor_x", hk.classic_adv_pfactor_x));
            hk.classic_adv_time_x    = static_cast<int>(get_long(sec, "classic_adv_time_x",        hk.classic_adv_time_x));
            hk.classic_adv_time_dynamic_x = get_bool(sec, "classic_adv_time_dynamic_x",            hk.classic_adv_time_dynamic_x);
            hk.classic_adv_kpmin_y   = static_cast<float>(get_double(sec, "classic_adv_kpmin_y",   hk.classic_adv_kpmin_y));
            hk.classic_adv_kpmax_y   = static_cast<float>(get_double(sec, "classic_adv_kpmax_y",   hk.classic_adv_kpmax_y));
            hk.classic_adv_ki_y      = static_cast<float>(get_double(sec, "classic_adv_ki_y",      hk.classic_adv_ki_y));
            hk.classic_adv_kd_y      = static_cast<float>(get_double(sec, "classic_adv_kd_y",      hk.classic_adv_kd_y));
            hk.classic_adv_imax_y    = static_cast<float>(get_double(sec, "classic_adv_imax_y",    hk.classic_adv_imax_y));
            hk.classic_adv_pfactor_y = static_cast<float>(get_double(sec, "classic_adv_pfactor_y", hk.classic_adv_pfactor_y));
            hk.classic_adv_time_y    = static_cast<int>(get_long(sec, "classic_adv_time_y",        hk.classic_adv_time_y));
            hk.classic_adv_time_dynamic_y = get_bool(sec, "classic_adv_time_dynamic_y",            hk.classic_adv_time_dynamic_y);
            hk.classic_prediction_mode       = static_cast<int>(get_long(sec, "classic_prediction_mode",       hk.classic_prediction_mode));
            hk.classic_velocity_lead_frames  = static_cast<float>(get_double(sec, "classic_velocity_lead_frames",  hk.classic_velocity_lead_frames));
            hk.classic_independent_y         = get_bool(sec, "classic_independent_y",                              hk.classic_independent_y);
            hk.classic_kalman_q_pos      = static_cast<float>(get_double(sec, "classic_kalman_q_pos",      hk.classic_kalman_q_pos));
            hk.classic_kalman_q_vel      = static_cast<float>(get_double(sec, "classic_kalman_q_vel",      hk.classic_kalman_q_vel));
            hk.classic_kalman_r_obs      = static_cast<float>(get_double(sec, "classic_kalman_r_obs",      hk.classic_kalman_r_obs));
            hk.classic_kalman_lookahead  = static_cast<float>(get_double(sec, "classic_kalman_lookahead",  hk.classic_kalman_lookahead));

            // 死区 (shared)
            hk.deadzone_enabled  = get_bool(sec, "deadzone_enabled",  hk.deadzone_enabled);
            hk.deadzone_percent  = static_cast<float>(get_double(sec, "deadzone_percent",  hk.deadzone_percent));

            // 扳机
            hk.trigger_enabled        = get_bool(sec, "trigger_enabled",        hk.trigger_enabled);
            hk.trigger_fire_delay     = static_cast<int>(get_long(sec, "trigger_fire_delay",     hk.trigger_fire_delay));
            hk.trigger_fire_duration  = static_cast<int>(get_long(sec, "trigger_fire_duration",  hk.trigger_fire_duration));
            hk.trigger_fire_interval  = static_cast<int>(get_long(sec, "trigger_fire_interval",  hk.trigger_fire_interval));
            hk.trigger_y_percent      = static_cast<int>(get_long(sec, "trigger_y_percent",      hk.trigger_y_percent));

            // 目标选择 3 槽位
            hk.target_class_1    = static_cast<int>(get_long(sec, "target_class_1",    hk.target_class_1));
            hk.target_y_top_1    = static_cast<float>(get_double(sec, "target_y_top_1",    hk.target_y_top_1));
            hk.target_y_bot_1    = static_cast<float>(get_double(sec, "target_y_bot_1",    hk.target_y_bot_1));
            hk.target_min_conf_1 = static_cast<float>(get_double(sec, "target_min_conf_1", hk.target_min_conf_1));
            hk.target_class_2    = static_cast<int>(get_long(sec, "target_class_2",    hk.target_class_2));
            hk.target_y_top_2    = static_cast<float>(get_double(sec, "target_y_top_2",    hk.target_y_top_2));
            hk.target_y_bot_2    = static_cast<float>(get_double(sec, "target_y_bot_2",    hk.target_y_bot_2));
            hk.target_min_conf_2 = static_cast<float>(get_double(sec, "target_min_conf_2", hk.target_min_conf_2));
            hk.target_class_3    = static_cast<int>(get_long(sec, "target_class_3",    hk.target_class_3));
            hk.target_y_top_3    = static_cast<float>(get_double(sec, "target_y_top_3",    hk.target_y_top_3));
            hk.target_y_bot_3    = static_cast<float>(get_double(sec, "target_y_bot_3",    hk.target_y_bot_3));
            hk.target_min_conf_3 = static_cast<float>(get_double(sec, "target_min_conf_3", hk.target_min_conf_3));
            hk.target_aim_range  = static_cast<int>(get_long(sec, "target_aim_range",  hk.target_aim_range));

            hk.crosshair_detect_enabled  = get_bool(sec, "crosshair_detect_enabled", false);
            hk.laser_detect_enabled      = get_bool(sec, "laser_detect_enabled", false);
            hk.flashlight_detect_enabled = get_bool(sec, "flashlight_detect_enabled", false);
            hk.glass_filter_enabled      = get_bool(sec, "glass_filter_enabled",      false);

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
            hk.aim_path_bezier_cx1  = static_cast<float>(get_double(sec, "aim_path_bezier_cx1", hk.aim_path_bezier_cx1));
            hk.aim_path_bezier_cy1  = static_cast<float>(get_double(sec, "aim_path_bezier_cy1", hk.aim_path_bezier_cy1));
            hk.aim_path_bezier_cx2  = static_cast<float>(get_double(sec, "aim_path_bezier_cx2", hk.aim_path_bezier_cx2));
            hk.aim_path_bezier_cy2  = static_cast<float>(get_double(sec, "aim_path_bezier_cy2", hk.aim_path_bezier_cy2));
            {
                const std::string raw = get_string(sec, "aim_path_custom_samples", "");
                hk.aim_path_custom_samples.clear();
                if (!raw.empty())
                {
                    for (const auto& tok : splitString(raw, ','))
                    {
                        try {
                            float v = std::stof(tok);
                            v = std::clamp(v, -1.0f, 1.0f);
                            hk.aim_path_custom_samples.push_back(v);
                        } catch (...) { /* ignore malformed sample */ }
                    }
                    // Endpoints are pinned to zero so the path actually
                    // ends ON the target — load-time enforcement guards
                    // against hand-edited ini files.
                    if (!hk.aim_path_custom_samples.empty())
                    {
                        hk.aim_path_custom_samples.front() = 0.0f;
                        hk.aim_path_custom_samples.back()  = 0.0f;
                    }
                }
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
        hk.speed_x        = std::clamp(hk.speed_x,        0.0f, 2.0f);
        hk.speed_y        = std::clamp(hk.speed_y,        0.0f, 2.0f);
        hk.dead_zone_px   = std::clamp(hk.dead_zone_px,   0.0f, 20.0f);
        hk.mover_kind     = std::clamp(hk.mover_kind,     0, 2);

        // 疾风 clamp
        hk.predictive_kp_x        = std::clamp(hk.predictive_kp_x,        0.0f, 10.0f);
        hk.predictive_kp_y        = std::clamp(hk.predictive_kp_y,        0.0f, 10.0f);
        hk.predictive_kd          = std::clamp(hk.predictive_kd,          0.0f, 5.0f);
        hk.predictive_pred_weight = std::clamp(hk.predictive_pred_weight, 0.0f, 3.0f);

        // 天枢 clamp
        hk.classic_aim_mode = std::clamp(hk.classic_aim_mode, 0, 1);
        hk.classic_simple_start_speed  = std::clamp(hk.classic_simple_start_speed,  0.0f, 5.0f);
        hk.classic_simple_end_speed    = std::clamp(hk.classic_simple_end_speed,    0.0f, 5.0f);
        hk.classic_simple_transition_ms= std::clamp(hk.classic_simple_transition_ms, 0, 10000);
        hk.classic_simple_ki           = std::clamp(hk.classic_simple_ki,           0.0f, 1.0f);
        hk.classic_simple_kd           = std::clamp(hk.classic_simple_kd,           0.0f, 2.0f);
        hk.classic_adv_kpmin_x   = std::clamp(hk.classic_adv_kpmin_x,   0.0f, 5.0f);
        hk.classic_adv_kpmax_x   = std::clamp(hk.classic_adv_kpmax_x,   0.0f, 5.0f);
        hk.classic_adv_ki_x      = std::clamp(hk.classic_adv_ki_x,      0.0f, 1.0f);
        hk.classic_adv_kd_x      = std::clamp(hk.classic_adv_kd_x,      0.0f, 2.0f);
        hk.classic_adv_imax_x    = std::clamp(hk.classic_adv_imax_x,    0.0f, 100.0f);
        hk.classic_adv_pfactor_x = std::clamp(hk.classic_adv_pfactor_x, 0.1f, 5.0f);
        hk.classic_adv_time_x    = std::clamp(hk.classic_adv_time_x,    0, 10000);
        hk.classic_adv_kpmin_y   = std::clamp(hk.classic_adv_kpmin_y,   0.0f, 5.0f);
        hk.classic_adv_kpmax_y   = std::clamp(hk.classic_adv_kpmax_y,   0.0f, 5.0f);
        hk.classic_adv_ki_y      = std::clamp(hk.classic_adv_ki_y,      0.0f, 1.0f);
        hk.classic_adv_kd_y      = std::clamp(hk.classic_adv_kd_y,      0.0f, 2.0f);
        hk.classic_adv_imax_y    = std::clamp(hk.classic_adv_imax_y,    0.0f, 100.0f);
        hk.classic_adv_pfactor_y = std::clamp(hk.classic_adv_pfactor_y, 0.1f, 5.0f);
        hk.classic_adv_time_y    = std::clamp(hk.classic_adv_time_y,    0, 10000);
        hk.classic_prediction_mode       = std::clamp(hk.classic_prediction_mode,       0, 2);
        hk.classic_velocity_lead_frames  = std::clamp(hk.classic_velocity_lead_frames,  0.0f, 10.0f);
        hk.classic_kalman_q_pos      = std::clamp(hk.classic_kalman_q_pos,      0.001f, 100.0f);
        hk.classic_kalman_q_vel      = std::clamp(hk.classic_kalman_q_vel,      0.001f, 100.0f);
        hk.classic_kalman_r_obs      = std::clamp(hk.classic_kalman_r_obs,      0.001f, 100.0f);
        hk.classic_kalman_lookahead  = std::clamp(hk.classic_kalman_lookahead,  0.0f, 100.0f);
        // 死区 clamp
        hk.deadzone_percent = std::clamp(hk.deadzone_percent, 0.0f, 100.0f);

        // 扳机 clamp
        hk.trigger_fire_delay    = std::clamp(hk.trigger_fire_delay,    0, 5000);
        hk.trigger_fire_duration = std::clamp(hk.trigger_fire_duration, 1, 5000);
        hk.trigger_fire_interval = std::clamp(hk.trigger_fire_interval, 0, 5000);
        hk.trigger_y_percent     = std::clamp(hk.trigger_y_percent,     1, 100);

        // 目标选择 clamp
        hk.target_y_top_1    = std::clamp(hk.target_y_top_1,    0.0f, 1.0f);
        hk.target_y_bot_1    = std::clamp(hk.target_y_bot_1,    0.0f, 1.0f);
        hk.target_min_conf_1 = std::clamp(hk.target_min_conf_1, 0.0f, 1.0f);
        hk.target_y_top_2    = std::clamp(hk.target_y_top_2,    0.0f, 1.0f);
        hk.target_y_bot_2    = std::clamp(hk.target_y_bot_2,    0.0f, 1.0f);
        hk.target_min_conf_2 = std::clamp(hk.target_min_conf_2, 0.0f, 1.0f);
        hk.target_y_top_3    = std::clamp(hk.target_y_top_3,    0.0f, 1.0f);
        hk.target_y_bot_3    = std::clamp(hk.target_y_bot_3,    0.0f, 1.0f);
        hk.target_min_conf_3 = std::clamp(hk.target_min_conf_3, 0.0f, 1.0f);
        hk.target_aim_range  = std::clamp(hk.target_aim_range,  1, 9999);

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

    file << "# Capture\n"
        << "capture_method = " << capture_method << "\n"
        << "udp_ip = " << udp_ip << "\n"
        << "udp_port = " << udp_port << "\n"
        << "tcp_ip = " << tcp_ip << "\n"
        << "tcp_port = " << tcp_port << "\n"
        << "eth_adapter = " << eth_adapter << "\n"
        << "eth_ethertype = " << eth_ethertype << "\n"
        << "opencv_capture_index = " << opencv_capture_index << "\n"
        << "opencv_capture_api = " << opencv_capture_api << "\n"
        << "opencv_capture_url = " << opencv_capture_url << "\n"
        << "opencv_capture_width = " << opencv_capture_width << "\n"
        << "opencv_capture_height = " << opencv_capture_height << "\n"
        << "opencv_capture_fps = " << opencv_capture_fps << "\n"
        << "capture_crop = " << capture_crop << "\n"
        << "capture_format = " << capture_format << "\n"
        << "capture_mf_gpu = " << to_bool_str(capture_mf_gpu) << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "circle_mask = " << to_bool_str(circle_mask) << "\n\n";

    file << "# Hardware / input device\n"
        << "# WIN32 | GHUB | ARDUINO | KMBOX_NET | KMBOX_A | MAKCU\n"
        << "input_method = " << input_method << "\n"
        << "arduino_baudrate = " << arduino_baudrate << "\n"
        << "arduino_port = " << arduino_port << "\n"
        << "arduino_16_bit_mouse = " << to_bool_str(arduino_16_bit_mouse) << "\n"
        << "arduino_enable_keys = " << to_bool_str(arduino_enable_keys) << "\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n"
        << "kmbox_a_pidvid = " << kmbox_a_pidvid << "\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n\n";

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
        << "export_enable_fp8 = " << to_bool_str(export_enable_fp8) << "\n"
        << "export_enable_fp16 = " << to_bool_str(export_enable_fp16) << "\n"
        << "fixed_input_size = " << to_bool_str(fixed_input_size) << "\n\n";

    file << "# CUDA / system\n"
        << "use_cuda_graph = " << to_bool_str(use_cuda_graph) << "\n"
        << "use_double_buffer = " << to_bool_str(use_double_buffer) << "\n"
        << "use_pinned_memory = " << to_bool_str(use_pinned_memory) << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << to_bool_str(enableGpuExclusiveMode) << "\n"
        << "capture_use_cuda = " << to_bool_str(capture_use_cuda) << "\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    file << "# Overlay\n"
        << "overlay_opacity = " << overlay_opacity << "\n"
        << std::fixed << std::setprecision(2)
        << "overlay_ui_scale = " << overlay_ui_scale << "\n\n";

    file << "# Authorization\n"
        << "auth_require_online = " << to_bool_str(auth_require_online) << "\n\n";

    file << "# Depth\n"
        << "depth_inference_enabled = " << to_bool_str(depth_inference_enabled) << "\n"
        << "depth_model_path = " << depth_model_path << "\n"
        << "depth_opt_input_size = " << depth_opt_input_size << "\n"
        << std::fixed << std::setprecision(3)
        << "depth_norm_clip_low_pct = " << depth_norm_clip_low_pct << "\n"
        << "depth_norm_clip_high_pct = " << depth_norm_clip_high_pct << "\n"
        << std::setprecision(0)
        << "depth_mask_fps = " << depth_mask_fps << "\n\n"
        << "replay_record_enabled = " << to_bool_str(replay_record_enabled) << "\n"
        << "replay_seconds = " << replay_seconds << "\n"
        << "replay_playback_speed = " << replay_playback_speed << "\n\n";

    file << "# Crosshair color detector (palette + ROI; per-hotkey toggle lives on each [hotkey.N])\n"
        << "crosshair_rect_w = "          << crosshair_rect_w          << "\n"
        << "crosshair_rect_h = "          << crosshair_rect_h          << "\n"
        << "crosshair_min_pixel_count = " << crosshair_min_pixel_count << "\n"
        << "crosshair_close_radius = "    << crosshair_close_radius    << "\n"
        << "crosshair_smooth = "          << crosshair_smooth          << "\n"
        << "laser_rect_w = "              << laser_rect_w              << "\n"
        << "laser_rect_h = "              << laser_rect_h              << "\n"
        << "laser_center_x = "            << laser_center_x            << "\n"
        << "laser_center_y = "            << laser_center_y            << "\n"
        << "laser_min_pixel_count = "     << laser_min_pixel_count     << "\n"
        << "laser_close_radius = "        << laser_close_radius        << "\n"
        << "laser_min_elongation = "      << laser_min_elongation      << "\n"
        << "laser_smooth = "              << laser_smooth              << "\n"
        << "laser_target_center_x = "     << laser_target_center_x     << "\n"
        << "laser_target_center_y = "     << laser_target_center_y     << "\n"
        << "laser_target_rect_w = "       << laser_target_rect_w       << "\n"
        << "laser_target_rect_h = "       << laser_target_rect_h       << "\n"
        << "flashlight_show_preview = "    << to_bool_str(flashlight_show_preview) << "\n"
        << "flashlight_sensitivity = "     << flashlight_sensitivity               << "\n"
        << "flashlight_reject_strength = " << flashlight_reject_strength           << "\n"
        << "flashlight_spot_size = "       << flashlight_spot_size                 << "\n"
        << "glass_filter_show_preview = "       << to_bool_str(glass_filter_show_preview)       << "\n"
        << "glass_filter_strength = "           << glass_filter_strength                        << "\n\n";

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
        file << std::fixed << std::setprecision(4)
             << "speed_x = "         << hk.speed_x         << "\n"
             << "speed_y = "         << hk.speed_y         << "\n"
             << "dead_zone_px = "    << hk.dead_zone_px    << "\n"
             << "mover_kind = "      << hk.mover_kind      << "\n"
             << "predictive_kp_x = "        << hk.predictive_kp_x        << "\n"
             << "predictive_kp_y = "        << hk.predictive_kp_y        << "\n"
             << "predictive_kd = "          << hk.predictive_kd          << "\n"
             << "predictive_pred_weight = " << hk.predictive_pred_weight << "\n"
             << "classic_aim_mode = " << hk.classic_aim_mode << "\n"
             << std::setprecision(4)
             << "classic_simple_start_speed = "  << hk.classic_simple_start_speed  << "\n"
             << "classic_simple_end_speed = "    << hk.classic_simple_end_speed    << "\n"
             << std::setprecision(0)
             << "classic_simple_transition_ms = "<< hk.classic_simple_transition_ms << "\n"
             << std::setprecision(4)
             << "classic_simple_ki = "           << hk.classic_simple_ki           << "\n"
             << "classic_simple_kd = "           << hk.classic_simple_kd           << "\n"
             << "classic_adv_kpmin_x = "   << hk.classic_adv_kpmin_x   << "\n"
             << "classic_adv_kpmax_x = "   << hk.classic_adv_kpmax_x   << "\n"
             << "classic_adv_ki_x = "      << hk.classic_adv_ki_x      << "\n"
             << "classic_adv_kd_x = "      << hk.classic_adv_kd_x      << "\n"
             << "classic_adv_imax_x = "    << hk.classic_adv_imax_x    << "\n"
             << "classic_adv_pfactor_x = " << hk.classic_adv_pfactor_x << "\n"
             << std::setprecision(0)
             << "classic_adv_time_x = "    << hk.classic_adv_time_x    << "\n"
             << "classic_adv_time_dynamic_x = " << to_bool_str(hk.classic_adv_time_dynamic_x) << "\n"
             << std::setprecision(4)
             << "classic_adv_kpmin_y = "   << hk.classic_adv_kpmin_y   << "\n"
             << "classic_adv_kpmax_y = "   << hk.classic_adv_kpmax_y   << "\n"
             << "classic_adv_ki_y = "      << hk.classic_adv_ki_y      << "\n"
             << "classic_adv_kd_y = "      << hk.classic_adv_kd_y      << "\n"
             << "classic_adv_imax_y = "    << hk.classic_adv_imax_y    << "\n"
             << "classic_adv_pfactor_y = " << hk.classic_adv_pfactor_y << "\n"
             << std::setprecision(0)
             << "classic_adv_time_y = "    << hk.classic_adv_time_y    << "\n"
             << "classic_adv_time_dynamic_y = " << to_bool_str(hk.classic_adv_time_dynamic_y) << "\n"
             << "classic_prediction_mode = "       << hk.classic_prediction_mode       << "\n"
             << std::setprecision(4)
             << "classic_velocity_lead_frames = "  << hk.classic_velocity_lead_frames  << "\n"
             << "classic_independent_y = "         << to_bool_str(hk.classic_independent_y)         << "\n"
             << "classic_kalman_q_pos = "      << hk.classic_kalman_q_pos      << "\n"
             << "classic_kalman_q_vel = "      << hk.classic_kalman_q_vel      << "\n"
             << "classic_kalman_r_obs = "      << hk.classic_kalman_r_obs      << "\n"
             << "classic_kalman_lookahead = "  << hk.classic_kalman_lookahead  << "\n"
             << "deadzone_enabled = "  << to_bool_str(hk.deadzone_enabled)  << "\n"
             << std::setprecision(4)
             << "deadzone_percent = "  << hk.deadzone_percent  << "\n"
             << "trigger_enabled = "       << to_bool_str(hk.trigger_enabled)       << "\n"
             << std::setprecision(0)
             << "trigger_fire_delay = "    << hk.trigger_fire_delay    << "\n"
             << "trigger_fire_duration = " << hk.trigger_fire_duration << "\n"
             << "trigger_fire_interval = " << hk.trigger_fire_interval << "\n"
             << "trigger_y_percent = "     << hk.trigger_y_percent     << "\n"
             << "target_class_1 = "      << hk.target_class_1      << "\n"
             << std::setprecision(4)
             << "target_y_top_1 = "      << hk.target_y_top_1      << "\n"
             << "target_y_bot_1 = "      << hk.target_y_bot_1      << "\n"
             << "target_min_conf_1 = "   << hk.target_min_conf_1   << "\n"
             << std::setprecision(0)
             << "target_class_2 = "      << hk.target_class_2      << "\n"
             << std::setprecision(4)
             << "target_y_top_2 = "      << hk.target_y_top_2      << "\n"
             << "target_y_bot_2 = "      << hk.target_y_bot_2      << "\n"
             << "target_min_conf_2 = "   << hk.target_min_conf_2   << "\n"
             << std::setprecision(0)
             << "target_class_3 = "      << hk.target_class_3      << "\n"
             << std::setprecision(4)
             << "target_y_top_3 = "      << hk.target_y_top_3      << "\n"
             << "target_y_bot_3 = "      << hk.target_y_bot_3      << "\n"
             << "target_min_conf_3 = "   << hk.target_min_conf_3   << "\n"
             << std::setprecision(0)
             << "target_aim_range = "    << hk.target_aim_range    << "\n"
             << "crosshair_detect_enabled = "  << to_bool_str(hk.crosshair_detect_enabled)  << "\n"
             << "laser_detect_enabled = "      << to_bool_str(hk.laser_detect_enabled)      << "\n"
             << "flashlight_detect_enabled = " << to_bool_str(hk.flashlight_detect_enabled) << "\n"
             << "glass_filter_enabled = "      << to_bool_str(hk.glass_filter_enabled)      << "\n"
             << "dynamic_fov_enabled = " << to_bool_str(hk.dynamic_fov_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "dynamic_fov_strength = " << hk.dynamic_fov_strength << "\n"
             << "aim_path_mode = " << hk.aim_path_mode << "\n"
             << std::fixed << std::setprecision(4)
             << "aim_path_bezier_cx1 = " << hk.aim_path_bezier_cx1 << "\n"
             << "aim_path_bezier_cy1 = " << hk.aim_path_bezier_cy1 << "\n"
             << "aim_path_bezier_cx2 = " << hk.aim_path_bezier_cx2 << "\n"
             << "aim_path_bezier_cy2 = " << hk.aim_path_bezier_cy2 << "\n";
        if (!hk.aim_path_custom_samples.empty())
        {
            file << "aim_path_custom_samples = ";
            for (size_t si = 0; si < hk.aim_path_custom_samples.size(); ++si)
            {
                if (si > 0) file << ',';
                file << hk.aim_path_custom_samples[si];
            }
            file << "\n";
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

    // Laser color palette: independent from the crosshair palette above.
    for (size_t i = 0; i < laser_colors.size(); ++i)
    {
        const auto& c = laser_colors[i];
        file << "[laser_color." << i << "]\n"
             << "name = "    << c.name    << "\n"
             << "enabled = " << to_bool_str(c.enabled) << "\n"
             << "h_low = "   << c.h_low   << "\n"
             << "h_high = "  << c.h_high  << "\n"
             << "s_min = "   << c.s_min   << "\n"
             << "s_max = "   << c.s_max   << "\n"
             << "v_min = "   << c.v_min   << "\n"
             << "v_max = "   << c.v_max   << "\n\n";
    }

    // Glass-film color palette: 独立调色板,用于玻璃过滤的边缘环命中判定。
    for (size_t i = 0; i < glass_colors.size(); ++i)
    {
        const auto& c = glass_colors[i];
        file << "[glass_color." << i << "]\n"
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

    // The rebuild above only emits 0..class_count-1, so re-add the synthetic
    // flashlight class it just dropped.
    ensure_flashlight_class();
}

void Config::ensure_flashlight_class()
{
    for (const auto& cf : class_filters)
        if (cf.class_id == kFlashlightClassId)
            return; // already present (incl. a user-chosen bucket) — leave it

    ClassFilterState st;
    st.class_id   = kFlashlightClassId;
    st.class_name = kFlashlightClassName;
    st.bucket     = ClassBucket::Aim; // default routable; user may rebucket later
    class_filters.push_back(std::move(st));
}



