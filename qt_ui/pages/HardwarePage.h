#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
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
    // 切换全局配置方案后按新值重读一遍控件。
    void loadFieldsFromConfig();

private:
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

    // 【2026-09-13 删除】m_captureAgeOffset (采集回调前帧龄估计) 与
    // m_crosshairSmooth (准星找色平滑强度) 两个控件, 连同它们的界面卡片一起移除。
};
