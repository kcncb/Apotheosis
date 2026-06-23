#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class ToggleSwitch;

class HardwarePage : public QWidget {
    Q_OBJECT

public:
    explicit HardwarePage(QWidget* parent = nullptr);

private slots:
    void onInputMethodChanged(int index);
    void refreshStatus();
    void reconnectDevice();

private:
    void loadFieldsFromConfig();

    QComboBox* m_inputMethodCombo{};
    QStackedWidget* m_deviceStack{};

    // Status
    QLabel* m_statusDot{};
    QLabel* m_statusText{};
    QPushButton* m_connectBtn{};
    QTimer* m_statusTimer{};

    // Arduino
    QLineEdit* m_arduinoPort{};
    QSpinBox* m_arduinoBaud{};
    ToggleSwitch* m_arduino16bit{};
    ToggleSwitch* m_arduinoKeys{};

    // Kmbox Net
    QLineEdit* m_kmnetIp{};
    QLineEdit* m_kmnetPort{};
    QLineEdit* m_kmnetUuid{};

    // Kmbox A
    QLineEdit* m_kmaPidvid{};

    // MAKCU
    QLineEdit* m_makcuPort{};
    QSpinBox* m_makcuBaud{};
};
