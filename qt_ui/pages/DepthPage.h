#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLineEdit;
class QSlider;
class QSpinBox;
class ToggleSwitch;

class DepthPage : public QWidget {
    Q_OBJECT

public:
    explicit DepthPage(QWidget* parent = nullptr);

private:
    void browseModel();

    // Depth inference
    ToggleSwitch* m_depthEnabled{};
    QLineEdit* m_modelPath{};
    QSpinBox* m_depthFps{};

    // Depth mask
    ToggleSwitch* m_maskEnabled{};
    QSpinBox* m_maskFps{};
    QSlider* m_nearSlider{};
    QSpinBox* m_nearSpin{};
    QSlider* m_expandSlider{};
    QSpinBox* m_expandSpin{};
    QSpinBox* m_holdFrames{};
    QSlider* m_alphaSlider{};
    QSpinBox* m_alphaSpin{};
    ToggleSwitch* m_invertMask{};
    QSlider* m_suppressSlider{};
    QDoubleSpinBox* m_suppressSpin{};
};
