#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QSpinBox;
class QPushButton;
class ToggleSwitch;

class SessionPage : public QWidget {
    Q_OBJECT

public:
    explicit SessionPage(QWidget* parent = nullptr);

private slots:
    void onBackendChanged(int index);
    void onToggleInference();

private:
    QComboBox* m_backendCombo{};
    QSpinBox* m_dmlDeviceId{};
    QLabel* m_dmlDeviceLabel{};
    QWidget* m_dmlDeviceRow{};
    QLabel* m_modelFileLabel{};

    QPushButton* m_toggleBtn{};
    QLabel* m_statusLabel{};
    bool m_running = false;

    ToggleSwitch* m_cudaGraph{};
    ToggleSwitch* m_dualBuffer{};
    ToggleSwitch* m_pinnedMemory{};
    QSpinBox* m_gpuReserve{};
    QSpinBox* m_cpuReserve{};
};
