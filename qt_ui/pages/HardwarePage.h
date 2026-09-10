#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

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
    QLabel* m_statusDot{};
    QLabel* m_statusText{};
    QPushButton* m_connectBtn{};
    QTimer* m_statusTimer{};

    QLineEdit* m_makcuPort{};
    QSpinBox* m_makcuBaud{};
    QLineEdit* m_makcuNewPort{};
    QSpinBox* m_makcuNewBaud{};
};
