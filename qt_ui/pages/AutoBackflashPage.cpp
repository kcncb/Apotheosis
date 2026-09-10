#include "pages/AutoBackflashPage.h"

#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "runtime/auto_backflash.h"
#include "runtime/inference_session.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_set>

namespace
{
QString zh(const char* text)
{
    return QString::fromUtf8(text);
}
}

AutoBackflashPage::AutoBackflashPage(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* intro = new CardWidget(zh(u8"自动背闪"), QStringLiteral("rotate-clockwise"));
    intro->contentLayout()->addWidget(FormKit::toggleRow(
        zh(u8"启用自动背闪"), config.auto_backflash_enabled, m_enabled));
    auto* description = new QLabel(zh(
        u8"指定类别连续出现后，左侧闪光向右转、右侧闪光向左转；等待设定时间，"
        u8"再快速反向偿还完全相同的移动量。"
        u8"功能不要求按住瞄准热键。"));
    description->setWordWrap(true);
    description->setStyleSheet("color:#71717A; font-size:12px;");
    intro->contentLayout()->addWidget(description);
    layout->addWidget(intro);

    auto* classesCard = new CardWidget(zh(u8"触发类别"), QStringLiteral("category"));
    m_classHint = new QLabel;
    m_classHint->setWordWrap(true);
    m_classHint->setStyleSheet("color:#71717A; font-size:12px;");
    classesCard->contentLayout()->addWidget(m_classHint);
    m_classes = new QListWidget;
    m_classes->setMinimumHeight(180);
    m_classes->setAlternatingRowColors(true);
    classesCard->contentLayout()->addWidget(m_classes);
    layout->addWidget(classesCard);

    int confirmFrames = 2;
    int turnAmount = 4000;
    int turnSpeed = 75;
    int returnDelay = 800;
    int returnSpeed = 100;
    int cooldown = 1500;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        confirmFrames = config.auto_backflash_confirm_frames;
        turnAmount = config.auto_backflash_turn_amount;
        turnSpeed = config.auto_backflash_turn_speed;
        returnDelay = config.auto_backflash_return_delay_ms;
        returnSpeed = config.auto_backflash_return_speed;
        cooldown = config.auto_backflash_cooldown_ms;
    }

    auto* paramsCard = new CardWidget(zh(u8"动作参数"), QStringLiteral("adjustments"));
    QSlider* slider = nullptr;
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"连续识别帧数"), 1, 8, confirmFrames,
        slider, m_confirmFrames, zh(u8" 帧")));
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"转身幅度"), 100, 30000, turnAmount,
        slider, m_turnAmount, zh(u8" 单位")));
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"转身速度"), 1, 100, turnSpeed,
        slider, m_turnSpeed, QStringLiteral("%")));
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"转回延迟"), 0, 5000, returnDelay,
        slider, m_returnDelay, QStringLiteral(" ms")));
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"返回速度"), 1, 100, returnSpeed,
        slider, m_returnSpeed, QStringLiteral("%")));
    paramsCard->contentLayout()->addWidget(FormKit::sliderRow(
        zh(u8"触发冷却"), 0, 10000, cooldown,
        slider, m_cooldown, QStringLiteral(" ms")));
    layout->addWidget(paramsCard);

    auto* testCard = new CardWidget(zh(u8"动作测试"), QStringLiteral("player-play"));
    auto* testHint = new QLabel(zh(
        u8"测试会使用当前幅度和速度向右转身，等待当前转回延迟后自动返回。"
        u8"请先启动推理并连接鼠标硬件。"));
    testHint->setWordWrap(true);
    testHint->setStyleSheet("color:#71717A; font-size:12px;");
    testCard->contentLayout()->addWidget(testHint);
    m_testButton = new QPushButton(zh(u8"测试转身并自动返回"));
    m_testButton->setFixedHeight(34);
    m_testButton->setCursor(Qt::PointingHandCursor);
    testCard->contentLayout()->addWidget(m_testButton);
    m_runtimeStatus = new QLabel(zh(u8"状态：待机"));
    m_runtimeStatus->setStyleSheet("color:#71717A; font-size:12px;");
    testCard->contentLayout()->addWidget(m_runtimeStatus);
    layout->addWidget(testCard);
    layout->addStretch();

    connect(m_enabled, &QAbstractButton::toggled, this, [this](bool enabled) {
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            config.auto_backflash_enabled = enabled;
        }
        markChanged();
    });

    const auto bind = [this](QSpinBox* control, int Config::* member) {
        connect(control, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, member](int value) {
            {
                std::lock_guard<std::recursive_mutex> lock(configMutex);
                config.*member = value;
            }
            markChanged();
        });
    };
    bind(m_confirmFrames, &Config::auto_backflash_confirm_frames);
    bind(m_turnAmount, &Config::auto_backflash_turn_amount);
    bind(m_turnSpeed, &Config::auto_backflash_turn_speed);
    bind(m_returnDelay, &Config::auto_backflash_return_delay_ms);
    bind(m_returnSpeed, &Config::auto_backflash_return_speed);
    bind(m_cooldown, &Config::auto_backflash_cooldown_ms);
    connect(m_testButton, &QPushButton::clicked,
            this, &AutoBackflashPage::runTest);

    rebuildClassList();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout,
            this, &AutoBackflashPage::refreshClasses);
    connect(m_pollTimer, &QTimer::timeout,
            this, &AutoBackflashPage::refreshRuntimeStatus);
    m_pollTimer->start();
    refreshRuntimeStatus();
}

void AutoBackflashPage::markChanged()
{
    ConfigBridge::instance().markDirty();
}

std::size_t AutoBackflashPage::classFingerprint() const
{
    std::size_t hash = 0;
    for (const auto& entry : config.class_filters)
    {
        hash ^= std::hash<int>{}(entry.class_id)
            + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::string>{}(entry.class_name)
            + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
}

void AutoBackflashPage::refreshClasses()
{
    int count = 0;
    std::size_t fingerprint = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        count = static_cast<int>(config.class_filters.size());
        fingerprint = classFingerprint();
    }
    if (count != m_lastClassCount || fingerprint != m_lastFingerprint)
        rebuildClassList();
}

void AutoBackflashPage::rebuildClassList()
{
    std::vector<ClassFilterState> classes;
    std::unordered_set<int> selected;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        classes = config.class_filters;
        selected.insert(config.auto_backflash_classes.begin(),
                        config.auto_backflash_classes.end());
        m_lastFingerprint = classFingerprint();
    }
    m_lastClassCount = static_cast<int>(classes.size());

    m_rebuilding = true;
    m_classes->clear();
    for (const auto& entry : classes)
    {
        const QString name = entry.class_name.empty()
            ? QStringLiteral("class_%1").arg(entry.class_id)
            : QString::fromUtf8(entry.class_name.c_str());
        auto* item = new QListWidgetItem(m_classes);
        item->setFlags(Qt::ItemIsEnabled);
        item->setSizeHint(QSize(0, 34));

        // 不使用 QListWidgetItem 的原生 checkState：它不会套用项目的
        // QCheckBox 主题，而且只有极小的 indicator 区域能响应点击。
        // 真实 QCheckBox 让“编号 + 类别名”整行文字都可以直接勾选。
        auto* check = new QCheckBox(
            QStringLiteral("[%1] %2").arg(entry.class_id).arg(name), m_classes);
        check->setProperty("classId", entry.class_id);
        check->setChecked(selected.count(entry.class_id) > 0);
        check->setCursor(Qt::PointingHandCursor);
        m_classes->setItemWidget(item, check);
        connect(check, &QCheckBox::toggled, this,
                [this](bool) { saveClassSelection(); });
    }
    m_rebuilding = false;

    if (classes.empty())
        m_classHint->setText(zh(u8"尚未加载模型。启动一次推理会话后，类别列表会自动出现。"));
    else
        m_classHint->setText(zh(
            u8"勾选一个或多个闪光类别。即使该类别在“目标”页设为删除，"
            u8"背闪检测仍会单独保留它，但不会让它参与瞄准。"));
}

void AutoBackflashPage::saveClassSelection()
{
    if (m_rebuilding) return;

    std::vector<int> selected;
    selected.reserve(static_cast<size_t>(m_classes->count()));
    for (int i = 0; i < m_classes->count(); ++i)
    {
        auto* check = qobject_cast<QCheckBox*>(
            m_classes->itemWidget(m_classes->item(i)));
        if (check && check->isChecked())
            selected.push_back(check->property("classId").toInt());
    }
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.auto_backflash_classes = std::move(selected);
    }
    markChanged();
}

void AutoBackflashPage::runTest()
{
    const bool running = g_inference_session && g_inference_session->running();
    const bool connected = (makcuSerial && makcuSerial->isOpen())
        || (makcuNewSerial && makcuNewSerial->isOpen());
    if (!running)
    {
        m_runtimeStatus->setText(zh(u8"状态：请先启动推理会话"));
        return;
    }
    if (!connected)
    {
        m_runtimeStatus->setText(zh(u8"状态：鼠标硬件未连接"));
        return;
    }
    auto_backflash::request_test();
    m_runtimeStatus->setText(zh(u8"状态：测试请求已发送"));
}

void AutoBackflashPage::refreshRuntimeStatus()
{
    QString text;
    switch (auto_backflash::published_phase())
    {
    case auto_backflash::Phase::Turning:   text = zh(u8"正在转身"); break;
    case auto_backflash::Phase::Holding:   text = zh(u8"等待转回"); break;
    case auto_backflash::Phase::Returning: text = zh(u8"正在返回"); break;
    case auto_backflash::Phase::Settling:  text = zh(u8"正在完成返回"); break;
    case auto_backflash::Phase::Cooldown:  text = zh(u8"冷却中"); break;
    case auto_backflash::Phase::Idle:
    default:                               text = zh(u8"待机"); break;
    }
    const int remaining = auto_backflash::published_remaining();
    if (remaining > 0)
        text += zh(u8"，剩余 %1 单位").arg(remaining);
    m_runtimeStatus->setText(zh(u8"状态：") + text);
    m_testButton->setEnabled(
        auto_backflash::published_phase() == auto_backflash::Phase::Idle
        || auto_backflash::published_phase() == auto_backflash::Phase::Cooldown);
}
