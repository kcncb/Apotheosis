#pragma once

#include <QWidget>

class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class ToggleSwitch;
class CardWidget;

class CapturePage : public QWidget {
    Q_OBJECT

public:
    explicit CapturePage(QWidget* parent = nullptr);

private slots:
    void onMethodChanged(int index);
    void onLoadConfig();

    void applyUdp();
    void applyTcp();
    void applyEth();
    void applyCard();
    void refreshEthAdapters();
    void refreshCaptureDevices();

private:
    void buildGeneralCard(QVBoxLayout* layout);
    void buildUdpCard(QVBoxLayout* layout);
    void buildTcpCard(QVBoxLayout* layout);
    void buildEthCard(QVBoxLayout* layout);
    void buildCardCard(QVBoxLayout* layout);
    void updateSectionVisibility();

    // ── General ──
    QComboBox* m_methodCombo{};
    QSpinBox* m_detResolution{};
    QSpinBox* m_captureFps{};
    QLabel* m_fpsWarning{};
    ToggleSwitch* m_circleMask{};

    // ── UDP ──
    CardWidget* m_udpCard{};
    QLineEdit* m_udpIp{};
    QSpinBox* m_udpPort{};

    // ── TCP ──
    CardWidget* m_tcpCard{};
    QLineEdit* m_tcpIp{};
    QSpinBox* m_tcpPort{};

    // ── Eth (ProSexy) ──
    CardWidget* m_ethCard{};
    QComboBox* m_ethAdapterCombo{};
    QLineEdit* m_ethEthertype{};

    // ── OpenCV / MF capture card ──
    CardWidget* m_cardCard{};
    QComboBox* m_devIndex{};
    QSpinBox* m_devWidth{};
    QSpinBox* m_devHeight{};
    QSpinBox* m_devFps{};
    QSpinBox* m_devCrop{};
    QComboBox* m_devFormat{};
    // OpenCV-only
    QWidget* m_apiRow{};
    QComboBox* m_devApi{};
    QWidget* m_urlRow{};
    QLineEdit* m_devUrl{};
    // MF-only
    QWidget* m_decodeRow{};
    QComboBox* m_devDecode{};
};
