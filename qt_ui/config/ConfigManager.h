#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QStringList>

class ConfigManager : public QObject {
    Q_OBJECT

public:
    static ConfigManager& instance();

    bool load(const QString& path = "config.ini");
    bool save();
    QString configPath() const;

    // Capture
    QString captureMethod() const;
    void setCaptureMethod(const QString& v);
    QString udpIp() const;
    void setUdpIp(const QString& v);
    int udpPort() const;
    void setUdpPort(int v);
    QString tcpIp() const;
    void setTcpIp(const QString& v);
    int tcpPort() const;
    void setTcpPort(int v);
    QString ethAdapter() const;
    void setEthAdapter(const QString& v);
    int ethEthertype() const;
    void setEthEthertype(int v);
    int opencvCaptureIndex() const;
    void setOpencvCaptureIndex(int v);
    QString opencvCaptureApi() const;
    void setOpencvCaptureApi(const QString& v);
    QString opencvCaptureUrl() const;
    void setOpencvCaptureUrl(const QString& v);
    int opencvCaptureWidth() const;
    void setOpencvCaptureWidth(int v);
    int opencvCaptureHeight() const;
    void setOpencvCaptureHeight(int v);
    int opencvCaptureFps() const;
    void setOpencvCaptureFps(int v);
    bool captureMfGpu() const;
    void setCaptureMfGpu(bool v);
    int captureCrop() const;
    void setCaptureCrop(int v);
    QString captureFormat() const;
    void setCaptureFormat(const QString& v);
    int detectionResolution() const;
    void setDetectionResolution(int v);
    int captureFps() const;
    void setCaptureFps(int v);
    bool circleMask() const;
    void setCircleMask(bool v);

    // Capture card
    int captureCardIndex() const;
    void setCaptureCardIndex(int v);
    int captureCardWidth() const;
    void setCaptureCardWidth(int v);
    int captureCardHeight() const;
    void setCaptureCardHeight(int v);
    int captureCardFps() const;
    void setCaptureCardFps(int v);
    QString captureCardFormat() const;
    void setCaptureCardFormat(const QString& v);
    int captureCardCropWidth() const;
    void setCaptureCardCropWidth(int v);
    int captureCardCropHeight() const;
    void setCaptureCardCropHeight(int v);

    // Hardware
    QString inputMethod() const;
    void setInputMethod(const QString& v);
    int arduinoBaudrate() const;
    void setArduinoBaudrate(int v);
    QString arduinoPort() const;
    void setArduinoPort(const QString& v);
    bool arduino16BitMouse() const;
    void setArduino16BitMouse(bool v);
    bool arduinoEnableKeys() const;
    void setArduinoEnableKeys(bool v);
    QString kmboxNetIp() const;
    void setKmboxNetIp(const QString& v);
    QString kmboxNetPort() const;
    void setKmboxNetPort(const QString& v);
    QString kmboxNetUuid() const;
    void setKmboxNetUuid(const QString& v);
    QString kmboxAPidvid() const;
    void setKmboxAPidvid(const QString& v);
    int makcuBaudrate() const;
    void setMakcuBaudrate(int v);
    QString makcuPort() const;
    void setMakcuPort(const QString& v);
    // AI
    QString backend() const;
    void setBackend(const QString& v);
    int dmlDeviceId() const;
    void setDmlDeviceId(int v);
    QString aiModel() const;
    void setAiModel(const QString& v);
    float confidenceThreshold() const;
    void setConfidenceThreshold(float v);
    float nmsThreshold() const;
    void setNmsThreshold(float v);
    int maxDetections() const;
    void setMaxDetections(int v);
    bool exportEnableFp8() const;
    void setExportEnableFp8(bool v);
    bool exportEnableFp16() const;
    void setExportEnableFp16(bool v);
    bool smallTargetEnabled() const;
    void setSmallTargetEnabled(bool v);
    float smallTargetConfidence() const;
    void setSmallTargetConfidence(float v);
    float smallTargetAreaFrac() const;
    void setSmallTargetAreaFrac(float v);

    // Depth
    bool depthInferenceEnabled() const;
    void setDepthInferenceEnabled(bool v);
    QString depthModelPath() const;
    void setDepthModelPath(const QString& v);
    int depthMaskFps() const;
    void setDepthMaskFps(int v);
    int depthOptInputSize() const;
    void setDepthOptInputSize(int v);
    float depthNormClipLowPct() const;
    void setDepthNormClipLowPct(float v);
    float depthNormClipHighPct() const;
    void setDepthNormClipHighPct(float v);

    // Overlay
    int overlayOpacity() const;
    void setOverlayOpacity(int v);
    float overlayUiScale() const;
    void setOverlayUiScale(float v);

    // Macro
    bool macroEnabled() const;
    void setMacroEnabled(bool v);
    QString macroScriptPath() const;
    void setMacroScriptPath(const QString& v);
    bool macroPrimaryButtonEvents() const;
    void setMacroPrimaryButtonEvents(bool v);

    // Crosshair
    int crosshairRectW() const;
    void setCrosshairRectW(int v);
    int crosshairRectH() const;
    void setCrosshairRectH(int v);
    int crosshairMinPixelCount() const;
    void setCrosshairMinPixelCount(int v);
    int crosshairCloseRadius() const;
    void setCrosshairCloseRadius(int v);
    float crosshairSmooth() const;
    void setCrosshairSmooth(float v);

    // Crosshair color profiles
    struct ColorProfile {
        QString name;
        bool enabled = true;
        int hLow = 0, hHigh = 10;
        int sMin = 120, sMax = 255;
        int vMin = 120, vMax = 255;
    };
    QList<ColorProfile> crosshairColors() const;
    void setCrosshairColors(const QList<ColorProfile>& colors);

    // Laser
    int laserRectW() const;
    void setLaserRectW(int v);
    int laserRectH() const;
    void setLaserRectH(int v);
    int laserCenterX() const;
    void setLaserCenterX(int v);
    int laserCenterY() const;
    void setLaserCenterY(int v);
    int laserTargetCenterX() const;
    void setLaserTargetCenterX(int v);
    int laserTargetCenterY() const;
    void setLaserTargetCenterY(int v);
    int laserTargetRectW() const;
    void setLaserTargetRectW(int v);
    int laserTargetRectH() const;
    void setLaserTargetRectH(int v);
    float laserMinElongation() const;
    void setLaserMinElongation(float v);
    int laserMinPixelCount() const;
    void setLaserMinPixelCount(int v);
    int laserCloseRadius() const;
    void setLaserCloseRadius(int v);
    float laserSmooth() const;
    void setLaserSmooth(float v);

    // Laser color profiles
    QList<ColorProfile> laserColors() const;
    void setLaserColors(const QList<ColorProfile>& colors);

    // Flashlight halo (寻光). Three macro knobs (0..100); all internals derived
    // by crosshair::flashlight_derive_tuning().
    bool flashlightShowPreview() const;
    void setFlashlightShowPreview(bool v);
    int flashlightSensitivity() const;       // 灵敏度
    void setFlashlightSensitivity(int v);
    int flashlightRejectStrength() const;    // 抗误锁
    void setFlashlightRejectStrength(int v);
    int flashlightSpotSize() const;          // 光斑大小
    void setFlashlightSpotSize(int v);

    // Glass filter (玻璃后目标抑制)
    bool glassFilterShowPreview() const;
    void setGlassFilterShowPreview(bool v);
    int glassFilterStrength() const;        // 过滤强度 0..100 (replaces ring/coverage/min-box)
    void setGlassFilterStrength(int v);

    // Glass film color profiles (reuses ColorProfile struct)
    QList<ColorProfile> glassColors() const;
    void setGlassColors(const QList<ColorProfile>& colors);

    // Debug
    bool showFps() const;
    void setShowFps(bool v);
    bool verbose() const;
    void setVerbose(bool v);
    int screenshotDelay() const;
    void setScreenshotDelay(int v);
    bool showWindow() const;
    void setShowWindow(bool v);
    bool replayRecordEnabled() const;
    void setReplayRecordEnabled(bool v);
    int replaySeconds() const;
    void setReplaySeconds(int v);
    float replayPlaybackSpeed() const;
    void setReplayPlaybackSpeed(float v);

    // Active hotkey group
    QString activeHotkeyGroup() const;
    void setActiveHotkeyGroup(const QString& v);

    // Hotkey profiles
    int hotkeyCount() const;

    struct HotkeyData {
        QString name;
        QString group;
        QStringList keys;
        int fovX = 106, fovY = 74;
        float speedX = 0.6f;
        float speedY = 0.6f;
        float deadZonePx = 2.0f;
        bool smartTriggerEnabled = false;
        float smartTriggerHitScale = 0.60f;
        float smartTriggerAggression = 0.50f;
        bool laserDetectEnabled = false;
        bool crosshairDetectEnabled = false;
        bool flashlightDetectEnabled = false;
        bool glassFilterEnabled = false;
        float lockAggression = 0.30f;
        bool yOffsetSizeDecayEnabled = false;
        bool dynamicFovEnabled = false;
        float dynamicFovStrength = 0.60f;
        // Aim trajectory curve. 0=Linear, 1=Bezier, 2=Custom.
        int   aimPathMode = 0;
        float aimPathBezierCx1 = 0.30f;
        float aimPathBezierCy1 = 0.00f;
        float aimPathBezierCx2 = 0.70f;
        float aimPathBezierCy2 = 0.00f;
        // Comma-separated 32 floats. Empty = all zeros (straight line).
        QString aimPathCustomSamples;
    };

    HotkeyData hotkey(int index) const;
    void setHotkey(int index, const HotkeyData& data);
    void addHotkey(const HotkeyData& data);
    void removeHotkey(int index);

signals:
    void configChanged();
    void configLoaded();

private:
    ConfigManager();
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void writeHotkeyToSettings(int index, const HotkeyData& data);
    HotkeyData readHotkeyFromSettings(int index) const;
    void reindexHotkeys();

    QSettings* m_settings = nullptr;
    QString m_path;
};
