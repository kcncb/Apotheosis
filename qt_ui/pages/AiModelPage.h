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

private slots:
    // 切换全局配置方案后按新值重读一遍控件 (信号全部屏蔽, 不回写)。
    void reloadFromConfig();

private:
    // ★ 2026-09-17: onBackendChanged 已删除 —— DirectML 后端整条移除后
    //   TensorRT 是唯一后端, 不存在"切换后端"这个动作。
    void browseModel();
    void onSmallTargetToggled(bool enabled);
    void updateModelInfo();
    void updateBackendStatus();

    // Model card
    QComboBox* m_modelCombo{};
    QLineEdit* m_modelPath{};
    QLabel* m_fixedInputLabel{};
    // m_backendCombo / m_dmlDeviceId / m_dmlRow 已随 DirectML 后端删除。
    QLabel* m_backendStatusLabel{};

    // Detection card
    QSlider* m_confSlider{};
    QDoubleSpinBox* m_confSpin{};
    QSlider* m_nmsSlider{};
    QDoubleSpinBox* m_nmsSpin{};
    // m_maxDetections 已删除: 上限固定为 kFixedMaxDetections, 界面只读展示。

    // Small target card
    ToggleSwitch* m_smallTargetEnabled{};
    QSlider* m_smallTargetConfSlider{};
    QDoubleSpinBox* m_smallTargetConfSpin{};
    QSlider* m_smallTargetAreaSlider{};
    QDoubleSpinBox* m_smallTargetAreaSpin{};
    QWidget* m_smallTargetConfRow{};
    QWidget* m_smallTargetAreaRow{};

};
