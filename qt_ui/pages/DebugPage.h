#pragma once

#include <QWidget>

class QLineEdit;
class QSpinBox;
class QSlider;
class QDoubleSpinBox;
class ToggleSwitch;

class DebugPage : public QWidget {
    Q_OBJECT

public:
    explicit DebugPage(QWidget* parent = nullptr);

private:
    QLineEdit* m_screenshotKey{};
    QSlider* m_screenshotDelaySlider{};
    QSpinBox* m_screenshotDelay{};

    ToggleSwitch* m_enableRecording{};
    QSlider* m_replayDurationSlider{};
    QSpinBox* m_replayDuration{};
    QSlider* m_replaySpeedSlider{};
    QDoubleSpinBox* m_replaySpeed{};

    ToggleSwitch* m_verboseLog{};
    ToggleSwitch* m_showFps{};
    ToggleSwitch* m_showWindow{};
};
