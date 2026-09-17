#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QSpinBox;
class ToggleSwitch;

class SessionPage : public QWidget {
    Q_OBJECT

public:
    explicit SessionPage(QWidget* parent = nullptr);

private slots:
    // ★ 2026-09-17: onBackendChanged / onDmlDeviceChanged 已删除
    //   (DirectML 后端整条移除, TensorRT 是唯一后端)。
    void onShowWindowChanged(bool checked);
    void loadConfig();

private:
    // Backend card — 只剩只读展示 + 状态行
    QLabel* m_backendStatusLabel{};

    // Preview card
    ToggleSwitch* m_showWindow{};

    // CUDA settings card
    ToggleSwitch* m_cudaGraph{};
    ToggleSwitch* m_gpuExclusive{};
    QSpinBox* m_gpuReserve{};
    QSpinBox* m_cpuReserve{};
    QSpinBox* m_systemMemoryReserve{};
};
