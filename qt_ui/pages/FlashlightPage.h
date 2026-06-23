#pragma once

#include <QWidget>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class ToggleSwitch;

class FlashlightPage : public QWidget {
    Q_OBJECT

public:
    explicit FlashlightPage(QWidget* parent = nullptr);

private:
    void loadConfig();
    void rebuildClassCombo();

    ToggleSwitch*   m_showPreview{};
    QSpinBox*       m_threshold{};
    QSlider*        m_thresholdSlider{};
    QSpinBox*       m_minRadius{};
    QSlider*        m_minRadiusSlider{};
    QSpinBox*       m_maxRadius{};
    QSlider*        m_maxRadiusSlider{};
    QDoubleSpinBox* m_circularity{};
    QSlider*        m_circularitySlider{};
    QSpinBox*       m_openRadius{};
    QSlider*        m_openRadiusSlider{};
    QSpinBox*       m_localContrast{};
    QSlider*        m_localContrastSlider{};
    QComboBox*      m_classCombo{};
};
