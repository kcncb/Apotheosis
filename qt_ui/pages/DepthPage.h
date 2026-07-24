#pragma once

#include <QWidget>

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class ToggleSwitch;

class DepthPage : public QWidget {
    Q_OBJECT

public:
    explicit DepthPage(QWidget* parent = nullptr);

private slots:
    void onLoadConfig();

private:
    void browseModel();

    // ── Depth inference (only powers the 寻光/flashlight depth gate) ──
    ToggleSwitch* m_depthEnabled{};
    QLineEdit* m_modelPath{};
    QSpinBox* m_optInputSize{};
    QSpinBox* m_maskFpsRuntime{};
    QDoubleSpinBox* m_normLowPct{};
    QDoubleSpinBox* m_normHighPct{};
};
