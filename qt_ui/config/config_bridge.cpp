#include "config/config_bridge.h"
#include "config/ConfigManager.h"

#include <QSignalBlocker>
#include <QTimer>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "capture.h"
#include "runtime/inference_session.h"

extern std::atomic<bool> detector_model_changed;

ConfigBridge::ConfigBridge() : QObject(nullptr) {
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, &ConfigBridge::onSaveTimeout);

    connect(&ConfigManager::instance(), &ConfigManager::configChanged,
            this, &ConfigBridge::syncToRuntime);
}

ConfigBridge& ConfigBridge::instance() {
    static ConfigBridge s;
    return s;
}

void ConfigBridge::markDirty() {
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

void ConfigBridge::onSaveTimeout() {
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    config.saveConfig();
}

void ConfigBridge::syncToRuntime() {
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qs = [](const QString& s) { return s.toStdString(); };

    // --- Capture ---
    std::string oldMethod = config.capture_method;
    config.capture_method = qs(cm.captureMethod());
    config.udp_ip         = qs(cm.udpIp());
    config.udp_port       = cm.udpPort();
    config.tcp_ip         = qs(cm.tcpIp());
    config.tcp_port       = cm.tcpPort();
    config.eth_adapter    = qs(cm.ethAdapter());
    config.eth_ethertype  = cm.ethEthertype();
    config.opencv_capture_index = cm.opencvCaptureIndex();
    config.opencv_capture_api   = qs(cm.opencvCaptureApi());
    config.opencv_capture_url   = qs(cm.opencvCaptureUrl());
    config.opencv_capture_width = cm.opencvCaptureWidth();
    config.opencv_capture_height = cm.opencvCaptureHeight();
    config.opencv_capture_fps   = cm.opencvCaptureFps();
    config.capture_mf_gpu       = cm.captureMfGpu();
    config.capture_crop         = cm.captureCrop();
    config.capture_format       = qs(cm.captureFormat());

    int oldDetRes = config.detection_resolution;
    config.detection_resolution = cm.detectionResolution();

    int oldFps = config.capture_fps;
    config.capture_fps   = cm.captureFps();
    config.circle_mask   = cm.circleMask();

    // --- Hardware ---
    std::string oldInput = config.input_method;
    config.input_method      = qs(cm.inputMethod());
    config.arduino_baudrate  = cm.arduinoBaudrate();
    config.arduino_port      = qs(cm.arduinoPort());
    config.arduino_16_bit_mouse = cm.arduino16BitMouse();
    config.arduino_enable_keys  = cm.arduinoEnableKeys();
    config.kmbox_net_ip   = qs(cm.kmboxNetIp());
    config.kmbox_net_port = qs(cm.kmboxNetPort());
    config.kmbox_net_uuid = qs(cm.kmboxNetUuid());
    config.kmbox_a_pidvid = qs(cm.kmboxAPidvid());
    config.makcu_baudrate = cm.makcuBaudrate();
    config.makcu_port     = qs(cm.makcuPort());
    // --- AI ---
    std::string oldModel = config.ai_model;
    config.backend              = qs(cm.backend());
    config.dml_device_id        = cm.dmlDeviceId();
    config.ai_model             = qs(cm.aiModel());
    config.confidence_threshold = cm.confidenceThreshold();
    config.nms_threshold        = cm.nmsThreshold();
    config.max_detections       = cm.maxDetections();
    config.export_enable_fp8    = cm.exportEnableFp8();
    config.export_enable_fp16   = cm.exportEnableFp16();
    config.small_target_enabled    = cm.smallTargetEnabled();
    config.small_target_confidence = cm.smallTargetConfidence();
    config.small_target_area_frac  = cm.smallTargetAreaFrac();

    // --- Depth ---
    config.depth_inference_enabled = cm.depthInferenceEnabled();
    config.depth_model_path        = qs(cm.depthModelPath());
    config.depth_fps               = cm.depthFps();
    config.depth_mask_enabled      = cm.depthMaskEnabled();
    config.depth_mask_near_percent = cm.depthMaskNearPercent();
    config.depth_mask_alpha        = cm.depthMaskAlpha();
    config.depth_mask_fps          = cm.depthMaskFps();
    config.depth_opt_input_size    = cm.depthOptInputSize();
    config.depth_norm_clip_low_pct = cm.depthNormClipLowPct();
    config.depth_norm_clip_high_pct = cm.depthNormClipHighPct();
    config.depth_show_heatmap      = cm.depthShowHeatmap();
    config.depth_heatmap_gamma     = cm.depthHeatmapGamma();
    config.depth_show_bbox_distance = cm.depthShowBboxDistance();
    config.depth_mask_expand       = cm.depthMaskExpand();
    config.depth_mask_hold_frames  = cm.depthMaskHoldFrames();
    config.depth_mask_suppression_ratio = cm.depthMaskSuppressionRatio();
    config.depth_mask_invert       = cm.depthMaskInvert();
    config.depth_colormap          = cm.depthColormap();

    // --- Overlay ---
    config.overlay_opacity  = cm.overlayOpacity();
    config.overlay_ui_scale = cm.overlayUiScale();

    // --- Macro ---
    config.macro_enabled = cm.macroEnabled();
    config.macro_script_path = qs(cm.macroScriptPath());
    config.macro_primary_button_events = cm.macroPrimaryButtonEvents();

    // --- Crosshair ---
    config.crosshair_rect_w         = cm.crosshairRectW();
    config.crosshair_rect_h         = cm.crosshairRectH();
    config.crosshair_min_pixel_count = cm.crosshairMinPixelCount();
    config.crosshair_close_radius   = cm.crosshairCloseRadius();
    config.crosshair_smooth         = cm.crosshairSmooth();

    {
        auto qcolors = cm.crosshairColors();
        config.crosshair_colors.clear();
        for (const auto& qc : qcolors) {
            CrosshairColorProfileConfig c;
            c.name    = qs(qc.name);
            c.enabled = qc.enabled;
            c.h_low   = qc.hLow;
            c.h_high  = qc.hHigh;
            c.s_min   = qc.sMin;
            c.s_max   = qc.sMax;
            c.v_min   = qc.vMin;
            c.v_max   = qc.vMax;
            config.crosshair_colors.push_back(c);
        }
    }

    // --- Laser ---
    config.laser_rect_w         = cm.laserRectW();
    config.laser_rect_h         = cm.laserRectH();
    config.laser_center_x       = cm.laserCenterX();
    config.laser_center_y       = cm.laserCenterY();
    config.laser_target_center_x = cm.laserTargetCenterX();
    config.laser_target_center_y = cm.laserTargetCenterY();
    config.laser_target_rect_w  = cm.laserTargetRectW();
    config.laser_target_rect_h  = cm.laserTargetRectH();
    config.laser_min_elongation = cm.laserMinElongation();
    config.laser_min_pixel_count = cm.laserMinPixelCount();
    config.laser_close_radius   = cm.laserCloseRadius();
    config.laser_smooth         = cm.laserSmooth();

    // --- Flashlight halo ---
    config.flashlight_show_preview         = cm.flashlightShowPreview();
    config.flashlight_brightness_threshold = cm.flashlightBrightnessThreshold();
    config.flashlight_min_radius           = cm.flashlightMinRadius();
    config.flashlight_max_radius           = cm.flashlightMaxRadius();
    config.flashlight_min_circularity      = cm.flashlightMinCircularity();
    config.flashlight_open_radius          = cm.flashlightOpenRadius();
    config.flashlight_min_local_contrast   = cm.flashlightMinLocalContrast();
    config.flashlight_max_spots            = cm.flashlightMaxSpots();

    // --- Glass filter ---
    config.glass_filter_show_preview = cm.glassFilterShowPreview();
    config.glass_edge_ring_frac      = cm.glassEdgeRingFrac();
    config.glass_coverage_threshold  = cm.glassCoverageThreshold();
    config.glass_min_box_short_side  = cm.glassMinBoxShortSide();

    {
        auto qcolors = cm.glassColors();
        config.glass_colors.clear();
        for (const auto& qc : qcolors) {
            CrosshairColorProfileConfig c;
            c.name    = qs(qc.name);
            c.enabled = qc.enabled;
            c.h_low   = qc.hLow;
            c.h_high  = qc.hHigh;
            c.s_min   = qc.sMin;
            c.s_max   = qc.sMax;
            c.v_min   = qc.vMin;
            c.v_max   = qc.vMax;
            config.glass_colors.push_back(c);
        }
    }

    {
        auto qcolors = cm.laserColors();
        config.laser_colors.clear();
        for (const auto& qc : qcolors) {
            CrosshairColorProfileConfig c;
            c.name    = qs(qc.name);
            c.enabled = qc.enabled;
            c.h_low   = qc.hLow;
            c.h_high  = qc.hHigh;
            c.s_min   = qc.sMin;
            c.s_max   = qc.sMax;
            c.v_min   = qc.vMin;
            c.v_max   = qc.vMax;
            config.laser_colors.push_back(c);
        }
    }

    // --- Debug ---
    config.show_fps   = cm.showFps();
    config.verbose    = cm.verbose();
    config.screenshot_delay = cm.screenshotDelay();
    config.show_window      = cm.showWindow();
    config.replay_record_enabled = cm.replayRecordEnabled();
    config.replay_seconds        = cm.replaySeconds();
    config.replay_playback_speed = cm.replayPlaybackSpeed();

    // --- Active group & Hotkeys ---
    // Both are managed directly by HotkeyPage writing to config.hotkeys[]
    // and config.active_hotkey_group.  Syncing through ConfigManager risks
    // double-encoding CJK group names and wiping aim_classes (which
    // HotkeyData doesn't carry).  Do NOT overwrite here.

    // --- Set change flags ---
    if (config.capture_method != oldMethod)
        capture_method_changed = true;
    if (config.detection_resolution != oldDetRes)
        detection_resolution_changed = true;
    if (config.capture_fps != oldFps)
        capture_fps_changed = true;
    if (config.ai_model != oldModel) {
        detector_model_changed = true;
        std::string model_path = "models/" + config.ai_model;
        runtime::preload_model_metadata(model_path, false);
    }
    if (config.input_method != oldInput)
        input_method_changed = true;

    markDirty();
}

void ConfigBridge::syncFromRuntime()
{
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qstr = [](const std::string& s) { return QString::fromStdString(s); };

    QSignalBlocker blocker(&cm);

    // --- Capture ---
    cm.setCaptureMethod(qstr(config.capture_method));
    cm.setUdpIp(qstr(config.udp_ip));
    cm.setUdpPort(config.udp_port);
    cm.setTcpIp(qstr(config.tcp_ip));
    cm.setTcpPort(config.tcp_port);
    cm.setEthAdapter(qstr(config.eth_adapter));
    cm.setEthEthertype(config.eth_ethertype);
    cm.setOpencvCaptureIndex(config.opencv_capture_index);
    cm.setOpencvCaptureApi(qstr(config.opencv_capture_api));
    cm.setOpencvCaptureUrl(qstr(config.opencv_capture_url));
    cm.setOpencvCaptureWidth(config.opencv_capture_width);
    cm.setOpencvCaptureHeight(config.opencv_capture_height);
    cm.setOpencvCaptureFps(config.opencv_capture_fps);
    cm.setCaptureMfGpu(config.capture_mf_gpu);
    cm.setCaptureCrop(config.capture_crop);
    cm.setCaptureFormat(qstr(config.capture_format));
    cm.setDetectionResolution(config.detection_resolution);
    cm.setCaptureFps(config.capture_fps);
    cm.setCircleMask(config.circle_mask);

    // --- Hardware ---
    cm.setInputMethod(qstr(config.input_method));
    cm.setArduinoBaudrate(config.arduino_baudrate);
    cm.setArduinoPort(qstr(config.arduino_port));
    cm.setArduino16BitMouse(config.arduino_16_bit_mouse);
    cm.setArduinoEnableKeys(config.arduino_enable_keys);
    cm.setKmboxNetIp(qstr(config.kmbox_net_ip));
    cm.setKmboxNetPort(qstr(config.kmbox_net_port));
    cm.setKmboxNetUuid(qstr(config.kmbox_net_uuid));
    cm.setKmboxAPidvid(qstr(config.kmbox_a_pidvid));
    cm.setMakcuBaudrate(config.makcu_baudrate);
    cm.setMakcuPort(qstr(config.makcu_port));
    // --- AI ---
    cm.setBackend(qstr(config.backend));
    cm.setDmlDeviceId(config.dml_device_id);
    cm.setAiModel(qstr(config.ai_model));
    cm.setConfidenceThreshold(config.confidence_threshold);
    cm.setNmsThreshold(config.nms_threshold);
    cm.setMaxDetections(config.max_detections);
    cm.setExportEnableFp8(config.export_enable_fp8);
    cm.setExportEnableFp16(config.export_enable_fp16);
    cm.setSmallTargetEnabled(config.small_target_enabled);
    cm.setSmallTargetConfidence(config.small_target_confidence);
    cm.setSmallTargetAreaFrac(config.small_target_area_frac);

    // --- Depth ---
    cm.setDepthInferenceEnabled(config.depth_inference_enabled);
    cm.setDepthModelPath(qstr(config.depth_model_path));
    cm.setDepthFps(config.depth_fps);
    cm.setDepthMaskEnabled(config.depth_mask_enabled);
    cm.setDepthMaskNearPercent(config.depth_mask_near_percent);
    cm.setDepthMaskAlpha(config.depth_mask_alpha);
    cm.setDepthMaskFps(config.depth_mask_fps);
    cm.setDepthOptInputSize(config.depth_opt_input_size);
    cm.setDepthNormClipLowPct(config.depth_norm_clip_low_pct);
    cm.setDepthNormClipHighPct(config.depth_norm_clip_high_pct);
    cm.setDepthShowHeatmap(config.depth_show_heatmap);
    cm.setDepthHeatmapGamma(config.depth_heatmap_gamma);
    cm.setDepthShowBboxDistance(config.depth_show_bbox_distance);
    cm.setDepthMaskExpand(config.depth_mask_expand);
    cm.setDepthMaskHoldFrames(config.depth_mask_hold_frames);
    cm.setDepthMaskSuppressionRatio(config.depth_mask_suppression_ratio);
    cm.setDepthMaskInvert(config.depth_mask_invert);
    cm.setDepthColormap(config.depth_colormap);

    // --- Overlay ---
    cm.setOverlayOpacity(config.overlay_opacity);
    cm.setOverlayUiScale(config.overlay_ui_scale);

    // --- Macro ---
    cm.setMacroEnabled(config.macro_enabled);
    cm.setMacroScriptPath(qstr(config.macro_script_path));
    cm.setMacroPrimaryButtonEvents(config.macro_primary_button_events);

    // --- Crosshair ---
    cm.setCrosshairRectW(config.crosshair_rect_w);
    cm.setCrosshairRectH(config.crosshair_rect_h);
    cm.setCrosshairMinPixelCount(config.crosshair_min_pixel_count);
    cm.setCrosshairCloseRadius(config.crosshair_close_radius);
    cm.setCrosshairSmooth(config.crosshair_smooth);

    // --- Laser ---
    cm.setLaserRectW(config.laser_rect_w);
    cm.setLaserRectH(config.laser_rect_h);
    cm.setLaserCenterX(config.laser_center_x);
    cm.setLaserCenterY(config.laser_center_y);
    cm.setLaserTargetCenterX(config.laser_target_center_x);
    cm.setLaserTargetCenterY(config.laser_target_center_y);
    cm.setLaserTargetRectW(config.laser_target_rect_w);
    cm.setLaserTargetRectH(config.laser_target_rect_h);
    cm.setLaserMinElongation(config.laser_min_elongation);
    cm.setLaserMinPixelCount(config.laser_min_pixel_count);
    cm.setLaserCloseRadius(config.laser_close_radius);
    cm.setLaserSmooth(config.laser_smooth);

    // --- Flashlight halo ---
    cm.setFlashlightShowPreview(config.flashlight_show_preview);
    cm.setFlashlightBrightnessThreshold(config.flashlight_brightness_threshold);
    cm.setFlashlightMinRadius(config.flashlight_min_radius);
    cm.setFlashlightMaxRadius(config.flashlight_max_radius);
    cm.setFlashlightMinCircularity(config.flashlight_min_circularity);
    cm.setFlashlightOpenRadius(config.flashlight_open_radius);
    cm.setFlashlightMinLocalContrast(config.flashlight_min_local_contrast);
    cm.setFlashlightMaxSpots(config.flashlight_max_spots);

    // --- Glass filter ---
    cm.setGlassFilterShowPreview(config.glass_filter_show_preview);
    cm.setGlassEdgeRingFrac(config.glass_edge_ring_frac);
    cm.setGlassCoverageThreshold(config.glass_coverage_threshold);
    cm.setGlassMinBoxShortSide(config.glass_min_box_short_side);
    {
        QList<ConfigManager::ColorProfile> qcolors;
        for (const auto& c : config.glass_colors) {
            ConfigManager::ColorProfile qc;
            qc.name    = qstr(c.name);
            qc.enabled = c.enabled;
            qc.hLow    = c.h_low;
            qc.hHigh   = c.h_high;
            qc.sMin    = c.s_min;
            qc.sMax    = c.s_max;
            qc.vMin    = c.v_min;
            qc.vMax    = c.v_max;
            qcolors.append(qc);
        }
        cm.setGlassColors(qcolors);
    }

    // --- Debug ---
    cm.setShowFps(config.show_fps);
    cm.setVerbose(config.verbose);
    cm.setScreenshotDelay(config.screenshot_delay);
    cm.setShowWindow(config.show_window);
    cm.setReplayRecordEnabled(config.replay_record_enabled);
    cm.setReplaySeconds(config.replay_seconds);
    cm.setReplayPlaybackSpeed(config.replay_playback_speed);

    // --- Active group ---
    cm.setActiveHotkeyGroup(qstr(config.active_hotkey_group));

    // --- Hotkeys ---
    for (int i = static_cast<int>(config.hotkeys.size()); i < cm.hotkeyCount(); )
        cm.removeHotkey(cm.hotkeyCount() - 1);
    for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i) {
        const auto& hp = config.hotkeys[i];
        ConfigManager::HotkeyData hd;
        hd.name = qstr(hp.name);
        hd.group = qstr(hp.group);
        hd.keys.clear();
        for (const auto& k : hp.keys)
            hd.keys.push_back(qstr(k));
        hd.fovX = hp.fovX;
        hd.fovY = hp.fovY;
        hd.speedX        = hp.speed_x;
        hd.speedY        = hp.speed_y;
        hd.deadZonePx    = hp.dead_zone_px;
        hd.smartTriggerEnabled    = hp.smart_trigger_enabled;
        hd.smartTriggerHitScale   = hp.smart_trigger_hit_scale;
        hd.smartTriggerAggression = hp.smart_trigger_aggression;
        hd.laserDetectEnabled      = hp.laser_detect_enabled;
        hd.crosshairDetectEnabled  = hp.crosshair_detect_enabled;
        hd.flashlightDetectEnabled = hp.flashlight_detect_enabled;
        hd.glassFilterEnabled      = hp.glass_filter_enabled;
        hd.lockAggression         = hp.lock_aggression;
        hd.yOffsetSizeDecayEnabled = hp.y_offset_size_decay_enabled;
        hd.dynamicFovEnabled  = hp.dynamic_fov_enabled;
        hd.dynamicFovStrength = hp.dynamic_fov_strength;
        hd.aimPathMode        = hp.aim_path_mode;
        hd.aimPathBezierCx1   = hp.aim_path_bezier_cx1;
        hd.aimPathBezierCy1   = hp.aim_path_bezier_cy1;
        hd.aimPathBezierCx2   = hp.aim_path_bezier_cx2;
        hd.aimPathBezierCy2   = hp.aim_path_bezier_cy2;
        {
            QString joined;
            for (size_t si = 0; si < hp.aim_path_custom_samples.size(); ++si) {
                if (si > 0) joined.append(',');
                joined.append(QString::number(hp.aim_path_custom_samples[si], 'f', 4));
            }
            hd.aimPathCustomSamples = joined;
        }
        if (i < cm.hotkeyCount())
            cm.setHotkey(i, hd);
        else
            cm.addHotkey(hd);
    }
}
