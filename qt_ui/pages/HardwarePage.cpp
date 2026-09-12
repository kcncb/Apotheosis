#include "pages/HardwarePage.h"

#include "Apotheosis.h"
#include "runtime/config_snapshot.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QComboBox>
#include <QDoubleSpinBox>
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

    deviceCard->contentLayout()->addWidget(m_deviceStack);
    layout->addWidget(deviceCard);

    // 「图像与鼠标映射」已移除。
    //
    // 现役控制链(mouse/aim_pid.h)全程在【鼠标计数】域输出: 控制器算完直接把计数交给
    // sendRawMove, 不存在"像素 -> 计数"这一步。而 mouse_pixels_per_count_x/y
    // 与发送后生效延迟/误差范围这三个参数, 本来就是旧 predictive_controller 用来
    // "扣除自身在途指令的像素位移"的写死标定 —— 现在没有任何代码读取它们。
    // 新控制器同样需要"每计数多少像素", 但它是观测器(mouse/aim_motion.h)在线估出来的,
    // 不落盘、不需要用户标定, 所以这里依旧没有可填的字段。
    //
    // 只保留采集回调前帧龄: 它仍然参与延迟遥测的采集时间戳修正。
    auto* mappingCard = new CardWidget(zh(u8"延迟估计"), QStringLiteral("adjustments"));
    auto addMapping = [&](const char* title, double Config::*field, double low, double high, const char* suffix) {
        auto* spin = new QDoubleSpinBox;
        spin->setDecimals(4); spin->setRange(low, high); spin->setSuffix(QString::fromUtf8(suffix));
        { std::lock_guard<std::recursive_mutex> lock(configMutex); spin->setValue(config.*field); }
        mappingCard->contentLayout()->addWidget(FormKit::fieldRow(zh(title), spin));
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [field](double v) {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            config.*field = v;
            ConfigBridge::instance().markDirty();
        });
    };
    addMapping(u8"采集回调前帧龄（估计）", &Config::capture_age_offset_ms, 0, 100, " ms");
    auto* note = new QLabel(zh(u8"回调前帧龄是模型估计, 填 0 表示尚未补偿。它只影响延迟遥测的时间戳, 不参与控制回路。"));
    note->setWordWrap(true); mappingCard->contentLayout()->addWidget(note);
    layout->addWidget(mappingCard);

    // 准星找色平滑: 以前只有配置文件里有这一项, 界面上看不到, 默认还是 0(等于关闭)。
    // 但准星枢轴是【误差的基准】—— 它抖多少, 控制器就白挨多少。这里给它一个可见的旋钮。
    {
        auto* crossCard = new CardWidget(zh(u8"准星找色"), QStringLiteral("crosshair"));
        auto* spin = new QDoubleSpinBox;
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setRange(0.0, 1.0);
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            spin->setValue(static_cast<double>(config.crosshair_smooth));
        }
        crossCard->contentLayout()->addWidget(FormKit::fieldRow(zh(u8"平滑强度"), spin));
        auto* tip = new QLabel(zh(u8"0~1：越大越平滑(静止时重平滑、快速移动时自动放开，所以不会拖慢甩枪)。"));
        tip->setWordWrap(true);
        crossCard->contentLayout()->addWidget(tip);
        auto* tip2 = new QLabel(zh(u8"填 0 或很小都按默认 0.5 处理：这道防线不该被关掉 —— 开火时准星被火光/烟雾干扰出现的跳点，会直接变成控制器的错误输入。"));
        tip2->setWordWrap(true);
        crossCard->contentLayout()->addWidget(tip2);
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [](double v) {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            config.crosshair_smooth = static_cast<float>(v);
            ConfigBridge::instance().markDirty();
        });
        layout->addWidget(crossCard);
    }

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

    runtime_config::publish();
    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
    refreshStatus();
}

void HardwarePage::refreshStatus()
{
    const bool useNew = m_inputMethodCombo && m_inputMethodCombo->currentIndex() == 1;
    std::lock_guard<std::mutex> deviceLock(inputDeviceMutex);
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
