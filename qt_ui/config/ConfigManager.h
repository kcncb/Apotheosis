#pragma once

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

    // Depth
    bool depthInferenceEnabled() const;
    void setDepthInferenceEnabled(bool v);
    QString depthModelPath() const;
    void setDepthModelPath(const QString& v);
    int depthFps() const;
    void setDepthFps(int v);
    bool depthMaskEnabled() const;
    void setDepthMaskEnabled(bool v);
    int depthMaskNearPercent() const;
    void setDepthMaskNearPercent(int v);
    int depthMaskAlpha() const;
    void setDepthMaskAlpha(int v);

    // Overlay
    int overlayOpacity() const;
    void setOverlayOpacity(int v);
    float overlayUiScale() const;
    void setOverlayUiScale(float v);

    // Debug
    bool showFps() const;
    void setShowFps(bool v);
    bool verbose() const;
    void setVerbose(bool v);

    // Hotkey profiles
    int hotkeyCount() const;

    struct HotkeyData {
        QString name;
        QStringList keys;
        int fovX = 106, fovY = 74;
        float speedX = 0.6f, speedY = 0.6f;
        float lockStrength = 0.0f, lockRadiusPx = 25.0f;
        int aimTrajectoryMode = 0;
        float bezierCx1 = 0.30f, bezierCy1 = 0.25f;
        float bezierCx2 = 0.70f, bezierCy2 = -0.15f;
        bool kalmanEnabled = true;
        float kalmanProcessNoisePos = 40.0f;
        float kalmanProcessNoiseVel = 1800.0f;
        float kalmanMeasurementNoise = 35.0f;
        bool smartTriggerEnabled = false;
        bool crosshairDetectEnabled = false;
        bool dynamicFovEnabled = false;
        float dynamicFovMarginFrac = 1.10f;
        float dynamicFovMinRadiusFrac = 0.20f;
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
