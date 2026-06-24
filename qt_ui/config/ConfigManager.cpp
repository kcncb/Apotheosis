#include "config/ConfigManager.h"

#include <QFileInfo>

ConfigManager::ConfigManager()
    : QObject(nullptr) {}

ConfigManager& ConfigManager::instance() {
    static ConfigManager s;
    return s;
}

bool ConfigManager::load(const QString& path) {
    m_path = path;
    delete m_settings;
    // Use a sidecar file (.cache) instead of config.ini so that QSettings'
    // auto-sync on destruction does not overwrite the SimpleIni flat file.
    // All values are populated via ConfigBridge::syncFromRuntime(); the sidecar
    // is only used as backing storage for the in-memory QVariant map.
    m_settings = new QSettings(m_path + ".cache", QSettings::IniFormat, this);

    if (!QFileInfo::exists(m_path)) {
        setCaptureMethod("udp_capture");
        setUdpIp("0.0.0.0");
        setUdpPort(1234);
        setDetectionResolution(320);
        setCaptureFps(60);
        setCircleMask(true);

        setBackend("TRT");
        setAiModel("sunxds_0.5.6.engine");
        setConfidenceThreshold(0.10f);
        setNmsThreshold(0.50f);
        setMaxDetections(100);
        setExportEnableFp8(false);
        setExportEnableFp16(true);

        setDepthInferenceEnabled(true);
        setDepthModelPath("depth_anything_v2.engine");
        setDepthFps(100);
        setDepthMaskEnabled(false);
        setDepthMaskNearPercent(20);
        setDepthMaskAlpha(90);

        setOverlayOpacity(240);
        setOverlayUiScale(1.0f);

        setInputMethod("WIN32");

        HotkeyData hk;
        hk.name = "Aim";
        hk.keys = {"RightMouseButton"};
        addHotkey(hk);

        save();
    }

    emit configLoaded();
    return true;
}

bool ConfigManager::save() {
    // Persistence is handled exclusively by Config::saveConfig() (SimpleIni)
    // to avoid section/key conflicts with QSettings INI format.
    // ConfigManager acts as an in-memory cache only.
    return true;
}

QString ConfigManager::configPath() const {
    return m_path;
}

// --- Capture ---

QString ConfigManager::captureMethod() const {
    return m_settings->value("Capture/capture_method", "udp_capture").toString();
}

void ConfigManager::setCaptureMethod(const QString& v) {
    m_settings->setValue("Capture/capture_method", v);
    emit configChanged();
}

QString ConfigManager::udpIp() const {
    return m_settings->value("Capture/udp_ip", "0.0.0.0").toString();
}

void ConfigManager::setUdpIp(const QString& v) {
    m_settings->setValue("Capture/udp_ip", v);
    emit configChanged();
}

int ConfigManager::udpPort() const {
    return m_settings->value("Capture/udp_port", 1234).toInt();
}

void ConfigManager::setUdpPort(int v) {
    m_settings->setValue("Capture/udp_port", v);
    emit configChanged();
}

QString ConfigManager::tcpIp() const {
    return m_settings->value("Capture/tcp_ip", "0.0.0.0").toString();
}

void ConfigManager::setTcpIp(const QString& v) {
    m_settings->setValue("Capture/tcp_ip", v);
    emit configChanged();
}

int ConfigManager::tcpPort() const {
    return m_settings->value("Capture/tcp_port", 1235).toInt();
}

void ConfigManager::setTcpPort(int v) {
    m_settings->setValue("Capture/tcp_port", v);
    emit configChanged();
}

QString ConfigManager::ethAdapter() const {
    return m_settings->value("Capture/eth_adapter", "").toString();
}

void ConfigManager::setEthAdapter(const QString& v) {
    m_settings->setValue("Capture/eth_adapter", v);
    emit configChanged();
}

int ConfigManager::ethEthertype() const {
    return m_settings->value("Capture/eth_ethertype", 0x88B5).toInt();
}

void ConfigManager::setEthEthertype(int v) {
    m_settings->setValue("Capture/eth_ethertype", v);
    emit configChanged();
}

int ConfigManager::opencvCaptureIndex() const {
    return m_settings->value("Capture/opencv_capture_index", 0).toInt();
}

void ConfigManager::setOpencvCaptureIndex(int v) {
    m_settings->setValue("Capture/opencv_capture_index", v);
    emit configChanged();
}

QString ConfigManager::opencvCaptureApi() const {
    return m_settings->value("Capture/opencv_capture_api", "DSHOW").toString();
}

void ConfigManager::setOpencvCaptureApi(const QString& v) {
    m_settings->setValue("Capture/opencv_capture_api", v);
    emit configChanged();
}

QString ConfigManager::opencvCaptureUrl() const {
    return m_settings->value("Capture/opencv_capture_url", "").toString();
}

void ConfigManager::setOpencvCaptureUrl(const QString& v) {
    m_settings->setValue("Capture/opencv_capture_url", v);
    emit configChanged();
}

int ConfigManager::opencvCaptureWidth() const {
    return m_settings->value("Capture/opencv_capture_width", 0).toInt();
}

void ConfigManager::setOpencvCaptureWidth(int v) {
    m_settings->setValue("Capture/opencv_capture_width", v);
    emit configChanged();
}

int ConfigManager::opencvCaptureHeight() const {
    return m_settings->value("Capture/opencv_capture_height", 0).toInt();
}

void ConfigManager::setOpencvCaptureHeight(int v) {
    m_settings->setValue("Capture/opencv_capture_height", v);
    emit configChanged();
}

int ConfigManager::opencvCaptureFps() const {
    return m_settings->value("Capture/opencv_capture_fps", 0).toInt();
}

void ConfigManager::setOpencvCaptureFps(int v) {
    m_settings->setValue("Capture/opencv_capture_fps", v);
    emit configChanged();
}

bool ConfigManager::captureMfGpu() const {
    return m_settings->value("Capture/capture_mf_gpu", true).toBool();
}

void ConfigManager::setCaptureMfGpu(bool v) {
    m_settings->setValue("Capture/capture_mf_gpu", v);
    emit configChanged();
}

int ConfigManager::captureCrop() const {
    return m_settings->value("Capture/capture_crop", 0).toInt();
}

void ConfigManager::setCaptureCrop(int v) {
    m_settings->setValue("Capture/capture_crop", v);
    emit configChanged();
}

QString ConfigManager::captureFormat() const {
    return m_settings->value("Capture/capture_format", "NV12").toString();
}

void ConfigManager::setCaptureFormat(const QString& v) {
    m_settings->setValue("Capture/capture_format", v);
    emit configChanged();
}

int ConfigManager::detectionResolution() const {
    return m_settings->value("Capture/detection_resolution", 320).toInt();
}

void ConfigManager::setDetectionResolution(int v) {
    m_settings->setValue("Capture/detection_resolution", v);
    emit configChanged();
}

int ConfigManager::captureFps() const {
    return m_settings->value("Capture/capture_fps", 60).toInt();
}

void ConfigManager::setCaptureFps(int v) {
    m_settings->setValue("Capture/capture_fps", v);
    emit configChanged();
}

bool ConfigManager::circleMask() const {
    return m_settings->value("Capture/circle_mask", true).toBool();
}

void ConfigManager::setCircleMask(bool v) {
    m_settings->setValue("Capture/circle_mask", v);
    emit configChanged();
}

// --- Capture card ---

int ConfigManager::captureCardIndex() const {
    return m_settings->value("Capture/capture_card_index", 0).toInt();
}

void ConfigManager::setCaptureCardIndex(int v) {
    m_settings->setValue("Capture/capture_card_index", v);
    emit configChanged();
}

int ConfigManager::captureCardWidth() const {
    return m_settings->value("Capture/capture_card_width", 0).toInt();
}

void ConfigManager::setCaptureCardWidth(int v) {
    m_settings->setValue("Capture/capture_card_width", v);
    emit configChanged();
}

int ConfigManager::captureCardHeight() const {
    return m_settings->value("Capture/capture_card_height", 0).toInt();
}

void ConfigManager::setCaptureCardHeight(int v) {
    m_settings->setValue("Capture/capture_card_height", v);
    emit configChanged();
}

int ConfigManager::captureCardFps() const {
    return m_settings->value("Capture/capture_card_fps", 0).toInt();
}

void ConfigManager::setCaptureCardFps(int v) {
    m_settings->setValue("Capture/capture_card_fps", v);
    emit configChanged();
}

QString ConfigManager::captureCardFormat() const {
    return m_settings->value("Capture/capture_card_format", "AUTO").toString();
}

void ConfigManager::setCaptureCardFormat(const QString& v) {
    m_settings->setValue("Capture/capture_card_format", v);
    emit configChanged();
}

int ConfigManager::captureCardCropWidth() const {
    return m_settings->value("Capture/capture_card_crop_width", 0).toInt();
}

void ConfigManager::setCaptureCardCropWidth(int v) {
    m_settings->setValue("Capture/capture_card_crop_width", v);
    emit configChanged();
}

int ConfigManager::captureCardCropHeight() const {
    return m_settings->value("Capture/capture_card_crop_height", 0).toInt();
}

void ConfigManager::setCaptureCardCropHeight(int v) {
    m_settings->setValue("Capture/capture_card_crop_height", v);
    emit configChanged();
}

// --- Hardware ---

QString ConfigManager::inputMethod() const {
    return m_settings->value("Hardware/input_method", "WIN32").toString();
}

void ConfigManager::setInputMethod(const QString& v) {
    m_settings->setValue("Hardware/input_method", v);
    emit configChanged();
}

int ConfigManager::arduinoBaudrate() const {
    return m_settings->value("Hardware/arduino_baudrate", 115200).toInt();
}

void ConfigManager::setArduinoBaudrate(int v) {
    m_settings->setValue("Hardware/arduino_baudrate", v);
    emit configChanged();
}

QString ConfigManager::arduinoPort() const {
    return m_settings->value("Hardware/arduino_port", "COM0").toString();
}

void ConfigManager::setArduinoPort(const QString& v) {
    m_settings->setValue("Hardware/arduino_port", v);
    emit configChanged();
}

bool ConfigManager::arduino16BitMouse() const {
    return m_settings->value("Hardware/arduino_16_bit_mouse", false).toBool();
}

void ConfigManager::setArduino16BitMouse(bool v) {
    m_settings->setValue("Hardware/arduino_16_bit_mouse", v);
    emit configChanged();
}

bool ConfigManager::arduinoEnableKeys() const {
    return m_settings->value("Hardware/arduino_enable_keys", false).toBool();
}

void ConfigManager::setArduinoEnableKeys(bool v) {
    m_settings->setValue("Hardware/arduino_enable_keys", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetIp() const {
    return m_settings->value("Hardware/kmbox_net_ip", "10.42.42.42").toString();
}

void ConfigManager::setKmboxNetIp(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_ip", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetPort() const {
    return m_settings->value("Hardware/kmbox_net_port", "1984").toString();
}

void ConfigManager::setKmboxNetPort(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_port", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetUuid() const {
    return m_settings->value("Hardware/kmbox_net_uuid", "DEADC0DE").toString();
}

void ConfigManager::setKmboxNetUuid(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_uuid", v);
    emit configChanged();
}

QString ConfigManager::kmboxAPidvid() const {
    return m_settings->value("Hardware/kmbox_a_pidvid", "").toString();
}

void ConfigManager::setKmboxAPidvid(const QString& v) {
    m_settings->setValue("Hardware/kmbox_a_pidvid", v);
    emit configChanged();
}

int ConfigManager::makcuBaudrate() const {
    return m_settings->value("Hardware/makcu_baudrate", 115200).toInt();
}

void ConfigManager::setMakcuBaudrate(int v) {
    m_settings->setValue("Hardware/makcu_baudrate", v);
    emit configChanged();
}

QString ConfigManager::makcuPort() const {
    return m_settings->value("Hardware/makcu_port", "COM0").toString();
}

void ConfigManager::setMakcuPort(const QString& v) {
    m_settings->setValue("Hardware/makcu_port", v);
    emit configChanged();
}

// --- AI ---

QString ConfigManager::backend() const {
    return m_settings->value("AI/backend", "TRT").toString();
}

void ConfigManager::setBackend(const QString& v) {
    m_settings->setValue("AI/backend", v);
    emit configChanged();
}

int ConfigManager::dmlDeviceId() const {
    return m_settings->value("AI/dml_device_id", 0).toInt();
}

void ConfigManager::setDmlDeviceId(int v) {
    m_settings->setValue("AI/dml_device_id", v);
    emit configChanged();
}

QString ConfigManager::aiModel() const {
    return m_settings->value("AI/ai_model", "sunxds_0.5.6.engine").toString();
}

void ConfigManager::setAiModel(const QString& v) {
    m_settings->setValue("AI/ai_model", v);
    emit configChanged();
}

float ConfigManager::confidenceThreshold() const {
    return m_settings->value("AI/confidence_threshold", 0.15).toFloat();
}

void ConfigManager::setConfidenceThreshold(float v) {
    m_settings->setValue("AI/confidence_threshold", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::nmsThreshold() const {
    return m_settings->value("AI/nms_threshold", 0.50).toFloat();
}

void ConfigManager::setNmsThreshold(float v) {
    m_settings->setValue("AI/nms_threshold", static_cast<double>(v));
    emit configChanged();
}

int ConfigManager::maxDetections() const {
    return m_settings->value("AI/max_detections", 20).toInt();
}

void ConfigManager::setMaxDetections(int v) {
    m_settings->setValue("AI/max_detections", v);
    emit configChanged();
}

bool ConfigManager::exportEnableFp8() const {
    return m_settings->value("AI/export_enable_fp8", false).toBool();
}

void ConfigManager::setExportEnableFp8(bool v) {
    m_settings->setValue("AI/export_enable_fp8", v);
    emit configChanged();
}

bool ConfigManager::exportEnableFp16() const {
    return m_settings->value("AI/export_enable_fp16", true).toBool();
}

void ConfigManager::setExportEnableFp16(bool v) {
    m_settings->setValue("AI/export_enable_fp16", v);
    emit configChanged();
}

bool ConfigManager::smallTargetEnabled() const {
    return m_settings->value("AI/small_target_enabled", false).toBool();
}

void ConfigManager::setSmallTargetEnabled(bool v) {
    m_settings->setValue("AI/small_target_enabled", v);
    emit configChanged();
}

float ConfigManager::smallTargetConfidence() const {
    return m_settings->value("AI/small_target_confidence", 0.08).toFloat();
}

void ConfigManager::setSmallTargetConfidence(float v) {
    m_settings->setValue("AI/small_target_confidence", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::smallTargetAreaFrac() const {
    return m_settings->value("AI/small_target_area_frac", 0.012).toFloat();
}

void ConfigManager::setSmallTargetAreaFrac(float v) {
    m_settings->setValue("AI/small_target_area_frac", static_cast<double>(v));
    emit configChanged();
}

// --- Depth ---

bool ConfigManager::depthInferenceEnabled() const {
    return m_settings->value("Depth/depth_inference_enabled", true).toBool();
}

void ConfigManager::setDepthInferenceEnabled(bool v) {
    m_settings->setValue("Depth/depth_inference_enabled", v);
    emit configChanged();
}

QString ConfigManager::depthModelPath() const {
    return m_settings->value("Depth/depth_model_path", "depth_anything_v2.engine").toString();
}

void ConfigManager::setDepthModelPath(const QString& v) {
    m_settings->setValue("Depth/depth_model_path", v);
    emit configChanged();
}

int ConfigManager::depthFps() const {
    return m_settings->value("Depth/depth_fps", 100).toInt();
}

void ConfigManager::setDepthFps(int v) {
    m_settings->setValue("Depth/depth_fps", v);
    emit configChanged();
}

bool ConfigManager::depthMaskEnabled() const {
    return m_settings->value("Depth/depth_mask_enabled", false).toBool();
}

void ConfigManager::setDepthMaskEnabled(bool v) {
    m_settings->setValue("Depth/depth_mask_enabled", v);
    emit configChanged();
}

int ConfigManager::depthMaskNearPercent() const {
    return m_settings->value("Depth/depth_mask_near_percent", 20).toInt();
}

void ConfigManager::setDepthMaskNearPercent(int v) {
    m_settings->setValue("Depth/depth_mask_near_percent", v);
    emit configChanged();
}

int ConfigManager::depthMaskAlpha() const {
    return m_settings->value("Depth/depth_mask_alpha", 90).toInt();
}

void ConfigManager::setDepthMaskAlpha(int v) {
    m_settings->setValue("Depth/depth_mask_alpha", v);
    emit configChanged();
}

int ConfigManager::depthMaskFps() const {
    return m_settings->value("Depth/depth_mask_fps", 5).toInt();
}

void ConfigManager::setDepthMaskFps(int v) {
    m_settings->setValue("Depth/depth_mask_fps", v);
    emit configChanged();
}

int ConfigManager::depthOptInputSize() const {
    return m_settings->value("Depth/depth_opt_input_size", 518).toInt();
}

void ConfigManager::setDepthOptInputSize(int v) {
    m_settings->setValue("Depth/depth_opt_input_size", v);
    emit configChanged();
}

float ConfigManager::depthNormClipLowPct() const {
    return m_settings->value("Depth/depth_norm_clip_low_pct", 0.0).toFloat();
}

void ConfigManager::setDepthNormClipLowPct(float v) {
    m_settings->setValue("Depth/depth_norm_clip_low_pct", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::depthNormClipHighPct() const {
    return m_settings->value("Depth/depth_norm_clip_high_pct", 100.0).toFloat();
}

void ConfigManager::setDepthNormClipHighPct(float v) {
    m_settings->setValue("Depth/depth_norm_clip_high_pct", static_cast<double>(v));
    emit configChanged();
}

bool ConfigManager::depthShowHeatmap() const {
    return m_settings->value("Depth/depth_show_heatmap", false).toBool();
}

void ConfigManager::setDepthShowHeatmap(bool v) {
    m_settings->setValue("Depth/depth_show_heatmap", v);
    emit configChanged();
}

float ConfigManager::depthHeatmapGamma() const {
    return m_settings->value("Depth/depth_heatmap_gamma", 1.0).toFloat();
}

void ConfigManager::setDepthHeatmapGamma(float v) {
    m_settings->setValue("Depth/depth_heatmap_gamma", static_cast<double>(v));
    emit configChanged();
}

bool ConfigManager::depthShowBboxDistance() const {
    return m_settings->value("Depth/depth_show_bbox_distance", false).toBool();
}

void ConfigManager::setDepthShowBboxDistance(bool v) {
    m_settings->setValue("Depth/depth_show_bbox_distance", v);
    emit configChanged();
}

int ConfigManager::depthMaskExpand() const {
    return m_settings->value("Depth/depth_mask_expand", 0).toInt();
}

void ConfigManager::setDepthMaskExpand(int v) {
    m_settings->setValue("Depth/depth_mask_expand", v);
    emit configChanged();
}

int ConfigManager::depthMaskHoldFrames() const {
    return m_settings->value("Depth/depth_mask_hold_frames", 5).toInt();
}

void ConfigManager::setDepthMaskHoldFrames(int v) {
    m_settings->setValue("Depth/depth_mask_hold_frames", v);
    emit configChanged();
}

float ConfigManager::depthMaskSuppressionRatio() const {
    return m_settings->value("Depth/depth_mask_suppression_ratio", 0.30).toFloat();
}

void ConfigManager::setDepthMaskSuppressionRatio(float v) {
    m_settings->setValue("Depth/depth_mask_suppression_ratio", static_cast<double>(v));
    emit configChanged();
}

bool ConfigManager::depthMaskInvert() const {
    return m_settings->value("Depth/depth_mask_invert", false).toBool();
}

void ConfigManager::setDepthMaskInvert(bool v) {
    m_settings->setValue("Depth/depth_mask_invert", v);
    emit configChanged();
}

int ConfigManager::depthColormap() const {
    return m_settings->value("Depth/depth_colormap", 2).toInt();
}

void ConfigManager::setDepthColormap(int v) {
    m_settings->setValue("Depth/depth_colormap", v);
    emit configChanged();
}

// --- Overlay ---

int ConfigManager::overlayOpacity() const {
    return m_settings->value("Overlay/overlay_opacity", 240).toInt();
}

void ConfigManager::setOverlayOpacity(int v) {
    m_settings->setValue("Overlay/overlay_opacity", v);
    emit configChanged();
}

float ConfigManager::overlayUiScale() const {
    return m_settings->value("Overlay/overlay_ui_scale", 1.0).toFloat();
}

void ConfigManager::setOverlayUiScale(float v) {
    m_settings->setValue("Overlay/overlay_ui_scale", static_cast<double>(v));
    emit configChanged();
}

// --- Debug ---

bool ConfigManager::showFps() const {
    return m_settings->value("Debug/show_fps", false).toBool();
}

void ConfigManager::setShowFps(bool v) {
    m_settings->setValue("Debug/show_fps", v);
    emit configChanged();
}

bool ConfigManager::verbose() const {
    return m_settings->value("Debug/verbose", false).toBool();
}

void ConfigManager::setVerbose(bool v) {
    m_settings->setValue("Debug/verbose", v);
    emit configChanged();
}

int ConfigManager::screenshotDelay() const {
    return m_settings->value("Debug/screenshot_delay", 100).toInt();
}

void ConfigManager::setScreenshotDelay(int v) {
    m_settings->setValue("Debug/screenshot_delay", v);
    emit configChanged();
}

bool ConfigManager::showWindow() const {
    return m_settings->value("Debug/show_window", false).toBool();
}

void ConfigManager::setShowWindow(bool v) {
    m_settings->setValue("Debug/show_window", v);
    emit configChanged();
}

bool ConfigManager::replayRecordEnabled() const {
    return m_settings->value("Debug/replay_record_enabled", false).toBool();
}

void ConfigManager::setReplayRecordEnabled(bool v) {
    m_settings->setValue("Debug/replay_record_enabled", v);
    emit configChanged();
}

int ConfigManager::replaySeconds() const {
    return m_settings->value("Debug/replay_seconds", 10).toInt();
}

void ConfigManager::setReplaySeconds(int v) {
    m_settings->setValue("Debug/replay_seconds", v);
    emit configChanged();
}

float ConfigManager::replayPlaybackSpeed() const {
    return m_settings->value("Debug/replay_playback_speed", 0.25).toFloat();
}

void ConfigManager::setReplayPlaybackSpeed(float v) {
    m_settings->setValue("Debug/replay_playback_speed", static_cast<double>(v));
    emit configChanged();
}

// --- Macro ---

bool ConfigManager::macroEnabled() const {
    return m_settings->value("Macro/macro_enabled", false).toBool();
}

void ConfigManager::setMacroEnabled(bool v) {
    m_settings->setValue("Macro/macro_enabled", v);
    emit configChanged();
}

QString ConfigManager::macroScriptPath() const {
    return m_settings->value("Macro/macro_script_path", "").toString();
}

void ConfigManager::setMacroScriptPath(const QString& v) {
    m_settings->setValue("Macro/macro_script_path", v);
    emit configChanged();
}

bool ConfigManager::macroPrimaryButtonEvents() const {
    return m_settings->value("Macro/macro_primary_button_events", false).toBool();
}

void ConfigManager::setMacroPrimaryButtonEvents(bool v) {
    m_settings->setValue("Macro/macro_primary_button_events", v);
    emit configChanged();
}

// --- Crosshair ---

int ConfigManager::crosshairRectW() const {
    return m_settings->value("Crosshair/crosshair_rect_w", 40).toInt();
}

void ConfigManager::setCrosshairRectW(int v) {
    m_settings->setValue("Crosshair/crosshair_rect_w", v);
    emit configChanged();
}

int ConfigManager::crosshairRectH() const {
    return m_settings->value("Crosshair/crosshair_rect_h", 40).toInt();
}

void ConfigManager::setCrosshairRectH(int v) {
    m_settings->setValue("Crosshair/crosshair_rect_h", v);
    emit configChanged();
}

int ConfigManager::crosshairMinPixelCount() const {
    return m_settings->value("Crosshair/crosshair_min_pixel_count", 4).toInt();
}

void ConfigManager::setCrosshairMinPixelCount(int v) {
    m_settings->setValue("Crosshair/crosshair_min_pixel_count", v);
    emit configChanged();
}

int ConfigManager::crosshairCloseRadius() const {
    return m_settings->value("Crosshair/crosshair_close_radius", 1).toInt();
}

void ConfigManager::setCrosshairCloseRadius(int v) {
    m_settings->setValue("Crosshair/crosshair_close_radius", v);
    emit configChanged();
}

float ConfigManager::crosshairSmooth() const {
    return m_settings->value("Crosshair/crosshair_smooth", 0.5).toFloat();
}

void ConfigManager::setCrosshairSmooth(float v) {
    m_settings->setValue("Crosshair/crosshair_smooth", static_cast<double>(v));
    emit configChanged();
}

QList<ConfigManager::ColorProfile> ConfigManager::crosshairColors() const {
    QList<ColorProfile> result;
    int i = 0;
    while (m_settings->contains(
        QStringLiteral("crosshair_color.%1/name").arg(i))) {
        auto prefix = QStringLiteral("crosshair_color.%1/").arg(i);
        ColorProfile c;
        c.name    = m_settings->value(prefix + "name", "Color").toString();
        c.enabled = m_settings->value(prefix + "enabled", true).toBool();
        c.hLow    = m_settings->value(prefix + "h_low", 0).toInt();
        c.hHigh   = m_settings->value(prefix + "h_high", 10).toInt();
        c.sMin    = m_settings->value(prefix + "s_min", 120).toInt();
        c.sMax    = m_settings->value(prefix + "s_max", 255).toInt();
        c.vMin    = m_settings->value(prefix + "v_min", 120).toInt();
        c.vMax    = m_settings->value(prefix + "v_max", 255).toInt();
        result.append(c);
        ++i;
    }
    if (result.isEmpty()) {
        ColorProfile low;
        low.name = QStringLiteral("Red-Low");  low.hLow = 0;   low.hHigh = 10;
        ColorProfile hi;
        hi.name  = QStringLiteral("Red-High"); hi.hLow  = 160; hi.hHigh  = 179;
        result.append(low);
        result.append(hi);
    }
    return result;
}

void ConfigManager::setCrosshairColors(const QList<ColorProfile>& colors) {
    // Remove old entries
    int old = 0;
    while (m_settings->contains(
        QStringLiteral("crosshair_color.%1/name").arg(old))) {
        m_settings->remove(QStringLiteral("crosshair_color.%1").arg(old));
        ++old;
    }
    // Write new entries
    for (int i = 0; i < colors.size(); ++i) {
        auto prefix = QStringLiteral("crosshair_color.%1/").arg(i);
        const auto& c = colors[i];
        m_settings->setValue(prefix + "name",    c.name);
        m_settings->setValue(prefix + "enabled", c.enabled);
        m_settings->setValue(prefix + "h_low",   c.hLow);
        m_settings->setValue(prefix + "h_high",  c.hHigh);
        m_settings->setValue(prefix + "s_min",   c.sMin);
        m_settings->setValue(prefix + "s_max",   c.sMax);
        m_settings->setValue(prefix + "v_min",   c.vMin);
        m_settings->setValue(prefix + "v_max",   c.vMax);
    }
    emit configChanged();
}

// --- Laser ---

int ConfigManager::laserRectW() const {
    return m_settings->value("Laser/laser_rect_w", 160).toInt();
}

void ConfigManager::setLaserRectW(int v) {
    m_settings->setValue("Laser/laser_rect_w", v);
    emit configChanged();
}

int ConfigManager::laserRectH() const {
    return m_settings->value("Laser/laser_rect_h", 240).toInt();
}

void ConfigManager::setLaserRectH(int v) {
    m_settings->setValue("Laser/laser_rect_h", v);
    emit configChanged();
}

int ConfigManager::laserCenterX() const {
    return m_settings->value("Laser/laser_center_x", 160).toInt();
}

void ConfigManager::setLaserCenterX(int v) {
    m_settings->setValue("Laser/laser_center_x", v);
    emit configChanged();
}

int ConfigManager::laserCenterY() const {
    return m_settings->value("Laser/laser_center_y", 200).toInt();
}

void ConfigManager::setLaserCenterY(int v) {
    m_settings->setValue("Laser/laser_center_y", v);
    emit configChanged();
}

int ConfigManager::laserTargetCenterX() const {
    return m_settings->value("Laser/laser_target_center_x", 160).toInt();
}

void ConfigManager::setLaserTargetCenterX(int v) {
    m_settings->setValue("Laser/laser_target_center_x", v);
    emit configChanged();
}

int ConfigManager::laserTargetCenterY() const {
    return m_settings->value("Laser/laser_target_center_y", 160).toInt();
}

void ConfigManager::setLaserTargetCenterY(int v) {
    m_settings->setValue("Laser/laser_target_center_y", v);
    emit configChanged();
}

int ConfigManager::laserTargetRectW() const {
    return m_settings->value("Laser/laser_target_rect_w", 60).toInt();
}

void ConfigManager::setLaserTargetRectW(int v) {
    m_settings->setValue("Laser/laser_target_rect_w", v);
    emit configChanged();
}

int ConfigManager::laserTargetRectH() const {
    return m_settings->value("Laser/laser_target_rect_h", 60).toInt();
}

void ConfigManager::setLaserTargetRectH(int v) {
    m_settings->setValue("Laser/laser_target_rect_h", v);
    emit configChanged();
}

float ConfigManager::laserMinElongation() const {
    return m_settings->value("Laser/laser_min_elongation", 3.0).toFloat();
}

void ConfigManager::setLaserMinElongation(float v) {
    m_settings->setValue("Laser/laser_min_elongation", static_cast<double>(v));
    emit configChanged();
}

int ConfigManager::laserMinPixelCount() const {
    return m_settings->value("Laser/laser_min_pixel_count", 10).toInt();
}

void ConfigManager::setLaserMinPixelCount(int v) {
    m_settings->setValue("Laser/laser_min_pixel_count", v);
    emit configChanged();
}

int ConfigManager::laserCloseRadius() const {
    return m_settings->value("Laser/laser_close_radius", 1).toInt();
}

void ConfigManager::setLaserCloseRadius(int v) {
    m_settings->setValue("Laser/laser_close_radius", v);
    emit configChanged();
}

float ConfigManager::laserSmooth() const {
    return m_settings->value("Laser/laser_smooth", 0.5).toFloat();
}

void ConfigManager::setLaserSmooth(float v) {
    m_settings->setValue("Laser/laser_smooth", static_cast<double>(v));
    emit configChanged();
}

QList<ConfigManager::ColorProfile> ConfigManager::laserColors() const {
    QList<ColorProfile> result;
    int i = 0;
    while (m_settings->contains(
        QStringLiteral("laser_color.%1/name").arg(i))) {
        auto prefix = QStringLiteral("laser_color.%1/").arg(i);
        ColorProfile c;
        c.name    = m_settings->value(prefix + "name", "Color").toString();
        c.enabled = m_settings->value(prefix + "enabled", true).toBool();
        c.hLow    = m_settings->value(prefix + "h_low", 0).toInt();
        c.hHigh   = m_settings->value(prefix + "h_high", 10).toInt();
        c.sMin    = m_settings->value(prefix + "s_min", 45).toInt();
        c.sMax    = m_settings->value(prefix + "s_max", 255).toInt();
        c.vMin    = m_settings->value(prefix + "v_min", 50).toInt();
        c.vMax    = m_settings->value(prefix + "v_max", 255).toInt();
        result.append(c);
        ++i;
    }
    if (result.isEmpty()) {
        ColorProfile low;
        low.name = QStringLiteral("Laser-Red-Low");  low.hLow = 0;   low.hHigh = 10;
        low.sMin = 45; low.vMin = 50;
        ColorProfile hi;
        hi.name  = QStringLiteral("Laser-Red-High"); hi.hLow  = 160; hi.hHigh  = 179;
        hi.sMin = 45; hi.vMin = 50;
        result.append(low);
        result.append(hi);
    }
    return result;
}

void ConfigManager::setLaserColors(const QList<ColorProfile>& colors) {
    // Remove old entries
    int old = 0;
    while (m_settings->contains(
        QStringLiteral("laser_color.%1/name").arg(old))) {
        m_settings->remove(QStringLiteral("laser_color.%1").arg(old));
        ++old;
    }
    // Write new entries
    for (int i = 0; i < colors.size(); ++i) {
        auto prefix = QStringLiteral("laser_color.%1/").arg(i);
        const auto& c = colors[i];
        m_settings->setValue(prefix + "name",    c.name);
        m_settings->setValue(prefix + "enabled", c.enabled);
        m_settings->setValue(prefix + "h_low",   c.hLow);
        m_settings->setValue(prefix + "h_high",  c.hHigh);
        m_settings->setValue(prefix + "s_min",   c.sMin);
        m_settings->setValue(prefix + "s_max",   c.sMax);
        m_settings->setValue(prefix + "v_min",   c.vMin);
        m_settings->setValue(prefix + "v_max",   c.vMax);
    }
    emit configChanged();
}

// --- Flashlight halo ---

bool ConfigManager::flashlightShowPreview() const {
    return m_settings->value("Flashlight/flashlight_show_preview", false).toBool();
}
void ConfigManager::setFlashlightShowPreview(bool v) {
    m_settings->setValue("Flashlight/flashlight_show_preview", v);
    emit configChanged();
}

int ConfigManager::flashlightBrightnessThreshold() const {
    return m_settings->value("Flashlight/flashlight_brightness_threshold", 220).toInt();
}
void ConfigManager::setFlashlightBrightnessThreshold(int v) {
    m_settings->setValue("Flashlight/flashlight_brightness_threshold", v);
    emit configChanged();
}

int ConfigManager::flashlightMinRadius() const {
    return m_settings->value("Flashlight/flashlight_min_radius", 5).toInt();
}
void ConfigManager::setFlashlightMinRadius(int v) {
    m_settings->setValue("Flashlight/flashlight_min_radius", v);
    emit configChanged();
}

int ConfigManager::flashlightMaxRadius() const {
    return m_settings->value("Flashlight/flashlight_max_radius", 200).toInt();
}
void ConfigManager::setFlashlightMaxRadius(int v) {
    m_settings->setValue("Flashlight/flashlight_max_radius", v);
    emit configChanged();
}

float ConfigManager::flashlightMinCircularity() const {
    return m_settings->value("Flashlight/flashlight_min_circularity", 0.60).toFloat();
}
void ConfigManager::setFlashlightMinCircularity(float v) {
    m_settings->setValue("Flashlight/flashlight_min_circularity", static_cast<double>(v));
    emit configChanged();
}

int ConfigManager::flashlightOpenRadius() const {
    return m_settings->value("Flashlight/flashlight_open_radius", 1).toInt();
}
void ConfigManager::setFlashlightOpenRadius(int v) {
    m_settings->setValue("Flashlight/flashlight_open_radius", v);
    emit configChanged();
}

int ConfigManager::flashlightMinLocalContrast() const {
    return m_settings->value("Flashlight/flashlight_min_local_contrast", 30).toInt();
}
void ConfigManager::setFlashlightMinLocalContrast(int v) {
    m_settings->setValue("Flashlight/flashlight_min_local_contrast", v);
    emit configChanged();
}

int ConfigManager::flashlightMaxSpots() const {
    return m_settings->value("Flashlight/flashlight_max_spots", 3).toInt();
}
void ConfigManager::setFlashlightMaxSpots(int v) {
    m_settings->setValue("Flashlight/flashlight_max_spots", v);
    emit configChanged();
}

// --- Glass filter ---

bool ConfigManager::glassFilterShowPreview() const {
    return m_settings->value("Glass/glass_filter_show_preview", false).toBool();
}
void ConfigManager::setGlassFilterShowPreview(bool v) {
    m_settings->setValue("Glass/glass_filter_show_preview", v);
    emit configChanged();
}

float ConfigManager::glassEdgeRingFrac() const {
    return m_settings->value("Glass/glass_edge_ring_frac", 0.15).toFloat();
}
void ConfigManager::setGlassEdgeRingFrac(float v) {
    m_settings->setValue("Glass/glass_edge_ring_frac", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::glassCoverageThreshold() const {
    return m_settings->value("Glass/glass_coverage_threshold", 0.45).toFloat();
}
void ConfigManager::setGlassCoverageThreshold(float v) {
    m_settings->setValue("Glass/glass_coverage_threshold", static_cast<double>(v));
    emit configChanged();
}

int ConfigManager::glassMinBoxShortSide() const {
    return m_settings->value("Glass/glass_min_box_short_side", 20).toInt();
}
void ConfigManager::setGlassMinBoxShortSide(int v) {
    m_settings->setValue("Glass/glass_min_box_short_side", v);
    emit configChanged();
}

QList<ConfigManager::ColorProfile> ConfigManager::glassColors() const {
    QList<ColorProfile> result;
    int i = 0;
    while (m_settings->contains(
        QStringLiteral("glass_color.%1/name").arg(i))) {
        auto prefix = QStringLiteral("glass_color.%1/").arg(i);
        ColorProfile c;
        c.name    = m_settings->value(prefix + "name", "Glass").toString();
        c.enabled = m_settings->value(prefix + "enabled", true).toBool();
        c.hLow    = m_settings->value(prefix + "h_low", 90).toInt();
        c.hHigh   = m_settings->value(prefix + "h_high", 115).toInt();
        c.sMin    = m_settings->value(prefix + "s_min", 5).toInt();
        c.sMax    = m_settings->value(prefix + "s_max", 90).toInt();
        c.vMin    = m_settings->value(prefix + "v_min", 170).toInt();
        c.vMax    = m_settings->value(prefix + "v_max", 255).toInt();
        result.append(c);
        ++i;
    }
    if (result.isEmpty()) {
        ColorProfile blue;
        blue.name = QStringLiteral("Glass-Blue");
        blue.hLow = 90; blue.hHigh = 115;
        blue.sMin = 5;  blue.sMax  = 90;
        blue.vMin = 170; blue.vMax = 255;
        ColorProfile green;
        green.name = QStringLiteral("Glass-Green");
        green.hLow = 55; green.hHigh = 85;
        green.sMin = 5;  green.sMax  = 90;
        green.vMin = 170; green.vMax = 255;
        result.append(blue);
        result.append(green);
    }
    return result;
}

void ConfigManager::setGlassColors(const QList<ColorProfile>& colors) {
    int old = 0;
    while (m_settings->contains(
        QStringLiteral("glass_color.%1/name").arg(old))) {
        m_settings->remove(QStringLiteral("glass_color.%1").arg(old));
        ++old;
    }
    for (int i = 0; i < colors.size(); ++i) {
        auto prefix = QStringLiteral("glass_color.%1/").arg(i);
        const auto& c = colors[i];
        m_settings->setValue(prefix + "name",    c.name);
        m_settings->setValue(prefix + "enabled", c.enabled);
        m_settings->setValue(prefix + "h_low",   c.hLow);
        m_settings->setValue(prefix + "h_high",  c.hHigh);
        m_settings->setValue(prefix + "s_min",   c.sMin);
        m_settings->setValue(prefix + "s_max",   c.sMax);
        m_settings->setValue(prefix + "v_min",   c.vMin);
        m_settings->setValue(prefix + "v_max",   c.vMax);
    }
    emit configChanged();
}

// --- Active hotkey group ---

QString ConfigManager::activeHotkeyGroup() const {
    return m_settings ? m_settings->value("active_hotkey_group",
        QStringLiteral("\xe9\xbb\x98\xe8\xae\xa4")).toString() : QStringLiteral("\xe9\xbb\x98\xe8\xae\xa4");
}
void ConfigManager::setActiveHotkeyGroup(const QString& v) {
    if (m_settings) m_settings->setValue("active_hotkey_group", v);
    emit configChanged();
}

// --- Hotkeys ---

int ConfigManager::hotkeyCount() const {
    if (!m_settings)
        return 0;

    int count = 0;
    while (m_settings->contains(
        QStringLiteral("Hotkey_%1/name").arg(count))) {
        ++count;
    }
    return count;
}

void ConfigManager::writeHotkeyToSettings(int index, const HotkeyData& data) {
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);
    m_settings->setValue(prefix + "name", data.name);
    m_settings->setValue(prefix + "group", data.group);
    m_settings->setValue(prefix + "keys", data.keys.join(","));
    m_settings->setValue(prefix + "fovX", data.fovX);
    m_settings->setValue(prefix + "fovY", data.fovY);
    m_settings->setValue(prefix + "speed_x",         static_cast<double>(data.speedX));
    m_settings->setValue(prefix + "speed_y",         static_cast<double>(data.speedY));
    m_settings->setValue(prefix + "dead_zone_px",    static_cast<double>(data.deadZonePx));
    m_settings->setValue(prefix + "smart_trigger_enabled",    data.smartTriggerEnabled);
    m_settings->setValue(prefix + "smart_trigger_hit_scale",  static_cast<double>(data.smartTriggerHitScale));
    m_settings->setValue(prefix + "smart_trigger_aggression", static_cast<double>(data.smartTriggerAggression));
    m_settings->setValue(prefix + "laser_detect_enabled", data.laserDetectEnabled);
    m_settings->setValue(prefix + "crosshair_detect_enabled", data.crosshairDetectEnabled);
    m_settings->setValue(prefix + "flashlight_detect_enabled", data.flashlightDetectEnabled);
    m_settings->setValue(prefix + "glass_filter_enabled", data.glassFilterEnabled);
    m_settings->setValue(prefix + "lock_aggression", static_cast<double>(data.lockAggression));
    m_settings->setValue(prefix + "y_offset_size_decay_enabled", data.yOffsetSizeDecayEnabled);
    m_settings->setValue(prefix + "dynamic_fov_enabled", data.dynamicFovEnabled);
    m_settings->setValue(prefix + "dynamic_fov_strength", static_cast<double>(data.dynamicFovStrength));
    m_settings->setValue(prefix + "aim_path_mode", data.aimPathMode);
    m_settings->setValue(prefix + "aim_path_bezier_cx1", static_cast<double>(data.aimPathBezierCx1));
    m_settings->setValue(prefix + "aim_path_bezier_cy1", static_cast<double>(data.aimPathBezierCy1));
    m_settings->setValue(prefix + "aim_path_bezier_cx2", static_cast<double>(data.aimPathBezierCx2));
    m_settings->setValue(prefix + "aim_path_bezier_cy2", static_cast<double>(data.aimPathBezierCy2));
    m_settings->setValue(prefix + "aim_path_custom_samples", data.aimPathCustomSamples);
}

ConfigManager::HotkeyData ConfigManager::readHotkeyFromSettings(int index) const {
    HotkeyData data;
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);

    data.name = m_settings->value(prefix + "name", "Aim").toString();
    data.group = m_settings->value(prefix + "group", QString::fromUtf8(u8"\xe9\xbb\x98\xe8\xae\xa4")).toString();
    data.keys = m_settings->value(prefix + "keys", "RightMouseButton").toString().split(",", Qt::SkipEmptyParts);
    data.fovX = m_settings->value(prefix + "fovX", 106).toInt();
    data.fovY = m_settings->value(prefix + "fovY", 74).toInt();
    data.speedX        = m_settings->value(prefix + "speed_x",        0.6).toFloat();
    data.speedY        = m_settings->value(prefix + "speed_y",        0.6).toFloat();
    data.deadZonePx    = m_settings->value(prefix + "dead_zone_px", 2.0).toFloat();
    data.smartTriggerEnabled    = m_settings->value(prefix + "smart_trigger_enabled", false).toBool();
    data.smartTriggerHitScale   = m_settings->value(prefix + "smart_trigger_hit_scale", 0.60).toFloat();
    data.smartTriggerAggression = m_settings->value(prefix + "smart_trigger_aggression", 0.50).toFloat();
    data.laserDetectEnabled = m_settings->value(prefix + "laser_detect_enabled", false).toBool();
    data.crosshairDetectEnabled = m_settings->value(prefix + "crosshair_detect_enabled", false).toBool();
    data.flashlightDetectEnabled = m_settings->value(prefix + "flashlight_detect_enabled", false).toBool();
    data.glassFilterEnabled = m_settings->value(prefix + "glass_filter_enabled", false).toBool();
    data.lockAggression = m_settings->value(prefix + "lock_aggression", 0.30).toFloat();
    data.yOffsetSizeDecayEnabled = m_settings->value(prefix + "y_offset_size_decay_enabled", false).toBool();
    data.dynamicFovEnabled = m_settings->value(prefix + "dynamic_fov_enabled", false).toBool();
    data.dynamicFovStrength = m_settings->value(prefix + "dynamic_fov_strength", 0.60).toFloat();
    data.aimPathMode = m_settings->value(prefix + "aim_path_mode", 0).toInt();
    data.aimPathBezierCx1 = m_settings->value(prefix + "aim_path_bezier_cx1", 0.30).toFloat();
    data.aimPathBezierCy1 = m_settings->value(prefix + "aim_path_bezier_cy1", 0.00).toFloat();
    data.aimPathBezierCx2 = m_settings->value(prefix + "aim_path_bezier_cx2", 0.70).toFloat();
    data.aimPathBezierCy2 = m_settings->value(prefix + "aim_path_bezier_cy2", 0.00).toFloat();
    data.aimPathCustomSamples = m_settings->value(prefix + "aim_path_custom_samples", QString()).toString();

    return data;
}

ConfigManager::HotkeyData ConfigManager::hotkey(int index) const {
    if (index < 0 || index >= hotkeyCount())
        return {};
    return readHotkeyFromSettings(index);
}

void ConfigManager::setHotkey(int index, const HotkeyData& data) {
    if (index < 0 || index >= hotkeyCount())
        return;
    writeHotkeyToSettings(index, data);
    emit configChanged();
}

void ConfigManager::addHotkey(const HotkeyData& data) {
    int index = hotkeyCount();
    writeHotkeyToSettings(index, data);
    emit configChanged();
}

void ConfigManager::removeHotkey(int index) {
    int count = hotkeyCount();
    if (index < 0 || index >= count || count <= 1)
        return;

    m_settings->remove(QStringLiteral("Hotkey_%1").arg(index));
    reindexHotkeys();
    emit configChanged();
}

void ConfigManager::reindexHotkeys() {
    QVector<HotkeyData> all;
    int i = 0;
    while (m_settings->contains(QStringLiteral("Hotkey_%1/name").arg(i))) {
        all.append(readHotkeyFromSettings(i));
        ++i;
    }

    for (int k = 0; k < i; ++k)
        m_settings->remove(QStringLiteral("Hotkey_%1").arg(k));

    for (int k = 0; k < all.size(); ++k)
        writeHotkeyToSettings(k, all[k]);
}
