#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
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

// Aim classes serialized as a semicolon-separated list; each entry is
// "class_id:y_offset". Example: "0:0.45;2:0.50;5:0.30".
std::string serialize_aim_classes(const std::vector<HotkeyAimClass>& classes)
{
    std::ostringstream oss;
    for (size_t i = 0; i < classes.size(); ++i)
    {
        if (i) oss << ';';
        oss << classes[i].class_id << ':' << std::fixed << std::setprecision(3) << classes[i].y_offset;
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
        auto colon = tok.find(':');
        if (colon == std::string::npos)
            continue;
        try
        {
            HotkeyAimClass c;
            c.class_id = std::stoi(tok.substr(0, colon));
            c.y_offset = std::stof(tok.substr(colon + 1));
            if (c.y_offset < 0.0f) c.y_offset = 0.0f;
            if (c.y_offset > 1.0f) c.y_offset = 1.0f;
            out.push_back(c);
        }
        catch (...)
        {
            // skip malformed entry
        }
    }
    return out;
}

void apply_default_hotkey(HotkeyProfile& hk)
{
    hk.name = "Aim";
    hk.keys = { "RightMouseButton" };
    hk.aim_classes.clear();
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
    detection_resolution = 320;
    capture_fps = 60;
    circle_mask = true;

    backend = "TRT";
    ai_model = "sunxds_0.5.6.engine";
    confidence_threshold = 0.10f;
    nms_threshold = 0.50f;
    max_detections = 100;
    export_enable_fp8 = false;
    export_enable_fp16 = true;
    fixed_input_size = false;

    use_cuda_graph = false;
    use_double_buffer = false;
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
    depth_fps = 100;
    depth_colormap = 18;
    depth_mask_enabled = false;
    depth_mask_fps = 5;
    depth_mask_near_percent = 20;
    depth_mask_expand = 0;
    depth_mask_hold_frames = 0;
    depth_mask_alpha = 90;
    depth_mask_invert = false;
    depth_mask_suppression_ratio = 0.30f;

    screenshot_button = splitString("None");
    screenshot_delay = 500;

    class_filters.clear();

    hotkeys.clear();
    HotkeyProfile hk;
    apply_default_hotkey(hk);
    hotkeys.push_back(std::move(hk));
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
    if (capture_method != "udp_capture" && capture_method != "tcp_capture"
        && capture_method != "capture_card")
        capture_method = "udp_capture";
    udp_ip = get_string("", "udp_ip", "0.0.0.0");
    udp_port = get_long("", "udp_port", 1234);
    if (udp_port < 1 || udp_port > 65535) udp_port = 1234;
    tcp_ip = get_string("", "tcp_ip", "0.0.0.0");
    tcp_port = get_long("", "tcp_port", 1235);
    if (tcp_port < 1 || tcp_port > 65535) tcp_port = 1235;
    capture_card_index = get_long("", "capture_card_index", 0);
    if (capture_card_index < 0) capture_card_index = 0;
    capture_card_width = get_long("", "capture_card_width", 0);
    capture_card_height = get_long("", "capture_card_height", 0);
    capture_card_fps = get_long("", "capture_card_fps", 0);
    capture_card_format = get_string("", "capture_card_format", "AUTO");
    if (capture_card_format != "AUTO" && capture_card_format != "NV12"
        && capture_card_format != "MJPG" && capture_card_format != "YUY2"
        && capture_card_format != "RGB32")
        capture_card_format = "AUTO";
    capture_card_crop_width = get_long("", "capture_card_crop_width", 0);
    capture_card_crop_height = get_long("", "capture_card_crop_height", 0);
    if (capture_card_width < 0) capture_card_width = 0;
    if (capture_card_height < 0) capture_card_height = 0;
    if (capture_card_fps < 0) capture_card_fps = 0;
    if (capture_card_crop_width < 0) capture_card_crop_width = 0;
    if (capture_card_crop_height < 0) capture_card_crop_height = 0;
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
    max_detections = get_long("", "max_detections", 20);
    export_enable_fp8 = get_bool("", "export_enable_fp8", false);
    export_enable_fp16 = get_bool("", "export_enable_fp16", true);
    fixed_input_size = get_bool("", "fixed_input_size", false);

    use_cuda_graph = get_bool("", "use_cuda_graph", false);
    use_double_buffer = get_bool("", "use_double_buffer", false);
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
    depth_fps = get_long("", "depth_fps", 100);
    if (depth_fps < 0) depth_fps = 0;
    depth_colormap = get_long("", "depth_colormap", 18);
    if (depth_colormap < 0 || depth_colormap > 21) depth_colormap = 18;
    depth_mask_enabled = get_bool("", "depth_mask_enabled", false);
    depth_mask_fps = std::max(0, get_long("", "depth_mask_fps", 5));
    depth_mask_near_percent = std::clamp(get_long("", "depth_mask_near_percent", 20), 1, 100);
    depth_mask_expand = std::clamp(get_long("", "depth_mask_expand", 0), 0, 128);
    depth_mask_hold_frames = std::clamp(get_long("", "depth_mask_hold_frames", 0), 0, 120);
    depth_mask_alpha = std::clamp(get_long("", "depth_mask_alpha", 90), 0, 255);
    depth_mask_invert = get_bool("", "depth_mask_invert", false);
    depth_mask_suppression_ratio = std::clamp(
        static_cast<float>(get_double("", "depth_mask_suppression_ratio", 0.30)),
        0.0f, 1.0f);

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

    // ---------- Crosshair color detector (palette + rect + area) ----------
    crosshair_rect_w           = std::clamp(get_long("", "crosshair_rect_w",  40), 4, 512);
    crosshair_rect_h           = std::clamp(get_long("", "crosshair_rect_h",  40), 4, 512);
    crosshair_min_pixel_count  = std::clamp(get_long("", "crosshair_min_pixel_count", 4), 1, 10000);
    crosshair_close_radius     = std::clamp(get_long("", "crosshair_close_radius",    1), 0, 7);
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
            hk.keys = splitString(get_string(sec, "keys", "RightMouseButton"));
            hk.aim_classes = parse_aim_classes(get_string(sec, "aim_classes", ""));

            hk.fovX = get_long(sec, "fovX", hk.fovX);
            hk.fovY = get_long(sec, "fovY", hk.fovY);
            hk.speed_x       = static_cast<float>(get_double(sec, "speed_x",       hk.speed_x));
            hk.speed_y       = static_cast<float>(get_double(sec, "speed_y",       hk.speed_y));
            hk.lock_strength = static_cast<float>(get_double(sec, "lock_strength", hk.lock_strength));
            hk.lock_radius_px = static_cast<float>(get_double(sec, "lock_radius_px", hk.lock_radius_px));

            {
                const long mode_raw = get_long(sec, "aim_trajectory_mode",
                    static_cast<long>(hk.aim_trajectory_mode));
                hk.aim_trajectory_mode = (mode_raw == 1)
                    ? AimTrajectoryMode::Bezier
                    : AimTrajectoryMode::Direct;
            }
            hk.bezier_cx1 = static_cast<float>(get_double(sec, "bezier_cx1", hk.bezier_cx1));
            hk.bezier_cy1 = static_cast<float>(get_double(sec, "bezier_cy1", hk.bezier_cy1));
            hk.bezier_cx2 = static_cast<float>(get_double(sec, "bezier_cx2", hk.bezier_cx2));
            hk.bezier_cy2 = static_cast<float>(get_double(sec, "bezier_cy2", hk.bezier_cy2));
            hk.bezier_follow_alpha = static_cast<float>(
                get_double(sec, "bezier_follow_alpha", hk.bezier_follow_alpha));
            hk.bezier_reanchor_threshold_px = static_cast<float>(
                get_double(sec, "bezier_reanchor_threshold_px", hk.bezier_reanchor_threshold_px));

            hk.predictionInterval = static_cast<float>(get_double(sec, "predictionInterval", hk.predictionInterval));
            hk.prediction_futurePositions = get_long(sec, "prediction_futurePositions", hk.prediction_futurePositions);
            hk.draw_futurePositions = get_bool(sec, "draw_futurePositions", hk.draw_futurePositions);

            hk.kalman_enabled = get_bool(sec, "kalman_enabled", hk.kalman_enabled);
            hk.kalman_process_noise_position = static_cast<float>(get_double(sec, "kalman_process_noise_position", hk.kalman_process_noise_position));
            hk.kalman_process_noise_velocity = static_cast<float>(get_double(sec, "kalman_process_noise_velocity", hk.kalman_process_noise_velocity));
            hk.kalman_measurement_noise = static_cast<float>(get_double(sec, "kalman_measurement_noise", hk.kalman_measurement_noise));
            hk.kalman_velocity_damping = static_cast<float>(get_double(sec, "kalman_velocity_damping", hk.kalman_velocity_damping));
            hk.kalman_max_velocity = static_cast<float>(get_double(sec, "kalman_max_velocity", hk.kalman_max_velocity));
            hk.kalman_warmup_frames = get_long(sec, "kalman_warmup_frames", hk.kalman_warmup_frames);
            hk.kalman_compensate_detection_delay = get_bool(sec, "kalman_compensate_detection_delay", hk.kalman_compensate_detection_delay);
            hk.kalman_additional_prediction_ms = static_cast<float>(get_double(sec, "kalman_additional_prediction_ms", hk.kalman_additional_prediction_ms));
            hk.kalman_reset_timeout_sec = static_cast<float>(get_double(sec, "kalman_reset_timeout_sec", hk.kalman_reset_timeout_sec));

            hk.crosshair_detect_enabled = get_bool(sec, "crosshair_detect_enabled", false);

            hk.lock_switch_score_margin = static_cast<float>(
                get_double(sec, "lock_switch_score_margin", hk.lock_switch_score_margin));
            hk.lock_switch_min_frames = get_long(sec, "lock_switch_min_frames", hk.lock_switch_min_frames);
            hk.lock_hold_min_frames   = get_long(sec, "lock_hold_min_frames",   hk.lock_hold_min_frames);
            hk.y_offset_size_decay_enabled = get_bool(sec, "y_offset_size_decay_enabled",
                                                       hk.y_offset_size_decay_enabled);
            hk.y_offset_size_decay_low_frac = static_cast<float>(
                get_double(sec, "y_offset_size_decay_low_frac", hk.y_offset_size_decay_low_frac));
            hk.y_offset_size_decay_high_frac = static_cast<float>(
                get_double(sec, "y_offset_size_decay_high_frac", hk.y_offset_size_decay_high_frac));

            hk.smart_trigger_enabled         = get_bool(sec, "smart_trigger_enabled", hk.smart_trigger_enabled);
            hk.smart_trigger_hit_radius_frac = static_cast<float>(get_double(sec, "smart_trigger_hit_radius_frac", hk.smart_trigger_hit_radius_frac));
            hk.smart_trigger_variance_max_px = static_cast<float>(get_double(sec, "smart_trigger_variance_max_px", hk.smart_trigger_variance_max_px));
            hk.smart_trigger_window_frames   = get_long(sec, "smart_trigger_window_frames", hk.smart_trigger_window_frames);
            hk.smart_trigger_min_prob        = static_cast<float>(get_double(sec, "smart_trigger_min_prob", hk.smart_trigger_min_prob));
            hk.smart_trigger_fire_duration_ms = get_long(sec, "smart_trigger_fire_duration_ms", hk.smart_trigger_fire_duration_ms);

            hk.threat_priority_enabled = get_bool(sec, "threat_priority_enabled", hk.threat_priority_enabled);
            hk.threat_weight           = static_cast<float>(get_double(sec, "threat_weight", hk.threat_weight));
            hk.threat_head_class_id    = get_long(sec, "threat_head_class_id", hk.threat_head_class_id);
            hk.threat_body_class_id    = get_long(sec, "threat_body_class_id", hk.threat_body_class_id);

            hk.dynamic_fov_enabled         = get_bool(sec, "dynamic_fov_enabled", hk.dynamic_fov_enabled);
            hk.dynamic_fov_margin_frac     = static_cast<float>(get_double(sec, "dynamic_fov_margin_frac",     hk.dynamic_fov_margin_frac));
            hk.dynamic_fov_min_radius_frac = static_cast<float>(get_double(sec, "dynamic_fov_min_radius_frac", hk.dynamic_fov_min_radius_frac));

            // Per-class Kalman overrides: one ini key per aim class:
            //   aim_class_kalman_<class_id> = enabled,pnp,pnv,mn,vd,mxv
            // Missing keys mean "no override" (the hotkey-level Kalman wins).
            for (auto& ac : hk.aim_classes)
            {
                const std::string key = "aim_class_kalman_" + std::to_string(ac.class_id);
                const std::string raw = get_string(sec, key.c_str(), "");
                if (raw.empty())
                    continue;
                const auto parts = splitString(raw);
                if (parts.size() < 6)
                    continue;
                try
                {
                    ac.kalman_override_enabled       = (parts[0] == "true" || parts[0] == "1");
                    ac.kalman_process_noise_position = std::stof(parts[1]);
                    ac.kalman_process_noise_velocity = std::stof(parts[2]);
                    ac.kalman_measurement_noise      = std::stof(parts[3]);
                    ac.kalman_velocity_damping       = std::stof(parts[4]);
                    ac.kalman_max_velocity           = std::stof(parts[5]);
                }
                catch (...)
                {
                    // Malformed override: leave class on defaults.
                    ac.kalman_override_enabled = false;
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

    // Clamp kalman params to safe ranges on every hotkey.
    auto clamp_kalman_fields = [](HotkeyProfile& hk) {
        hk.kalman_process_noise_position = std::clamp(hk.kalman_process_noise_position, 0.0001f, 5000.0f);
        hk.kalman_process_noise_velocity = std::clamp(hk.kalman_process_noise_velocity, 0.0001f, 50000.0f);
        hk.kalman_measurement_noise = std::clamp(hk.kalman_measurement_noise, 0.0001f, 5000.0f);
        hk.kalman_velocity_damping = std::clamp(hk.kalman_velocity_damping, 0.0f, 3.0f);
        hk.kalman_max_velocity = std::clamp(hk.kalman_max_velocity, 100.0f, 60000.0f);
        hk.kalman_warmup_frames = std::clamp(hk.kalman_warmup_frames, 0, 20);
        hk.kalman_additional_prediction_ms = std::clamp(hk.kalman_additional_prediction_ms, -80.0f, 120.0f);
        hk.kalman_reset_timeout_sec = std::clamp(hk.kalman_reset_timeout_sec, 0.05f, 3.0f);
        hk.speed_x       = std::clamp(hk.speed_x, 0.0f, 1.0f);
        hk.speed_y       = std::clamp(hk.speed_y, 0.0f, 1.0f);
        hk.lock_strength = std::clamp(hk.lock_strength, 0.0f, 1.0f);
        hk.lock_radius_px = std::clamp(hk.lock_radius_px, 0.0f, 200.0f);
        hk.bezier_cx1 = std::clamp(hk.bezier_cx1, 0.0f, 1.0f);
        hk.bezier_cx2 = std::clamp(hk.bezier_cx2, 0.0f, 1.0f);
        hk.bezier_cy1 = std::clamp(hk.bezier_cy1, -1.0f, 1.0f);
        hk.bezier_cy2 = std::clamp(hk.bezier_cy2, -1.0f, 1.0f);
        hk.bezier_follow_alpha = std::clamp(hk.bezier_follow_alpha, 0.0f, 1.0f);
        hk.bezier_reanchor_threshold_px = std::clamp(hk.bezier_reanchor_threshold_px, 1.0f, 4096.0f);

        hk.lock_switch_score_margin = std::clamp(hk.lock_switch_score_margin, 0.0f, 200.0f);
        hk.lock_switch_min_frames   = std::clamp(hk.lock_switch_min_frames, 1, 6000);
        hk.lock_hold_min_frames     = std::clamp(hk.lock_hold_min_frames, 0, 2400);
        hk.y_offset_size_decay_low_frac  = std::clamp(hk.y_offset_size_decay_low_frac,  0.0f, 1.0f);
        hk.y_offset_size_decay_high_frac = std::clamp(hk.y_offset_size_decay_high_frac, 0.0f, 1.0f);
        if (hk.y_offset_size_decay_high_frac <= hk.y_offset_size_decay_low_frac)
            hk.y_offset_size_decay_high_frac = std::min(1.0f, hk.y_offset_size_decay_low_frac + 0.01f);

        hk.smart_trigger_hit_radius_frac = std::clamp(hk.smart_trigger_hit_radius_frac, 0.05f, 1.0f);
        hk.smart_trigger_variance_max_px = std::clamp(hk.smart_trigger_variance_max_px, 0.0f, 100.0f);
        hk.smart_trigger_window_frames   = std::clamp(hk.smart_trigger_window_frames,   2, 60);
        hk.smart_trigger_min_prob        = std::clamp(hk.smart_trigger_min_prob,        0.0f, 1.0f);
        hk.smart_trigger_fire_duration_ms = std::clamp(hk.smart_trigger_fire_duration_ms, 5, 1000);

        hk.threat_weight = std::clamp(hk.threat_weight, 0.0f, 1.0f);
        if (hk.threat_head_class_id < -1)
            hk.threat_head_class_id = -1;
        if (hk.threat_body_class_id < -1)
            hk.threat_body_class_id = -1;

        hk.dynamic_fov_margin_frac     = std::clamp(hk.dynamic_fov_margin_frac,     1.0f, 3.0f);
        hk.dynamic_fov_min_radius_frac = std::clamp(hk.dynamic_fov_min_radius_frac, 0.05f, 1.0f);

        for (auto& ac : hk.aim_classes)
        {
            ac.kalman_process_noise_position = std::clamp(ac.kalman_process_noise_position, 0.0001f, 5000.0f);
            ac.kalman_process_noise_velocity = std::clamp(ac.kalman_process_noise_velocity, 0.0001f, 50000.0f);
            ac.kalman_measurement_noise      = std::clamp(ac.kalman_measurement_noise,      0.0001f, 5000.0f);
            ac.kalman_velocity_damping       = std::clamp(ac.kalman_velocity_damping,       0.0f,    3.0f);
            ac.kalman_max_velocity           = std::clamp(ac.kalman_max_velocity,           100.0f,  60000.0f);
        }
    };
    for (auto& hk : hotkeys)
        clamp_kalman_fields(hk);

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
        << "capture_card_index = " << capture_card_index << "\n"
        << "capture_card_width = " << capture_card_width << "\n"
        << "capture_card_height = " << capture_card_height << "\n"
        << "capture_card_fps = " << capture_card_fps << "\n"
        << "capture_card_format = " << capture_card_format << "\n"
        << "capture_card_crop_width = " << capture_card_crop_width << "\n"
        << "capture_card_crop_height = " << capture_card_crop_height << "\n"
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
        << "depth_fps = " << depth_fps << "\n"
        << "depth_colormap = " << depth_colormap << "\n"
        << "depth_mask_enabled = " << to_bool_str(depth_mask_enabled) << "\n"
        << "depth_mask_fps = " << depth_mask_fps << "\n"
        << "depth_mask_near_percent = " << depth_mask_near_percent << "\n"
        << "depth_mask_expand = " << depth_mask_expand << "\n"
        << "depth_mask_hold_frames = " << depth_mask_hold_frames << "\n"
        << "depth_mask_alpha = " << depth_mask_alpha << "\n"
        << "depth_mask_invert = " << to_bool_str(depth_mask_invert) << "\n"
        << std::fixed << std::setprecision(3)
        << "depth_mask_suppression_ratio = " << depth_mask_suppression_ratio << "\n\n"
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

    // Hotkey profiles.
    for (size_t i = 0; i < hotkeys.size(); ++i)
    {
        const auto& hk = hotkeys[i];
        file << "[hotkey." << i << "]\n";
        file << "name = " << hk.name << "\n";
        file << "keys = " << joinStrings(hk.keys) << "\n";
        file << "aim_classes = " << serialize_aim_classes(hk.aim_classes) << "\n";
        file << "fovX = " << hk.fovX << "\n";
        file << "fovY = " << hk.fovY << "\n";
        file << std::fixed << std::setprecision(4)
             << "speed_x = "       << hk.speed_x       << "\n"
             << "speed_y = "       << hk.speed_y       << "\n"
             << "lock_strength = " << hk.lock_strength << "\n"
             << "lock_radius_px = " << hk.lock_radius_px << "\n"
             << std::setprecision(0)
             << "aim_trajectory_mode = " << static_cast<int>(hk.aim_trajectory_mode) << "\n"
             << std::fixed << std::setprecision(4)
             << "bezier_cx1 = " << hk.bezier_cx1 << "\n"
             << "bezier_cy1 = " << hk.bezier_cy1 << "\n"
             << "bezier_cx2 = " << hk.bezier_cx2 << "\n"
             << "bezier_cy2 = " << hk.bezier_cy2 << "\n"
             << "bezier_follow_alpha = " << hk.bezier_follow_alpha << "\n"
             << std::setprecision(2)
             << "bezier_reanchor_threshold_px = " << hk.bezier_reanchor_threshold_px << "\n";
        file << std::fixed << std::setprecision(2)
             << "predictionInterval = " << hk.predictionInterval << "\n"
             << std::setprecision(0)
             << "prediction_futurePositions = " << hk.prediction_futurePositions << "\n"
             << "draw_futurePositions = " << to_bool_str(hk.draw_futurePositions) << "\n"
             << "kalman_enabled = " << to_bool_str(hk.kalman_enabled) << "\n"
             << std::fixed << std::setprecision(4)
             << "kalman_process_noise_position = " << hk.kalman_process_noise_position << "\n"
             << "kalman_process_noise_velocity = " << hk.kalman_process_noise_velocity << "\n"
             << "kalman_measurement_noise = " << hk.kalman_measurement_noise << "\n"
             << "kalman_velocity_damping = " << hk.kalman_velocity_damping << "\n"
             << "kalman_max_velocity = " << hk.kalman_max_velocity << "\n"
             << std::setprecision(0)
             << "kalman_warmup_frames = " << hk.kalman_warmup_frames << "\n"
             << "kalman_compensate_detection_delay = " << to_bool_str(hk.kalman_compensate_detection_delay) << "\n"
             << std::fixed << std::setprecision(2)
             << "kalman_additional_prediction_ms = " << hk.kalman_additional_prediction_ms << "\n"
             << "kalman_reset_timeout_sec = " << hk.kalman_reset_timeout_sec << "\n"
             << "crosshair_detect_enabled = " << to_bool_str(hk.crosshair_detect_enabled) << "\n"
             << std::fixed << std::setprecision(4)
             << "lock_switch_score_margin = " << hk.lock_switch_score_margin << "\n"
             << std::setprecision(0)
             << "lock_switch_min_frames = "   << hk.lock_switch_min_frames << "\n"
             << "lock_hold_min_frames = "     << hk.lock_hold_min_frames << "\n"
             << "y_offset_size_decay_enabled = " << to_bool_str(hk.y_offset_size_decay_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "y_offset_size_decay_low_frac = "  << hk.y_offset_size_decay_low_frac  << "\n"
             << "y_offset_size_decay_high_frac = " << hk.y_offset_size_decay_high_frac << "\n"
             << "smart_trigger_enabled = " << to_bool_str(hk.smart_trigger_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "smart_trigger_hit_radius_frac = " << hk.smart_trigger_hit_radius_frac << "\n"
             << "smart_trigger_variance_max_px = " << hk.smart_trigger_variance_max_px << "\n"
             << std::setprecision(0)
             << "smart_trigger_window_frames = " << hk.smart_trigger_window_frames << "\n"
             << std::fixed << std::setprecision(3)
             << "smart_trigger_min_prob = " << hk.smart_trigger_min_prob << "\n"
             << std::setprecision(0)
             << "smart_trigger_fire_duration_ms = " << hk.smart_trigger_fire_duration_ms << "\n"
             << "threat_priority_enabled = " << to_bool_str(hk.threat_priority_enabled) << "\n"
             << "threat_weight = " << hk.threat_weight << "\n"
             << "threat_head_class_id = " << hk.threat_head_class_id << "\n"
             << "threat_body_class_id = " << hk.threat_body_class_id << "\n"
             << "dynamic_fov_enabled = " << to_bool_str(hk.dynamic_fov_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "dynamic_fov_margin_frac = "     << hk.dynamic_fov_margin_frac << "\n"
             << "dynamic_fov_min_radius_frac = " << hk.dynamic_fov_min_radius_frac << "\n";

        // Per-class Kalman overrides: only emit keys for classes that have
        // overrides enabled. Skipping the rest keeps config.ini readable.
        for (const auto& ac : hk.aim_classes)
        {
            if (!ac.kalman_override_enabled)
                continue;
            file << "aim_class_kalman_" << ac.class_id << " = "
                 << (ac.kalman_override_enabled ? "1" : "0") << ","
                 << std::fixed << std::setprecision(4)
                 << ac.kalman_process_noise_position << ","
                 << ac.kalman_process_noise_velocity << ","
                 << ac.kalman_measurement_noise << ","
                 << ac.kalman_velocity_damping << ","
                 << ac.kalman_max_velocity << "\n";
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



