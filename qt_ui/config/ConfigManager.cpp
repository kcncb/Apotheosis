#include "config/ConfigManager.h"

#include "config.h"   // kFixedMaxDetections

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
        setDetectionResolution(320);
        setCaptureFps(60);
        setCircleMask(true);

        // ★ 2026-09-17: setBackend 已删除(backend 恒为 TRT); maxDetections 固定 20。
        setAiModel("sunxds_0.5.6.engine");
        setConfidenceThreshold(0.10f);
        setNmsThreshold(0.50f);
        setMaxDetections(kFixedMaxDetections);

        setInputMethod("MAKCU");

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

void ConfigManager::notifyRuntimeReloaded() {
    emit configLoaded();
}

// --- Capture: 只有「采集卡」一种方式 ---
//
// 旧的 capture_method / udp_* / tcp_* / eth_* / opencv_capture_* / capture_crop /
// capture_mf_gpu 键已全部废弃。这里不再提供它们的读写接口, 老 QSettings 里的
// 残留值不会被读取, 也不会再被写回。

QString ConfigManager::captureDevice() const {
    return m_settings->value("Capture/capture_device", "").toString();
}

void ConfigManager::setCaptureDevice(const QString& v) {
    m_settings->setValue("Capture/capture_device", v);
    emit configChanged();
}

QString ConfigManager::captureFormat() const {
    return m_settings->value("Capture/capture_format", "").toString();
}

void ConfigManager::setCaptureFormat(const QString& v) {
    m_settings->setValue("Capture/capture_format", v);
    emit configChanged();
}

int ConfigManager::captureWidth() const {
    return m_settings->value("Capture/capture_width", 0).toInt();
}

void ConfigManager::setCaptureWidth(int v) {
    m_settings->setValue("Capture/capture_width", v);
    emit configChanged();
}

int ConfigManager::captureHeight() const {
    return m_settings->value("Capture/capture_height", 0).toInt();
}

void ConfigManager::setCaptureHeight(int v) {
    m_settings->setValue("Capture/capture_height", v);
    emit configChanged();
}

int ConfigManager::captureFps() const {
    return m_settings->value("Capture/capture_fps", 0).toInt();
}

void ConfigManager::setCaptureFps(int v) {
    m_settings->setValue("Capture/capture_fps", v);
    emit configChanged();
}

bool ConfigManager::captureGpuDecode() const {
    return m_settings->value("Capture/capture_gpu_decode", true).toBool();
}

void ConfigManager::setCaptureGpuDecode(bool v) {
    m_settings->setValue("Capture/capture_gpu_decode", v);
    emit configChanged();
}

int ConfigManager::detectionResolution() const {
    return m_settings->value("Capture/detection_resolution", 320).toInt();
}

void ConfigManager::setDetectionResolution(int v) {
    m_settings->setValue("Capture/detection_resolution", v);
    emit configChanged();
}

bool ConfigManager::circleMask() const {
    return m_settings->value("Capture/circle_mask", true).toBool();
}

void ConfigManager::setCircleMask(bool v) {
    m_settings->setValue("Capture/circle_mask", v);
    emit configChanged();
}

// --- Hardware ---

QString ConfigManager::inputMethod() const {
    const QString value = m_settings->value("Hardware/input_method", "MAKCU").toString();
    return value == QStringLiteral("MAKCUNEW") ? value : QStringLiteral("MAKCU");
}

void ConfigManager::setInputMethod(const QString& v) {
    m_settings->setValue("Hardware/input_method", v);
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

int ConfigManager::makcuNewBaudrate() const {
    return m_settings->value("Hardware/makcu_new_baudrate", 6000000).toInt();
}

void ConfigManager::setMakcuNewBaudrate(int v) {
    m_settings->setValue("Hardware/makcu_new_baudrate", v);
    emit configChanged();
}

QString ConfigManager::makcuNewPort() const {
    return m_settings->value("Hardware/makcu_new_port", "COM0").toString();
}

void ConfigManager::setMakcuNewPort(const QString& v) {
    m_settings->setValue("Hardware/makcu_new_port", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetIp() const {
    return m_settings->value("Hardware/kmbox_net_ip", "192.168.2.88").toString();
}

void ConfigManager::setKmboxNetIp(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_ip", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetPort() const {
    return m_settings->value("Hardware/kmbox_net_port", "6234").toString();
}

void ConfigManager::setKmboxNetPort(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_port", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetUuid() const {
    return m_settings->value("Hardware/kmbox_net_uuid", "12345").toString();
}

void ConfigManager::setKmboxNetUuid(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_uuid", v);
    emit configChanged();
}

// --- AI ---

// ★ 2026-09-17: DirectML 后端已整条移除, TensorRT 是唯一后端。
//   backend() 现在恒返回 "TRT"; setBackend / dmlDeviceId / setDmlDeviceId 已删除
//   (原来是"可选后端"这套 UI 的读写口)。老 QSettings 里的 AI/backend 与
//   AI/dml_device_id 键不会再被读取, 也不会再被写回。
QString ConfigManager::backend() const {
    return QStringLiteral("TRT");
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

// ★ 2026-09-17: 固定为 kFixedMaxDetections (=20), 不再从 QSettings 读用户值。
//   setter 保留只为兼容既有调用点(它现在只写常量), 不再有任何 UI 绑定到它。
int ConfigManager::maxDetections() const {
    return kFixedMaxDetections;
}

void ConfigManager::setMaxDetections(int v) {
    Q_UNUSED(v);
    m_settings->setValue("AI/max_detections", kFixedMaxDetections);
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

QString ConfigManager::screenshotButton() const {
    return m_settings->value("Debug/screenshot_button", QStringLiteral("None")).toString();
}

void ConfigManager::setScreenshotButton(const QString& v) {
    m_settings->setValue("Debug/screenshot_button", v.isEmpty() ? QStringLiteral("None") : v);
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

// 【2026-09-13 删除】crosshairSmooth() / setCrosshairSmooth() —— 准星平滑已移除,
// 平滑统一由 PID 之前的 anchor_filter 负责。见 Apotheosis/config/config.h。

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
    // ★ 2026-09-17: 这里原来还写 trigger_* / aim_path_* 共 18 个键。
    //   它们对应的 HotkeyProfile 字段已随瞄准控制链删除, 于是这些 QSettings 键
    //   退化成【写完就没人读、且没有任何运行时消费者】的死数据(在
    //   ConfigManager::HotkeyData 里绕一圈又回到 QSettings)。已整段删除。
    m_settings->setValue(prefix + "aim_classes",         data.aimClasses);
    m_settings->setValue(prefix + "crosshair_detect_enabled", data.crosshairDetectEnabled);
    m_settings->setValue(prefix + "dynamic_fov_enabled", data.dynamicFovEnabled);
    m_settings->setValue(prefix + "dynamic_fov_strength", static_cast<double>(data.dynamicFovStrength));
    // ── ★★ 通用控制器层 (2026-09-17 第三轮续) ──────────────────────────
    // ★ 23 个键逐个写出。它们与 HotkeyProfile 的 ctl_* 一一对应,
    //   由 config_bridge 双向同步 —— 漏一个的表现是"界面能改、重启就没了"。
    m_settings->setValue(prefix + "ctl_enabled", data.ctlEnabled);
    m_settings->setValue(prefix + "ctl_kp_x", static_cast<double>(data.ctlKpX));
    m_settings->setValue(prefix + "ctl_kp_y", static_cast<double>(data.ctlKpY));
    m_settings->setValue(prefix + "ctl_ki_x", static_cast<double>(data.ctlKiX));
    m_settings->setValue(prefix + "ctl_ki_y", static_cast<double>(data.ctlKiY));
    m_settings->setValue(prefix + "ctl_kd_x", static_cast<double>(data.ctlKdX));
    m_settings->setValue(prefix + "ctl_kd_y", static_cast<double>(data.ctlKdY));
    m_settings->setValue(prefix + "ctl_tau_unwind_sec", static_cast<double>(data.ctlTauUnwindSec));
    m_settings->setValue(prefix + "ctl_tau_deriv_sec", static_cast<double>(data.ctlTauDerivSec));
    m_settings->setValue(prefix + "ctl_i_max", static_cast<double>(data.ctlIMax));
    m_settings->setValue(prefix + "ctl_max_output_counts", data.ctlMaxOutputCounts);
    m_settings->setValue(prefix + "ctl_p_full_scale_px", static_cast<double>(data.ctlPFullScalePx));
    m_settings->setValue(prefix + "ctl_y_offset", static_cast<double>(data.ctlYOffset));
    m_settings->setValue(prefix + "ctl_y_offset_max", static_cast<double>(data.ctlYOffsetMax));
    m_settings->setValue(prefix + "ctl_hysteresis_ratio", static_cast<double>(data.ctlHysteresisRatio));
    m_settings->setValue(prefix + "ctl_max_distance_px", static_cast<double>(data.ctlMaxDistancePx));
    m_settings->setValue(prefix + "ctl_random_seed", data.ctlRandomSeed);
    m_settings->setValue(prefix + "ctl_match_center_ratio", static_cast<double>(data.ctlMatchCenterRatio));
    m_settings->setValue(prefix + "ctl_area_ratio_tol", static_cast<double>(data.ctlAreaRatioTol));
    m_settings->setValue(prefix + "ctl_k_snap_mult", static_cast<double>(data.ctlKSnapMult));
    m_settings->setValue(prefix + "ctl_min_aspect", static_cast<double>(data.ctlMinAspect));
    m_settings->setValue(prefix + "ctl_max_aspect", static_cast<double>(data.ctlMaxAspect));
}

ConfigManager::HotkeyData ConfigManager::readHotkeyFromSettings(int index) const {
    HotkeyData data;
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);

    data.name = m_settings->value(prefix + "name", "Aim").toString();
    data.group = m_settings->value(prefix + "group", QString::fromUtf8(u8"\xe9\xbb\x98\xe8\xae\xa4")).toString();
    data.keys = m_settings->value(prefix + "keys", "RightMouseButton").toString().split(",", Qt::SkipEmptyParts);
    data.fovX = m_settings->value(prefix + "fovX", 106).toInt();
    data.fovY = m_settings->value(prefix + "fovY", 74).toInt();
    // ★ 2026-09-17: 这里原来还读 trigger_* / aim_path_* 共 18 个键(与上面的写出对称)。
    //   对应的 HotkeyProfile 字段已随瞄准控制链删除, 这些成员已从 HotkeyData 移除。
    data.aimClasses      = m_settings->value(prefix + "aim_classes", QString()).toString();
    data.crosshairDetectEnabled = m_settings->value(prefix + "crosshair_detect_enabled", false).toBool();
    data.dynamicFovEnabled = m_settings->value(prefix + "dynamic_fov_enabled", false).toBool();
    data.dynamicFovStrength = m_settings->value(prefix + "dynamic_fov_strength", 0.60).toFloat();
    // ── ★★ 通用控制器层 (2026-09-17 第三轮续) ──────────────────────────
    // ★ 与上面的写出【严格对称】。默认值刻意与 HotkeyProfile 的成员初值一致,
    //   这样老 QSettings 里没有这些键时行为不变(等价历史单套行为)。
    data.ctlEnabled = m_settings->value(prefix + "ctl_enabled", false).toBool();
    data.ctlKpX = m_settings->value(prefix + "ctl_kp_x", 35.0).toDouble();
    data.ctlKpY = m_settings->value(prefix + "ctl_kp_y", 35.0).toDouble();
    data.ctlKiX = m_settings->value(prefix + "ctl_ki_x", 0.0).toDouble();
    data.ctlKiY = m_settings->value(prefix + "ctl_ki_y", 0.0).toDouble();
    data.ctlKdX = m_settings->value(prefix + "ctl_kd_x", 0.0).toDouble();
    data.ctlKdY = m_settings->value(prefix + "ctl_kd_y", 0.0).toDouble();
    data.ctlTauUnwindSec = m_settings->value(prefix + "ctl_tau_unwind_sec", 0.030).toDouble();
    data.ctlTauDerivSec = m_settings->value(prefix + "ctl_tau_deriv_sec", 0.020).toDouble();
    data.ctlIMax = m_settings->value(prefix + "ctl_i_max", 0.0).toDouble();
    data.ctlMaxOutputCounts = m_settings->value(prefix + "ctl_max_output_counts", 200).toInt();
    data.ctlPFullScalePx = m_settings->value(prefix + "ctl_p_full_scale_px", 0.0).toDouble();
    data.ctlYOffset = m_settings->value(prefix + "ctl_y_offset", 0.5).toDouble();
    data.ctlYOffsetMax = m_settings->value(prefix + "ctl_y_offset_max", 0.5).toDouble();
    data.ctlHysteresisRatio = m_settings->value(prefix + "ctl_hysteresis_ratio", 1.3).toDouble();
    data.ctlMaxDistancePx = m_settings->value(prefix + "ctl_max_distance_px", 0.0).toDouble();
    data.ctlRandomSeed = m_settings->value(prefix + "ctl_random_seed", 0).toInt();
    data.ctlMatchCenterRatio = m_settings->value(prefix + "ctl_match_center_ratio", 0.5).toDouble();
    data.ctlAreaRatioTol = m_settings->value(prefix + "ctl_area_ratio_tol", 2.0).toDouble();
    data.ctlKSnapMult = m_settings->value(prefix + "ctl_k_snap_mult", 1.15).toDouble();
    data.ctlMinAspect = m_settings->value(prefix + "ctl_min_aspect", 0.2).toDouble();
    data.ctlMaxAspect = m_settings->value(prefix + "ctl_max_aspect", 5.0).toDouble();

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
