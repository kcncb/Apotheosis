#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSlider;
class QSpinBox;
class ToggleSwitch;

class DepthPage : public QWidget {
    Q_OBJECT

public:
    explicit DepthPage(QWidget* parent = nullptr);

private slots:
    void onLoadConfig();

private:
    void browseModel();

    // ── Depth inference ──
    ToggleSwitch* m_depthEnabled{};
    QLineEdit* m_modelPath{};
    QSpinBox* m_depthFps{};

    // ── Depth runtime ──
    QSpinBox* m_maskFpsRuntime{};
    QSpinBox* m_optInputSize{};
    QSlider* m_normClipLowSlider{};
    QDoubleSpinBox* m_normClipLowSpin{};
    QSlider* m_normClipHighSlider{};
    QDoubleSpinBox* m_normClipHighSpin{};

    // ── Depth mask ──
    ToggleSwitch* m_maskEnabled{};
    QSlider* m_nearSlider{};
    QSpinBox* m_nearSpin{};
    QSlider* m_expandSlider{};
    QSpinBox* m_expandSpin{};
    QSpinBox* m_holdFrames{};
    QSlider* m_suppressSlider{};
    QDoubleSpinBox* m_suppressSpin{};
    QSlider* m_alphaSlider{};
    QSpinBox* m_alphaSpin{};
    ToggleSwitch* m_invertMask{};
    QComboBox* m_colormapCombo{};

    // ── Depth visualization ──
    ToggleSwitch* m_showHeatmap{};
    QSlider* m_heatmapGammaSlider{};
    QDoubleSpinBox* m_heatmapGammaSpin{};
    ToggleSwitch* m_showBboxDistance{};
};
