#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
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

    QLineEdit* m_modelPath{};
    QComboBox* m_backendCombo{};
    QSpinBox* m_dmlDeviceId{};
    QWidget* m_dmlRow{};

    QSlider* m_confSlider{};
    QDoubleSpinBox* m_confSpin{};
    QSlider* m_nmsSlider{};
    QDoubleSpinBox* m_nmsSpin{};
    QSpinBox* m_maxDetections{};

    ToggleSwitch* m_fp16{};
    ToggleSwitch* m_fp8{};
    ToggleSwitch* m_fixedInput{};
};
