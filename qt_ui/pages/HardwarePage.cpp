#include "pages/HardwarePage.h"

#include "Apotheosis.h"
#include "runtime/config_snapshot.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include "config/config_bridge.h"
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

#include "mouse/Makcu.h"
#include "mouse/MakcuNew.h"
#include "mouse/kmboxNetConnection.h"

namespace {

QString zh(const char* text)
{
    return QString::fromUtf8(text);
}

constexpr const char* kInputMethodIds[] = {"MAKCU", "MAKCUNEW", "KMBOXNET"};
constexpr int kInputMethodCount = 3;

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
    // 三档走同一套驱动抽象 (mouse/mouse_driver.h, 形状移植自 AimMagic 的 FUN_140040ff0)
    m_inputMethodCombo->addItems({
        QStringLiteral("MAKCU"),
        QStringLiteral("MAKCUNEW"),
        QStringLiteral("KMBOXNET")
    });
    inputCard->contentLayout()->addWidget(
        FormKit::fieldRow(zh(u8"方式"), m_inputMethodCombo));
    layout->addWidget(inputCard);

    auto* statusCard = new CardWidget(zh(u8"连接状态"), QStringLiteral("wifi"));
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_statusDot = new QLabel(QString::fromUtf8(u8"●"));
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
        m_makcuNewBaud->setRange(1200, 6000000);
        panel->addWidget(FormKit::fieldRow(zh(u8"波特率"), m_makcuNewBaud));
        m_deviceStack->addWidget(page);
    }

    {
        auto* page = new QWidget;
        auto* panel = new QVBoxLayout(page);
        panel->setContentsMargins(0, 0, 0, 0);
        panel->setSpacing(10);
        m_kmboxNetIp = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"盒子 IP"), m_kmboxNetIp));
        m_kmboxNetPort = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"端口"), m_kmboxNetPort));
        m_kmboxNetUuid = new QLineEdit;
        panel->addWidget(FormKit::fieldRow(zh(u8"UUID / MAC"), m_kmboxNetUuid));
        m_deviceStack->addWidget(page);
    }

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    // ── 【2026-09-13 删除】「延迟估计」卡片(含它唯一的 addMapping 字段) ──────────
    //
    // 这张卡片曾经装过「图像与鼠标映射」那几个标定量, 后来只剩「采集回调前帧龄（估计）」
    // 一个输入框。那个框也删掉之后, 卡片就【一个字段都不剩】了 —— 如果只删字段、留下
    // 卡片, 界面上会出现一个只有标题的空壳(与「移动锁死瞄准」那次是同一类错误)。
    // 所以这里连卡片和 addMapping lambda 一起删。
    //
    // 背景(为什么没有可填的字段了):
    //   现役控制链(mouse/aim_pid.h)全程在【鼠标计数】域输出: 控制器算完直接把计数交给
    //   sendRawMove, 不存在"像素 -> 计数"这一步。mouse_pixels_per_count_x/y 与"发送后
    //   生效延迟/误差范围"这三个参数, 本来就是旧 predictive_controller 用来"扣除自身
    //   在途指令的像素位移"的写死标定 —— 没有任何代码读取它们。
    //   帧龄估计同样是手填的猜测值, 控制器不吃任何延迟估计。
    //   真实的端到端延迟由 latency_probe 逐帧实测并落进延迟日志(那个保留)。

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
    connect(m_kmboxNetIp, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetIp(value);
    });
    connect(m_kmboxNetPort, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetPort(value);
    });
    connect(m_kmboxNetUuid, &QLineEdit::textChanged, this, [](const QString& value) {
        ConfigManager::instance().setKmboxNetUuid(value);
    });
    connect(m_connectBtn, &QPushButton::clicked, this, &HardwarePage::reconnectDevice);

    // 切换全局配置方案后, 设备类型/串口必须跟着新方案走。
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &HardwarePage::loadFieldsFromConfig);

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
    const QString method = cm.inputMethod();
    int index = 0;
    if (method == QStringLiteral("MAKCUNEW")) index = 1;
    else if (method == QStringLiteral("KMBOXNET")) index = 2;

    m_inputMethodCombo->blockSignals(true);
    m_inputMethodCombo->setCurrentIndex(index);
    m_deviceStack->setCurrentIndex(index);
    m_inputMethodCombo->blockSignals(false);

    m_makcuPort->setText(cm.makcuPort());
    m_makcuBaud->setValue(cm.makcuBaudrate());
    m_makcuNewPort->setText(cm.makcuNewPort());
    m_makcuNewBaud->setValue(cm.makcuNewBaudrate());
    m_kmboxNetIp->setText(cm.kmboxNetIp());
    m_kmboxNetPort->setText(cm.kmboxNetPort());
    m_kmboxNetUuid->setText(cm.kmboxNetUuid());
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
        config.kmbox_net_ip = cm.kmboxNetIp().toStdString();
        config.kmbox_net_port = cm.kmboxNetPort().toStdString();
        config.kmbox_net_uuid = cm.kmboxNetUuid().toStdString();
    }

    runtime_config::publish();
    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
    refreshStatus();
}

extern MakcuConnection* makcuSerial;
extern MakcuNewConnection* makcuNewSerial;
extern KmboxNetConnection* kmboxNetSerial;

void HardwarePage::refreshStatus()
{
    const int idx = m_inputMethodCombo ? m_inputMethodCombo->currentIndex() : 0;
    std::lock_guard<std::mutex> deviceLock(inputDeviceMutex);

    bool pointerExists = false;
    bool connected = false;
    QString deviceName;

    switch (idx)
    {
    case 0:
        deviceName = QStringLiteral("MAKCU");
        pointerExists = (makcuSerial != nullptr);
        connected = pointerExists && makcuSerial->isOpen();
        break;
    case 1:
        deviceName = QStringLiteral("MAKCUNEW");
        pointerExists = (makcuNewSerial != nullptr);
        connected = pointerExists && makcuNewSerial->isOpen();
        break;
    case 2:
        deviceName = QStringLiteral("KMBOXNET");
        pointerExists = (kmboxNetSerial != nullptr);
        connected = pointerExists && kmboxNetSerial->isOpen();
        break;
    default:
        deviceName = zh(u8"未知");
        break;
    }

    if (connected) {
        m_statusDot->setStyleSheet("color:#22C55E; font-size:16px;");
        m_statusText->setText(deviceName + zh(u8" — 已连接"));
        m_statusText->setStyleSheet("color:#22C55E; font-size:13px;");
        m_connectBtn->setText(zh(u8"重连"));
    } else {
        m_statusDot->setStyleSheet("color:#EF4444; font-size:16px;");
        m_statusText->setText(deviceName + (pointerExists
            ? zh(u8" — 连接失败(检查IP/端口/UUID或串口号)")
            : zh(u8" — 未初始化")));
        m_statusText->setStyleSheet("color:#EF4444; font-size:13px;");
        m_connectBtn->setText(zh(u8"连接"));
    }
}
