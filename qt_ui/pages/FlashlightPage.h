#pragma once

#include <QWidget>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class ToggleSwitch;

class FlashlightPage : public QWidget {
    Q_OBJECT

public:
    explicit FlashlightPage(QWidget* parent = nullptr);

private:
    void loadConfig();

    ToggleSwitch* m_showPreview{};
    QSpinBox*     m_sensitivity{};        // 检测倾向
    QSlider*      m_sensitivitySlider{};
    QSpinBox*     m_reject{};             // 抗误锁
    QSlider*      m_rejectSlider{};
    QSpinBox*     m_spotSize{};           // 光斑大小
    QSlider*      m_spotSizeSlider{};
};
