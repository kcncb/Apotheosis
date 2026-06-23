#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class ToggleSwitch;

class AiModelPage : public QWidget {
    Q_OBJECT

public:
    explicit AiModelPage(QWidget* parent = nullptr);

private:
    void onBackendChanged(int index);
    void browseModel();
    void onSmallTargetToggled(bool enabled);

    // Model card
    QComboBox* m_modelCombo{};
    QLineEdit* m_modelPath{};
    QLabel* m_fixedInputLabel{};
    QComboBox* m_backendCombo{};
    QSpinBox* m_dmlDeviceId{};
    QWidget* m_dmlRow{};
    QLabel* m_backendStatusLabel{};

    // Detection card
    QSlider* m_confSlider{};
    QDoubleSpinBox* m_confSpin{};
    QSlider* m_nmsSlider{};
    QDoubleSpinBox* m_nmsSpin{};
    QSpinBox* m_maxDetections{};

    // Small target card
    ToggleSwitch* m_smallTargetEnabled{};
    QSlider* m_smallTargetConfSlider{};
    QDoubleSpinBox* m_smallTargetConfSpin{};
    QSlider* m_smallTargetAreaSlider{};
    QDoubleSpinBox* m_smallTargetAreaSpin{};
    QWidget* m_smallTargetConfRow{};
    QWidget* m_smallTargetAreaRow{};

    // Export card
    ToggleSwitch* m_fp16{};
    ToggleSwitch* m_fp8{};
};
