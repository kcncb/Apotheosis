#include "pages/HardwarePage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <mutex>

namespace {

QString zh(const char* text)
{
    return QString::fromUtf8(text);
}

constexpr const char* kInputMethodIds[] = {"MAKCU", "MAKCUNEW"};
constexpr int kInputMethodCount = 2;

} // namespace

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

    auto* inputCard = new CardWidget(zh(u8"输入方式"), QStringLiteral("plug"));
    m_inputMethodCombo = new QComboBox;
    m_inputMethodCombo->addItems({QStringLiteral("MAKCU"), QStringLiteral("MAKCUNEW")});
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(zh(u8"方式"), m_inputMethodCombo));
    layout->addWidget(inputCard);

    auto* statusCard = new CardWidget(zh(u8"连接状态"), QStringLiteral("wifi"));
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_statusDot = new QLabel(QStringLiteral("●"));
    m_statusDot->setFixedWidth(20);
    statusRow->addWidget(m_statusDot);
    m_statusText = new QLabel;
    m_statusText->setStyleSheet("font-size:13px;");
    statusRow->addWidget(m_statusText, 1);
    m_connectBtn = new QPushButton(zh(u8"连接"));
    m_connectBtn->setFixedHeight(28);
    m_connectBtn->setCursor(Qt::PointingHandCursor);
    statusRow->addWidget(m_connectBtn);
    statusCard->contentLayout()->addLayout(statusRow);
    layout->addWidget(statusCard);

    auto* deviceCard = new CardWidget(zh(u8"设备参数"), QStringLiteral("adjustments"));
    m_deviceStack = new QStackedWidget;

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_makcuPort = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuPort));
        m_makcuBaud = new QSpinBox;
        m_makcuBaud->setRange(1200, 921600);
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuBaud));
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_makcuNewPort = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"串口"), m_makcuNewPort));
        m_makcuNewBaud = new QSpinBox;
        m_makcuNewBaud->setRange(1200, 4000000);
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaud));
        m_deviceStack->addWidget(page);
    }

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    loadFieldsFromConfig();

    connect(m_inputMethodCombo, &QComboBox::currentIndexChanged,
            this, &HardwarePage::onInputMethodChanged);
    connect(m_makcuPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setMakcuPort(value);
    });
    connect(m_makcuBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
        ConfigManager::instance().setMakcuBaudrate(value);
    });
    connect(m_makcuNewPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setMakcuNewPort(value);
    });
    connect(m_makcuNewBaud, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
        ConfigManager::instance().setMakcuNewBaudrate(value);
    });
    connect(m_connectBtn, &QPushButton::clicked, this, &HardwarePage::reconnectDevice);

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
    const int index = cm.inputMethod() == QStringLiteral("MAKCUNEW") ? 1 : 0;
    m_inputMethodCombo->blockSignals(true);
    m_inputMethodCombo->setCurrentIndex(index);
    m_deviceStack->setCurrentIndex(index);
    m_inputMethodCombo->blockSignals(false);

    m_makcuPort->setText(cm.makcuPort());
    m_makcuBaud->setValue(cm.makcuBaudrate());
    m_makcuNewPort->setText(cm.makcuNewPort());
    m_makcuNewBaud->setValue(cm.makcuNewBaudrate());
}

void HardwarePage::onInputMethodChanged(int index)
{
    m_deviceStack->setCurrentIndex(index);
    if (index >= 0 && index < kInputMethodCount)
        ConfigManager::instance().setInputMethod(QString::fromLatin1(kInputMethodIds[index]));
    reconnectDevice();
}

void HardwarePage::reconnectDevice()
{
    auto& cm = ConfigManager::instance();
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.input_method = cm.inputMethod().toStdString();
        config.makcu_port = cm.makcuPort().toStdString();
        config.makcu_baudrate = cm.makcuBaudrate();
        config.makcu_new_port = cm.makcuNewPort().toStdString();
        config.makcu_new_baudrate = cm.makcuNewBaudrate();
    }

    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
    refreshStatus();
}

void HardwarePage::refreshStatus()
{
    const bool useNew = m_inputMethodCombo && m_inputMethodCombo->currentIndex() == 1;
    const bool pointerExists = useNew ? makcuNewSerial != nullptr : makcuSerial != nullptr;
    const bool connected = useNew
        ? pointerExists && makcuNewSerial->isOpen()
        : pointerExists && makcuSerial->isOpen();
    const QString deviceName = useNew ? QStringLiteral("MAKCUNEW") : QStringLiteral("MAKCU");

    if (connected) {
        m_statusDot->setStyleSheet("color:#22C55E; font-size:16px;");
        m_statusText->setText(deviceName + zh(u8" — 已连接"));
        m_statusText->setStyleSheet("color:#22C55E; font-size:13px;");
        m_connectBtn->setText(zh(u8"重连"));
    } else {
        m_statusDot->setStyleSheet("color:#EF4444; font-size:16px;");
        m_statusText->setText(deviceName + (pointerExists
            ? zh(u8" — 连接失败") : zh(u8" — 未初始化")));
        m_statusText->setStyleSheet("color:#EF4444; font-size:13px;");
        m_connectBtn->setText(zh(u8"连接"));
    }
}
