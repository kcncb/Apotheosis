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

    // ── Capture: 只有「采集卡」一种方式 ──
    // 所有参数都来自设备真实能力探测, UI 用 格式/分辨率/帧率 三级联动下拉让
    // 用户从中选。这里只是 Qt 侧的内存缓存, 真正落盘由 Config::saveConfig() 完成。
    QString captureDevice() const;      // 设备 friendly name (不是 index)
    void setCaptureDevice(const QString& v);
    QString captureFormat() const;      // NV12 | MJPG | YUY2 | RGB32
    void setCaptureFormat(const QString& v);
    int captureWidth() const;
    void setCaptureWidth(int v);
    int captureHeight() const;
    void setCaptureHeight(int v);
    int captureFps() const;
    void setCaptureFps(int v);
    bool captureGpuDecode() const;
    void setCaptureGpuDecode(bool v);
    int detectionResolution() const;
    void setDetectionResolution(int v);
    bool circleMask() const;
    void setCircleMask(bool v);
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
        // 0 = 长按模式(按住不松手, 准星离开命中区才松开); >0 = 连点模式。
        int triggerFireDuration = 0;
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
