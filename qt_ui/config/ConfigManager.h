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
    int makcuBaudrate() const;
    void setMakcuBaudrate(int v);
    QString makcuPort() const;
    void setMakcuPort(const QString& v);
    int makcuNewBaudrate() const;
    void setMakcuNewBaudrate(int v);
    QString makcuNewPort() const;
    void setMakcuNewPort(const QString& v);
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


    // Debug
    bool showFps() const;
    void setShowFps(bool v);
    bool verbose() const;
    void setVerbose(bool v);
    int screenshotDelay() const;
    void setScreenshotDelay(int v);
    QString screenshotButton() const;
    void setScreenshotButton(const QString& v);
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
        int lostTargetCacheFrames = 5;
        bool triggerEnabled = false;
        int triggerFireDelay = 0;
        int triggerFireDuration = 100;
        int triggerFireInterval = 200;
        int triggerYPercent = 100;
        int triggerDelayJitterMs    = 0;
        int triggerDurationJitterMs = 0;
        int triggerIntervalJitterMs = 0;
        int triggerSwitchCooldownMs = 0;
        // 优先级排序的类别列表, 每条 "id:y_min:y_max:min_conf", 分号分隔。
        QString aimClasses;
        bool crosshairDetectEnabled = false;
        bool dynamicFovEnabled = false;
        float dynamicFovStrength = 0.60f;
        // Aim trajectory curve. 0=Linear, 1=Bezier, 2=Custom.
        int   aimPathMode = 0;
        float aimPathBezierCx1 = 0.30f;
        float aimPathBezierCy1 = 0.00f;
        float aimPathBezierCx2 = 0.70f;
        float aimPathBezierCy2 = 0.00f;
        // Comma-separated high-resolution floats. Empty = straight line.
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
