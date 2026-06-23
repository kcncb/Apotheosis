#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSlider;
class QDoubleSpinBox;
class QVBoxLayout;
class ToggleSwitch;

class DebugPage : public QWidget {
    Q_OBJECT

public:
    explicit DebugPage(QWidget* parent = nullptr);

    // Live telemetry feed (called from MainWindow's monitor poll timer).
    void setFovReadout(const QString& text);

private slots:
    void onLoadConfig();

private:
    void buildScreenshotCard(QVBoxLayout* layout);
    void buildReplayCard(QVBoxLayout* layout);
    void buildDiagCard(QVBoxLayout* layout);
    void buildDynamicFovCard(QVBoxLayout* layout);

    // ── Screenshot ──
    QLineEdit* m_screenshotKey{};
    QSlider* m_screenshotDelaySlider{};
    QSpinBox* m_screenshotDelay{};

    // ── Replay ──
    ToggleSwitch* m_enableRecording{};
    QSlider* m_replayDurationSlider{};
    QSpinBox* m_replayDuration{};
    QSlider* m_replaySpeedSlider{};
    QDoubleSpinBox* m_replaySpeed{};

    // ── Diagnostics ──
    ToggleSwitch* m_verboseLog{};
    ToggleSwitch* m_showFps{};
    ToggleSwitch* m_showWindow{};

    // ── Dynamic FOV (read-only) ──
    QLabel* m_fovReadout{};
};
