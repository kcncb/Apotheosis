#pragma once

#include <QWidget>

class QComboBox;
class QSpinBox;
class QLineEdit;
class QStackedWidget;
class ToggleSwitch;

class CapturePage : public QWidget {
    Q_OBJECT

public:
    explicit CapturePage(QWidget* parent = nullptr);

private:
    void onMethodChanged(int index);

    QComboBox* m_methodCombo{};
    QStackedWidget* m_methodStack{};

    // UDP
    QLineEdit* m_udpIp{};
    QSpinBox* m_udpPort{};

    // Capture card
    QSpinBox* m_cardIndex{};
    QSpinBox* m_cardWidth{};
    QSpinBox* m_cardHeight{};
    QSpinBox* m_cardFps{};
    QComboBox* m_cardFormat{};
    QSpinBox* m_cropWidth{};
    QSpinBox* m_cropHeight{};

    // Detection
    QComboBox* m_detResolution{};
    QSpinBox* m_captureFps{};
    ToggleSwitch* m_circleMask{};
};
