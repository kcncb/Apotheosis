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
    void onDmlDeviceChanged(int value);
    void onToggleInference();
    void onShowWindowChanged(bool checked);
    void loadConfig();

private:
    // Backend card
    QComboBox* m_backendCombo{};
    QSpinBox* m_dmlDeviceId{};
    QWidget* m_dmlDeviceRow{};
    QLabel* m_backendStatusLabel{};

    // Session control card
    QPushButton* m_toggleBtn{};
    QLabel* m_statusLabel{};
    bool m_running = false;

    // Preview card
    ToggleSwitch* m_showWindow{};

    // CUDA settings card
    ToggleSwitch* m_cudaGraph{};
    ToggleSwitch* m_dualBuffer{};
    ToggleSwitch* m_pinnedMemory{};
    QSpinBox* m_gpuReserve{};
    QSpinBox* m_cpuReserve{};
};
