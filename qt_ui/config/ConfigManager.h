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

    // 生效配置被整体替换 (切换/新建配置方案) 之后调用: 让所有连了
    // configLoaded 的页面把控件按新值重读一遍。
    // 只发信号, 不改任何值 —— 值由 ConfigBridge::syncFromRuntime() 负责。
    void notifyRuntimeReloaded();

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
    // KMBox Net (以太网 UDP, 2026-09-15 恢复)
    QString kmboxNetIp() const;
    void setKmboxNetIp(const QString& v);
    QString kmboxNetPort() const;
    void setKmboxNetPort(const QString& v);
    QString kmboxNetUuid() const;
    void setKmboxNetUuid(const QString& v);
    // AI
    // ★ 2026-09-17: DirectML 后端已整条移除 → setBackend / dmlDeviceId /
    //   setDmlDeviceId 三个读写口删除; backend() 恒返回 "TRT"。
    QString backend() const;
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
    // 【2026-09-13 删除】crosshairSmooth() / setCrosshairSmooth() —— 准星平滑已移除。

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
        // ★ 2026-09-17: 原来这里有 ~20 个瞄准链成员(trigger_* 12 个 / aim_path_* 8 个)。
        //   它们只在这个结构体和 QSettings 之间往返, 没有任何运行时消费者, 所以随
        //   瞄准控制链一起删除。删掉它们也意味着 QSettings 里那些键不再被读写 ——
        //   用户旧的 QSettings 里残留的值会被忽略, 不影响任何活着的键。
        // ── 下面这些仍然活着(检测 / 瞄点选择 / 准星找色 在用) ──
        // 优先级排序的类别列表, 每条 "id:y_min:y_max:min_conf", 分号分隔。
        QString aimClasses;
        bool crosshairDetectEnabled = false;
        bool dynamicFovEnabled = false;
        float dynamicFovStrength = 0.60f;

        // ── ★★ 通用控制器层 (2026-09-17 第三轮续) ──────────────────────
        // 与 HotkeyProfile 的 ctl_* 一一对应，由 config_bridge 双向同步。
        // ★★ 必须逐个列出: 这个结构是界面侧的落盘载体，`aim_classes` 当初
        //    就是因为"只在 HotkeyProfile 里有、这里没有"而断过线。
        // ★ 默认值与 HotkeyProfile 的成员初值一致（等价历史单行为）。
        bool   ctlEnabled = false;
        double ctlKpX = 35.0, ctlKpY = 35.0;
        double ctlKiX = 0.0,  ctlKiY = 0.0;
        double ctlKdX = 0.0,  ctlKdY = 0.0;
        double ctlTauUnwindSec = 0.030;
        double ctlTauDerivSec = 0.020;
        double ctlIMax = 0.0;
        int    ctlMaxOutputCounts = 200;
        double ctlPFullScalePx = 0.0;
        double ctlYOffset = 0.5;
        double ctlYOffsetMax = 0.5;
        double ctlHysteresisRatio = 1.3;
        double ctlMaxDistancePx = 0.0;
        int    ctlRandomSeed = 0;
        double ctlMatchCenterRatio = 0.5;
        double ctlAreaRatioTol = 2.0;
        double ctlKSnapMult = 1.15;
        double ctlMinAspect = 0.2;
        double ctlMaxAspect = 5.0;
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
