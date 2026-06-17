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
    m_settings = new QSettings(m_path, QSettings::IniFormat, this);

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
    if (!m_settings)
        return false;
    m_settings->sync();
    return m_settings->status() == QSettings::NoError;
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
    m_settings->setValue(prefix + "keys", data.keys.join(","));
    m_settings->setValue(prefix + "fovX", data.fovX);
    m_settings->setValue(prefix + "fovY", data.fovY);
    m_settings->setValue(prefix + "speed_x", static_cast<double>(data.speedX));
    m_settings->setValue(prefix + "speed_y", static_cast<double>(data.speedY));
    m_settings->setValue(prefix + "lock_strength", static_cast<double>(data.lockStrength));
    m_settings->setValue(prefix + "lock_radius_px", static_cast<double>(data.lockRadiusPx));
    m_settings->setValue(prefix + "aim_trajectory_mode", data.aimTrajectoryMode);
    m_settings->setValue(prefix + "bezier_cx1", static_cast<double>(data.bezierCx1));
    m_settings->setValue(prefix + "bezier_cy1", static_cast<double>(data.bezierCy1));
    m_settings->setValue(prefix + "bezier_cx2", static_cast<double>(data.bezierCx2));
    m_settings->setValue(prefix + "bezier_cy2", static_cast<double>(data.bezierCy2));
    m_settings->setValue(prefix + "kalman_enabled", data.kalmanEnabled);
    m_settings->setValue(prefix + "kalman_process_noise_position", static_cast<double>(data.kalmanProcessNoisePos));
    m_settings->setValue(prefix + "kalman_process_noise_velocity", static_cast<double>(data.kalmanProcessNoiseVel));
    m_settings->setValue(prefix + "kalman_measurement_noise", static_cast<double>(data.kalmanMeasurementNoise));
    m_settings->setValue(prefix + "smart_trigger_enabled", data.smartTriggerEnabled);
    m_settings->setValue(prefix + "crosshair_detect_enabled", data.crosshairDetectEnabled);
    m_settings->setValue(prefix + "dynamic_fov_enabled", data.dynamicFovEnabled);
    m_settings->setValue(prefix + "dynamic_fov_margin_frac", static_cast<double>(data.dynamicFovMarginFrac));
    m_settings->setValue(prefix + "dynamic_fov_min_radius_frac", static_cast<double>(data.dynamicFovMinRadiusFrac));
}

ConfigManager::HotkeyData ConfigManager::readHotkeyFromSettings(int index) const {
    HotkeyData data;
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);

    data.name = m_settings->value(prefix + "name", "Aim").toString();
    data.keys = m_settings->value(prefix + "keys", "RightMouseButton").toString().split(",", Qt::SkipEmptyParts);
    data.fovX = m_settings->value(prefix + "fovX", 106).toInt();
    data.fovY = m_settings->value(prefix + "fovY", 74).toInt();
    data.speedX = m_settings->value(prefix + "speed_x", 0.6).toFloat();
    data.speedY = m_settings->value(prefix + "speed_y", 0.6).toFloat();
    data.lockStrength = m_settings->value(prefix + "lock_strength", 0.0).toFloat();
    data.lockRadiusPx = m_settings->value(prefix + "lock_radius_px", 25.0).toFloat();
    data.aimTrajectoryMode = m_settings->value(prefix + "aim_trajectory_mode", 0).toInt();
    data.bezierCx1 = m_settings->value(prefix + "bezier_cx1", 0.30).toFloat();
    data.bezierCy1 = m_settings->value(prefix + "bezier_cy1", 0.25).toFloat();
    data.bezierCx2 = m_settings->value(prefix + "bezier_cx2", 0.70).toFloat();
    data.bezierCy2 = m_settings->value(prefix + "bezier_cy2", -0.15).toFloat();
    data.kalmanEnabled = m_settings->value(prefix + "kalman_enabled", true).toBool();
    data.kalmanProcessNoisePos = m_settings->value(prefix + "kalman_process_noise_position", 40.0).toFloat();
    data.kalmanProcessNoiseVel = m_settings->value(prefix + "kalman_process_noise_velocity", 1800.0).toFloat();
    data.kalmanMeasurementNoise = m_settings->value(prefix + "kalman_measurement_noise", 35.0).toFloat();
    data.smartTriggerEnabled = m_settings->value(prefix + "smart_trigger_enabled", false).toBool();
    data.crosshairDetectEnabled = m_settings->value(prefix + "crosshair_detect_enabled", false).toBool();
    data.dynamicFovEnabled = m_settings->value(prefix + "dynamic_fov_enabled", false).toBool();
    data.dynamicFovMarginFrac = m_settings->value(prefix + "dynamic_fov_margin_frac", 1.10).toFloat();
    data.dynamicFovMinRadiusFrac = m_settings->value(prefix + "dynamic_fov_min_radius_frac", 0.20).toFloat();

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
