#include "pages/HardwarePage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include "Apotheosis.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <mutex>

static const char* const kInputMethodIds[] = {
    "WIN32", "GHUB", "ARDUINO", "KMBOX_NET", "KMBOX_A", "MAKCU"
};
static constexpr int kInputMethodCount = 6;

HardwarePage::HardwarePage(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto& cm = ConfigManager::instance();

    // ── Card 1: 输入方式 ──
    auto* inputCard = new CardWidget(QStringLiteral("\xe8\xbe\x93\xe5\x85\xa5\xe6\x96\xb9\xe5\xbc\x8f"),
                                     QStringLiteral("plug"));

    m_inputMethodCombo = new QComboBox;
    m_inputMethodCombo->addItems({
        QStringLiteral("Win32"),
        QStringLiteral("Logitech GHUB"),
        QStringLiteral("Arduino"),
        QStringLiteral("Kmbox Net"),
        QStringLiteral("Kmbox A"),
        QStringLiteral("MAKCU"),
    });
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("\xe6\x96\xb9\xe5\xbc\x8f"), m_inputMethodCombo));
    layout->addWidget(inputCard);

    // ── Card 2: 连接状态 ──
    auto* statusCard = new CardWidget(QStringLiteral("\xe8\xbf\x9e\xe6\x8e\xa5\xe7\x8a\xb6\xe6\x80\x81"),
                                      QStringLiteral("wifi"));
    auto* sc = statusCard->contentLayout();
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);

    m_statusDot = new QLabel(QStringLiteral("\xe2\x97\x8f"));
    m_statusDot->setFixedWidth(20);
    statusRow->addWidget(m_statusDot);

    m_statusText = new QLabel;
    m_statusText->setStyleSheet("font-size:13px;");
    statusRow->addWidget(m_statusText, 1);

    m_connectBtn = new QPushButton(QStringLiteral("\xe8\xbf\x9e\xe6\x8e\xa5"));
    m_connectBtn->setFixedHeight(28);
    m_connectBtn->setCursor(Qt::PointingHandCursor);
    statusRow->addWidget(m_connectBtn);

    sc->addLayout(statusRow);
    layout->addWidget(statusCard);

    // ── Card 3: 设备参数 ──
    auto* deviceCard = new CardWidget(QStringLiteral("\xe8\xae\xbe\xe5\xa4\x87\xe5\x8f\x82\xe6\x95\xb0"),
                                      QStringLiteral("adjustments"));
    m_deviceStack = new QStackedWidget;

    auto makePanel = [](QVBoxLayout*& panelLayout) -> QWidget* {
        auto* page = new QWidget;
        panelLayout = new QVBoxLayout(page);
        panelLayout->setContentsMargins(0, 0, 0, 0);
        panelLayout->setSpacing(10);
        return page;
    };

    auto noConfigPanel = []() -> QWidget* {
        auto* page = new QWidget;
        auto* l = new QVBoxLayout(page);
        l->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(QStringLiteral("\xe6\x97\xa0\xe9\x9c\x80\xe9\xa2\x9d\xe5\xa4\x96\xe9\x85\x8d\xe7\xbd\xae"));
        label->setProperty("class", "secondary");
        l->addWidget(label);
        return page;
    };

    // Index 0: Win32
    m_deviceStack->addWidget(noConfigPanel());
    // Index 1: GHUB
    m_deviceStack->addWidget(noConfigPanel());

    // Index 2: Arduino
    {
        QVBoxLayout* lay = nullptr;
        auto* page = makePanel(lay);
        m_arduinoPort = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("\xe4\xb8\xb2\xe5\x8f\xa3"), m_arduinoPort));
        m_arduinoBaud = new QSpinBox;
        m_arduinoBaud->setRange(1200, 921600);
        lay->addWidget(FormKit::fieldRow(QStringLiteral("\xe6\xb3\xa2\xe7\x89\xb9\xe7\x8e\x87"), m_arduinoBaud));
        lay->addWidget(FormKit::toggleRow(QStringLiteral("16\xe4\xbd\x8d\xe9\xbc\xa0\xe6\xa0\x87"), false, m_arduino16bit));
        lay->addWidget(FormKit::toggleRow(QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe6\x8c\x89\xe9\x94\xae"), false, m_arduinoKeys));
        m_deviceStack->addWidget(page);
    }

    // Index 3: Kmbox Net
    {
        QVBoxLayout* lay = nullptr;
        auto* page = makePanel(lay);
        m_kmnetIp = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("IP \xe5\x9c\xb0\xe5\x9d\x80"), m_kmnetIp));
        m_kmnetPort = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("\xe7\xab\xaf\xe5\x8f\xa3"), m_kmnetPort));
        m_kmnetUuid = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("UUID"), m_kmnetUuid));
        m_deviceStack->addWidget(page);
    }

    // Index 4: Kmbox A
    {
        QVBoxLayout* lay = nullptr;
        auto* page = makePanel(lay);
        m_kmaPidvid = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("PID/VID"), m_kmaPidvid));
        m_deviceStack->addWidget(page);
    }

    // Index 5: MAKCU
    {
        QVBoxLayout* lay = nullptr;
        auto* page = makePanel(lay);
        m_makcuPort = new QLineEdit;
        lay->addWidget(FormKit::fieldRow(QStringLiteral("\xe4\xb8\xb2\xe5\x8f\xa3"), m_makcuPort));
        m_makcuBaud = new QSpinBox;
        m_makcuBaud->setRange(1200, 921600);
        lay->addWidget(FormKit::fieldRow(QStringLiteral("\xe6\xb3\xa2\xe7\x89\xb9\xe7\x8e\x87"), m_makcuBaud));
        m_deviceStack->addWidget(page);
    }

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    // ── Load values from config ──
    loadFieldsFromConfig();

    // ── Connect field changes → ConfigManager ──
    connect(m_inputMethodCombo, &QComboBox::currentIndexChanged,
            this, &HardwarePage::onInputMethodChanged);

    // Arduino
    connect(m_arduinoPort, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setArduinoPort(t);
    });
    connect(m_arduinoBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int v) {
        ConfigManager::instance().setArduinoBaudrate(v);
    });
    connect(m_arduino16bit, &ToggleSwitch::toggled, this, [](bool v) {
        ConfigManager::instance().setArduino16BitMouse(v);
    });
    connect(m_arduinoKeys, &ToggleSwitch::toggled, this, [](bool v) {
        ConfigManager::instance().setArduinoEnableKeys(v);
    });

    // Kmbox Net
    connect(m_kmnetIp, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setKmboxNetIp(t);
    });
    connect(m_kmnetPort, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setKmboxNetPort(t);
    });
    connect(m_kmnetUuid, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setKmboxNetUuid(t);
    });

    // Kmbox A
    connect(m_kmaPidvid, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setKmboxAPidvid(t);
    });

    // MAKCU
    connect(m_makcuPort, &QLineEdit::textChanged, this, [](const QString& t) {
        ConfigManager::instance().setMakcuPort(t);
    });
    connect(m_makcuBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int v) {
        ConfigManager::instance().setMakcuBaudrate(v);
    });

    // 连接 button
    connect(m_connectBtn, &QPushButton::clicked, this, &HardwarePage::reconnectDevice);

    // Auto-refresh timer
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &HardwarePage::refreshStatus);
    m_statusTimer->start();

    refreshStatus();
    layout->addStretch();
}

void HardwarePage::loadFieldsFromConfig()
{
    auto& cm = ConfigManager::instance();

    // Input method combo
    QString saved = cm.inputMethod();
    int idx = 0;
    for (int i = 0; i < kInputMethodCount; ++i) {
        if (saved == QString::fromLatin1(kInputMethodIds[i])) { idx = i; break; }
    }
    m_inputMethodCombo->blockSignals(true);
    m_inputMethodCombo->setCurrentIndex(idx);
    m_deviceStack->setCurrentIndex(idx);
    m_inputMethodCombo->blockSignals(false);

    // Arduino
    m_arduinoPort->setText(cm.arduinoPort());
    m_arduinoBaud->setValue(cm.arduinoBaudrate());
    m_arduino16bit->setChecked(cm.arduino16BitMouse());
    m_arduinoKeys->setChecked(cm.arduinoEnableKeys());

    // Kmbox Net
    m_kmnetIp->setText(cm.kmboxNetIp());
    m_kmnetPort->setText(cm.kmboxNetPort());
    m_kmnetUuid->setText(cm.kmboxNetUuid());

    // Kmbox A
    m_kmaPidvid->setText(cm.kmboxAPidvid());

    // MAKCU
    m_makcuPort->setText(cm.makcuPort());
    m_makcuBaud->setValue(cm.makcuBaudrate());

}

void HardwarePage::onInputMethodChanged(int index)
{
    m_deviceStack->setCurrentIndex(index);

    if (index >= 0 && index < kInputMethodCount) {
        ConfigManager::instance().setInputMethod(
            QString::fromLatin1(kInputMethodIds[index]));
    }

    reconnectDevice();
}

void HardwarePage::reconnectDevice()
{
    // Flush ALL hardware fields from ConfigManager → C++ config, then recreate
    auto& cm = ConfigManager::instance();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.input_method        = cm.inputMethod().toStdString();
        config.arduino_port        = cm.arduinoPort().toStdString();
        config.arduino_baudrate    = cm.arduinoBaudrate();
        config.arduino_16_bit_mouse = cm.arduino16BitMouse();
        config.arduino_enable_keys = cm.arduinoEnableKeys();
        config.kmbox_net_ip        = cm.kmboxNetIp().toStdString();
        config.kmbox_net_port      = cm.kmboxNetPort().toStdString();
        config.kmbox_net_uuid      = cm.kmboxNetUuid().toStdString();
        config.kmbox_a_pidvid      = cm.kmboxAPidvid().toStdString();
        config.makcu_port          = cm.makcuPort().toStdString();
        config.makcu_baudrate      = cm.makcuBaudrate();
    }

    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);

    refreshStatus();
}

void HardwarePage::refreshStatus()
{
    int method = m_inputMethodCombo ? m_inputMethodCombo->currentIndex() : 0;

    bool connected = false;
    bool ptrExists = false;
    QString deviceName;

    switch (method) {
    case 0:
        connected = true;
        ptrExists = true;
        deviceName = QStringLiteral("Win32 (\xe7\xb3\xbb\xe7\xbb\x9f\xe5\x86\x85\xe7\xbd\xae)");
        break;
    case 1:
        ptrExists = (gHub != nullptr);
        connected = ptrExists;
        deviceName = QStringLiteral("Logitech GHUB");
        break;
    case 2:
        ptrExists = (arduinoSerial != nullptr);
        connected = ptrExists && arduinoSerial->isOpen();
        deviceName = QStringLiteral("Arduino");
        break;
    case 3:
        ptrExists = (kmboxNetSerial != nullptr);
        connected = ptrExists && kmboxNetSerial->isOpen();
        deviceName = QStringLiteral("Kmbox Net");
        break;
    case 4:
        ptrExists = (kmboxASerial != nullptr);
        connected = ptrExists && kmboxASerial->isOpen();
        deviceName = QStringLiteral("Kmbox A");
        break;
    case 5:
        ptrExists = (makcuSerial != nullptr);
        connected = ptrExists && makcuSerial->isOpen();
        deviceName = QStringLiteral("MAKCU");
        break;
    default:
        deviceName = QStringLiteral("\xe6\x9c\xaa\xe7\x9f\xa5");
        break;
    }

    if (connected) {
        m_statusDot->setStyleSheet("color:#22C55E; font-size:16px;");
        m_statusText->setText(deviceName + QStringLiteral(" \xe2\x80\x94 \xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5"));
        m_statusText->setStyleSheet("color:#22C55E; font-size:13px;");
        m_connectBtn->setText(QStringLiteral("\xe9\x87\x8d\xe8\xbf\x9e"));
    } else {
        m_statusDot->setStyleSheet("color:#EF4444; font-size:16px;");
        QString detail = !ptrExists
            ? QStringLiteral(" \xe2\x80\x94 \xe6\x9c\xaa\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96")
            : QStringLiteral(" \xe2\x80\x94 \xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5");
        m_statusText->setText(deviceName + detail);
        m_statusText->setStyleSheet("color:#EF4444; font-size:13px;");
        m_connectBtn->setText(QStringLiteral("\xe8\xbf\x9e\xe6\x8e\xa5"));
    }
}
