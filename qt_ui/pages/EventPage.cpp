#include "pages/EventPage.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>

#include "Apotheosis.h"
#include "config.h"
#include "config/config_bridge.h"
#include "runtime/event_orchestrator.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

using namespace event_orch;

// ─── VK / 鼠标按钮下拉选项(值 = Win32 VK code / 按钮号)────────────────────
namespace
{
struct Choice { const char* label; int value; };

// 空 label 表示分隔线。
const std::vector<Choice> kKeyChoices = {
    // ── 字母 ──
    {"", 0},
    {"A", 0x41}, {"B", 0x42}, {"C", 0x43}, {"D", 0x44}, {"E", 0x45}, {"F", 0x46},
    {"G", 0x47}, {"H", 0x48}, {"I", 0x49}, {"J", 0x4A}, {"K", 0x4B}, {"L", 0x4C},
    {"M", 0x4D}, {"N", 0x4E}, {"O", 0x4F}, {"P", 0x50}, {"Q", 0x51}, {"R", 0x52},
    {"S", 0x53}, {"T", 0x54}, {"U", 0x55}, {"V", 0x56}, {"W", 0x57}, {"X", 0x58},
    {"Y", 0x59}, {"Z", 0x5A},
    // ── 数字 ──
    {"", 0},
    {"0", 0x30}, {"1", 0x31}, {"2", 0x32}, {"3", 0x33}, {"4", 0x34},
    {"5", 0x35}, {"6", 0x36}, {"7", 0x37}, {"8", 0x38}, {"9", 0x39},
    // ── 功能键 ──
    {"", 0},
    {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73},  {"F5", 0x74},  {"F6", 0x75},
    {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B},
    // ── 修饰键 ──
    {"", 0},
    {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12},
    {"LShift", 0xA0}, {"RShift", 0xA1},
    {"LCtrl", 0xA2},  {"RCtrl", 0xA3},
    {"LAlt", 0xA4},   {"RAlt", 0xA5},
    {"LWin", 0x5B},   {"RWin", 0x5C},
    // ── 编辑 / 空白键 ──
    {"", 0},
    {"Space", 0x20},     {"Enter", 0x0D},     {"Tab", 0x09},   {"Esc", 0x1B},
    {"Backspace", 0x08}, {"Delete", 0x2E},    {"Insert", 0x2D},
    {"Home", 0x24},      {"End", 0x23},
    {"PageUp", 0x21},    {"PageDown", 0x22},  {"CapsLock", 0x14},
    // ── 方向键 ──
    {"", 0},
    {"\xe2\x86\x91 Up", 0x26},    // ↑ Up
    {"\xe2\x86\x93 Down", 0x28},  // ↓ Down
    {"\xe2\x86\x90 Left", 0x25},  // ← Left
    {"\xe2\x86\x92 Right", 0x27}, // → Right
    // ── 符号 ──
    {"", 0},
    {";",  0xBA}, {"=",  0xBB}, {",", 0xBC}, {"-", 0xBD}, {".", 0xBE}, {"/", 0xBF},
    {"`",  0xC0}, {"[",  0xDB}, {"\\", 0xDC}, {"]", 0xDD}, {"'", 0xDE},
    // ── 小键盘 ──
    {"", 0},
    {"Num 0", 0x60}, {"Num 1", 0x61}, {"Num 2", 0x62}, {"Num 3", 0x63}, {"Num 4", 0x64},
    {"Num 5", 0x65}, {"Num 6", 0x66}, {"Num 7", 0x67}, {"Num 8", 0x68}, {"Num 9", 0x69},
    {"Num *", 0x6A}, {"Num +", 0x6B}, {"Num -", 0x6D}, {"Num .", 0x6E}, {"Num /", 0x6F},
};

const std::vector<Choice> kMouseChoices = {
    {"\xe5\xb7\xa6\xe9\x94\xae",         0},   // 左键
    {"\xe5\x8f\xb3\xe9\x94\xae",         1},   // 右键
    {"\xe4\xb8\xad\xe9\x94\xae",         2},   // 中键
    {"X1 \xe4\xbe\xa7\xe9\x94\xae",      3},   // X1 侧键
    {"X2 \xe4\xbe\xa7\xe9\x94\xae",      4},   // X2 侧键
};

void fill_combo(QComboBox* c, const std::vector<Choice>& choices)
{
    c->clear();
    for (const auto& ch : choices)
    {
        if (ch.label[0] == '\0')
        {
            c->insertSeparator(c->count());
        }
        else
        {
            c->addItem(QString::fromUtf8(ch.label), ch.value);
        }
    }
}

// 找 value 对应的 item index;找不到返回第一个可选项。
int find_choice_index(QComboBox* c, int value)
{
    for (int i = 0; i < c->count(); ++i)
    {
        const QVariant v = c->itemData(i);
        if (v.isValid() && v.toInt() == value) return i;
    }
    // 找第一个有 data 的 item
    for (int i = 0; i < c->count(); ++i)
        if (c->itemData(i).isValid()) return i;
    return 0;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 单条规则的可编辑 Card。行为:内部字段变化 → 通过回调告知 EventPage 保存。
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
class RuleCard : public QWidget
{
public:
    RuleCard(const Rule& r, EventPage* owner)
        : m_owner(owner)
    {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);

        m_card = new CardWidget(QString::fromStdString(r.name),
                                QStringLiteral("activity"));
        m_card->setCollapsible(true);
        auto* cl = m_card->contentLayout();

        // 头部行:名字 + 启用开关 + 删除按钮
        auto* headRow = new QHBoxLayout;
        m_name = new QLineEdit(QString::fromStdString(r.name));
        m_name->setPlaceholderText(QString::fromUtf8(u8"规则名称"));
        m_enabled = new ToggleSwitch;
        m_enabled->setChecked(r.enabled);
        m_delBtn = new QPushButton(QString::fromUtf8(u8"删除"));
        m_delBtn->setFixedWidth(64);
        headRow->addWidget(new QLabel(QString::fromUtf8(u8"名称")));
        headRow->addWidget(m_name, /*stretch*/1);
        headRow->addWidget(new QLabel(QString::fromUtf8(u8"启用")));
        headRow->addWidget(m_enabled);
        headRow->addWidget(m_delBtn);
        cl->addLayout(headRow);

        // event + cooldown 行
        auto* row2 = new QHBoxLayout;
        m_event = new QComboBox;
        for (int i = 0; i < static_cast<int>(EventType::Count); ++i)
            m_event->addItem(QString::fromUtf8(event_name(static_cast<EventType>(i))));
        m_event->setCurrentIndex(std::clamp(static_cast<int>(r.event), 0,
                                            static_cast<int>(EventType::Count) - 1));

        m_cooldown = new QSpinBox;
        m_cooldown->setRange(0, 600000);
        m_cooldown->setSuffix(QStringLiteral(" ms"));
        m_cooldown->setValue(r.cooldown_ms);
        m_cooldown->setToolTip(QString::fromUtf8(
            u8"两次事件触发的最小间隔（0=不限）"));

        row2->addWidget(new QLabel(QString::fromUtf8(u8"事件")));
        row2->addWidget(m_event, 1);
        row2->addWidget(new QLabel(QString::fromUtf8(u8"冷却")));
        row2->addWidget(m_cooldown);
        cl->addLayout(row2);

        // 执行方式:单次 / 固定循环 / 在事件有效期间持续循环。
        auto* modeRow = new QHBoxLayout;
        m_mode = new QComboBox;
        for (int i = 0; i < static_cast<int>(event_orch::ExecutionMode::Count); ++i)
            m_mode->addItem(QString::fromUtf8(
                execution_mode_name(static_cast<event_orch::ExecutionMode>(i))));
        m_mode->setCurrentIndex(std::clamp(static_cast<int>(r.execution_mode), 0,
                                           static_cast<int>(event_orch::ExecutionMode::Count) - 1));
        m_mode->setToolTip(QString::fromUtf8(
            u8"单次：每次事件执行一轮；固定循环：执行指定轮数；"
            u8"持续期间循环：直到目标丢失、开火松开或寻光结束。"));

        m_repeatOptions = new QWidget;
        auto* repeatLayout = new QHBoxLayout(m_repeatOptions);
        repeatLayout->setContentsMargins(0, 0, 0, 0);
        repeatLayout->setSpacing(6);
        m_repeatCountLabel = new QLabel(QString::fromUtf8(u8"次数"));
        m_repeatCount = new QSpinBox;
        m_repeatCount->setRange(2, 1000);
        m_repeatCount->setValue(std::clamp(r.repeat_count, 2, 1000));
        m_repeatInterval = new QSpinBox;
        m_repeatInterval->setRange(1, 600000);
        m_repeatInterval->setSuffix(QStringLiteral(" ms"));
        m_repeatInterval->setValue(std::clamp(r.repeat_interval_ms, 1, 600000));
        repeatLayout->addWidget(m_repeatCountLabel);
        repeatLayout->addWidget(m_repeatCount);
        repeatLayout->addWidget(new QLabel(QString::fromUtf8(u8"轮间隔")));
        repeatLayout->addWidget(m_repeatInterval);

        modeRow->addWidget(new QLabel(QString::fromUtf8(u8"执行方式")));
        modeRow->addWidget(m_mode, 1);
        modeRow->addWidget(m_repeatOptions);
        cl->addLayout(modeRow);
        refreshModeUi();

        // Actions
        m_actionsBox = new QVBoxLayout;
        m_actionsBox->setSpacing(4);
        cl->addLayout(m_actionsBox);

        m_addActionBtn = new QPushButton(QString::fromUtf8(u8"+ 添加动作"));
        cl->addWidget(m_addActionBtn);

        for (const auto& a : r.actions)
            appendActionRow(a);

        outer->addWidget(m_card);

        wireSignals();
    }

    void wireSignals();

    Rule collect() const
    {
        Rule r;
        r.name        = m_name->text().toStdString();
        r.enabled     = m_enabled->isChecked();
        r.event       = static_cast<EventType>(m_event->currentIndex());
        r.execution_mode = static_cast<event_orch::ExecutionMode>(m_mode->currentIndex());
        r.repeat_count = m_repeatCount->value();
        r.repeat_interval_ms = m_repeatInterval->value();
        r.cooldown_ms = m_cooldown->value();
        for (const auto& row : m_rows)
        {
            Action a;
            a.type = static_cast<ActionType>(row.type->currentIndex());
            // 按 type 从对应的 widget 读值。
            switch (a.type)
            {
            case ActionType::Delay:
                a.a = row.aDelayMs->value();
                break;
            case ActionType::KeyTap:
            case ActionType::KeyDown:
            case ActionType::KeyUp:
                a.a = row.aKey->currentData().toInt();
                break;
            case ActionType::MouseTap:
            case ActionType::MouseDown:
            case ActionType::MouseUp:
                a.a = row.aMouse->currentData().toInt();
                break;
            case ActionType::Count:
                break;
            }
            a.b = row.b->value();
            r.actions.push_back(a);
        }
        return r;
    }

    QPushButton* deleteButton() { return m_delBtn; }

private:
    void refreshModeUi()
    {
        event_orch::ExecutionMode mode =
            static_cast<event_orch::ExecutionMode>(m_mode->currentIndex());
        // “切换目标”没有唯一结束边沿，不能安全使用持续模式。
        if (mode == event_orch::ExecutionMode::WhileActive &&
            m_event->currentIndex() == static_cast<int>(EventType::TargetSwitched))
        {
            m_mode->setCurrentIndex(static_cast<int>(event_orch::ExecutionMode::Once));
            mode = event_orch::ExecutionMode::Once;
        }
        m_repeatOptions->setVisible(mode != event_orch::ExecutionMode::Once);
        const bool fixed = (mode == event_orch::ExecutionMode::RepeatCount);
        m_repeatCountLabel->setVisible(fixed);
        m_repeatCount->setVisible(fixed);
    }

    struct Row
    {
        QWidget*        container;
        QComboBox*      type;
        QLabel*         aLabel;
        QStackedWidget* aStack;     // 3 页:延迟 / 按键 / 鼠标
        QSpinBox*       aDelayMs;   // 页 0
        QComboBox*      aKey;       // 页 1
        QComboBox*      aMouse;     // 页 2
        QLabel*         bLabel;
        QSpinBox*       b;
        QPushButton*    del;
    };

    // 根据当前 type 切 stack 页 + 更新 label + 处理 b 可见性。
    static void refreshRow(Row& row)
    {
        const auto t = static_cast<ActionType>(row.type->currentIndex());
        switch (t)
        {
        case ActionType::Delay:
            row.aLabel->setText(QString::fromUtf8(u8"延迟"));
            row.aStack->setCurrentIndex(0);
            row.bLabel->setVisible(false);
            row.b->setVisible(false);
            break;

        case ActionType::KeyTap:
        case ActionType::KeyDown:
        case ActionType::KeyUp:
            row.aLabel->setText(QString::fromUtf8(u8"按键"));
            row.aStack->setCurrentIndex(1);
            {
                const bool need_b = (t == ActionType::KeyTap);
                row.bLabel->setVisible(need_b);
                row.b->setVisible(need_b);
                if (need_b) row.bLabel->setText(QString::fromUtf8(u8"按住"));
            }
            break;

        case ActionType::MouseTap:
        case ActionType::MouseDown:
        case ActionType::MouseUp:
            row.aLabel->setText(QString::fromUtf8(u8"按钮"));
            row.aStack->setCurrentIndex(2);
            {
                const bool need_b = (t == ActionType::MouseTap);
                row.bLabel->setVisible(need_b);
                row.b->setVisible(need_b);
                if (need_b) row.bLabel->setText(QString::fromUtf8(u8"按住"));
            }
            break;

        case ActionType::Count:
            break;
        }
    }

    void appendActionRow(const Action& a)
    {
        auto* c = new QWidget;
        auto* h = new QHBoxLayout(c);
        h->setContentsMargins(0, 0, 0, 0);

        auto* type = new QComboBox;
        for (int i = 0; i < static_cast<int>(ActionType::Count); ++i)
            type->addItem(QString::fromUtf8(action_name(static_cast<ActionType>(i))));
        type->setCurrentIndex(std::clamp(static_cast<int>(a.type), 0,
                                          static_cast<int>(ActionType::Count) - 1));

        auto* aLabel = new QLabel;

        // Stack:延迟(spinbox) / 按键(combo) / 鼠标(combo)。
        auto* aStack = new QStackedWidget;
        aStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* aDelayMs = new QSpinBox;
        aDelayMs->setRange(0, 600000);
        aDelayMs->setSuffix(QStringLiteral(" ms"));

        auto* aKey = new QComboBox;
        fill_combo(aKey, kKeyChoices);

        auto* aMouse = new QComboBox;
        fill_combo(aMouse, kMouseChoices);

        aStack->addWidget(aDelayMs);   // index 0
        aStack->addWidget(aKey);       // index 1
        aStack->addWidget(aMouse);     // index 2

        // 按 type 初始化各页的当前值。收敛不到预设时,combo 选第一个可选项。
        const auto init_t = static_cast<ActionType>(type->currentIndex());
        if (init_t == ActionType::Delay)
        {
            aDelayMs->setValue(a.a);
        }
        else if (init_t == ActionType::KeyTap || init_t == ActionType::KeyDown
              || init_t == ActionType::KeyUp)
        {
            aKey->setCurrentIndex(find_choice_index(aKey, a.a));
            aDelayMs->setValue(50);
        }
        else
        {
            aMouse->setCurrentIndex(find_choice_index(aMouse, a.a));
            aDelayMs->setValue(50);
        }

        auto* bLabel = new QLabel;
        auto* bv = new QSpinBox;
        bv->setRange(1, 60000);
        bv->setSuffix(QStringLiteral(" ms"));
        bv->setValue(a.b);

        auto* del = new QPushButton(QString::fromUtf8(u8"×"));
        del->setFixedWidth(28);

        h->addWidget(type, 2);
        h->addWidget(aLabel);
        h->addWidget(aStack, 2);
        h->addWidget(bLabel);
        h->addWidget(bv, 1);
        h->addWidget(del);

        m_actionsBox->addWidget(c);

        Row row{c, type, aLabel, aStack, aDelayMs, aKey, aMouse, bLabel, bv, del};
        m_rows.push_back(row);
        refreshRow(m_rows.back());

        QObject::connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         this, [this, c] {
                             for (auto& row : m_rows)
                                 if (row.container == c) { refreshRow(row); break; }
                             emitChanged();
                         });
        QObject::connect(aDelayMs, QOverload<int>::of(&QSpinBox::valueChanged),
                         this, [this] { emitChanged(); });
        QObject::connect(aKey, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         this, [this] { emitChanged(); });
        QObject::connect(aMouse, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         this, [this] { emitChanged(); });
        QObject::connect(bv, QOverload<int>::of(&QSpinBox::valueChanged),
                         this, [this] { emitChanged(); });
        QObject::connect(del, &QPushButton::clicked, this, [this, c] {
            for (auto it = m_rows.begin(); it != m_rows.end(); ++it)
                if (it->container == c) { m_rows.erase(it); break; }
            c->deleteLater();
            emitChanged();
        });
    }

    void emitChanged();

    EventPage* m_owner;
    CardWidget* m_card{};
    QLineEdit* m_name{};
    ToggleSwitch* m_enabled{};
    QPushButton* m_delBtn{};
    QComboBox* m_event{};
    QComboBox* m_mode{};
    QSpinBox*  m_cooldown{};
    QWidget*   m_repeatOptions{};
    QLabel*    m_repeatCountLabel{};
    QSpinBox*  m_repeatCount{};
    QSpinBox*  m_repeatInterval{};
    QVBoxLayout* m_actionsBox{};
    QPushButton* m_addActionBtn{};

    std::vector<Row> m_rows;
};

} // namespace

// ─── EventPage ──────────────────────────────────────────────────────────────

EventPage::EventPage(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    m_scroll->setWidget(content);

    // 顶部按钮 + 说明
    m_addBtn = new QPushButton(QString::fromUtf8(u8"+ 添加规则"));
    m_addBtn->setMinimumHeight(34);
    layout->addWidget(m_addBtn);

    auto* hint = new QLabel(QString::fromUtf8(
        u8"事件触发时按顺序执行动作。可选单次、固定循环，或在事件有效期间持续循环。\n"
        u8"延迟只推迟后续动作，不阻塞瞄准引擎；持续循环会在对应结束事件到来时取消。"));
    // 事件编排:事件触发时按顺序执行动作序列。Delay 仅推迟后续动作,不阻塞引擎。
    hint->setProperty("class", "secondary");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // 承载规则卡的容器
    auto* rulesContainer = new QWidget;
    m_listLayout = new QVBoxLayout(rulesContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(10);
    layout->addWidget(rulesContainer);
    layout->addStretch();

    connect(m_addBtn, &QPushButton::clicked, this, &EventPage::addRule);

    onLoadConfig();
}

void EventPage::onLoadConfig()
{
    m_loading = true;
    rebuildFromRules();
    m_loading = false;
}

void EventPage::rebuildFromRules()
{
    // Clear existing rows
    while (auto* item = m_listLayout->takeAt(0))
    {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }

    auto rules = event_orch::get_rules();
    if (rules.empty())
    {
        // 首次进来:如果 config 里有序列化的,反序列化回来。
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const auto& s : config.event_rules_serialized)
            rules.push_back(event_orch::deserialize_rule(s));
    }

    for (const auto& r : rules)
    {
        auto* card = new RuleCard(r, this);
        connect(card->deleteButton(), &QPushButton::clicked, this, [this, card] {
            m_listLayout->removeWidget(card);
            card->deleteLater();
            saveToConfig();
        });
        m_listLayout->addWidget(card);
    }
}

void EventPage::addRule()
{
    Rule r;
    r.name = u8"新规则";
    auto* card = new RuleCard(r, this);
    connect(card->deleteButton(), &QPushButton::clicked, this, [this, card] {
        m_listLayout->removeWidget(card);
        card->deleteLater();
        saveToConfig();
    });
    m_listLayout->addWidget(card);
    saveToConfig();
}

void EventPage::saveToConfig()
{
    if (m_loading) return;

    std::vector<Rule> rules;
    for (int i = 0; i < m_listLayout->count(); ++i)
    {
        auto* it = m_listLayout->itemAt(i);
        if (!it) continue;
        auto* card = dynamic_cast<RuleCard*>(it->widget());
        if (!card) continue;
        rules.push_back(card->collect());
    }

    // 引擎实时生效
    event_orch::set_rules(rules);

    // 序列化持久化
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.event_rules_serialized.clear();
        for (const auto& r : rules)
            config.event_rules_serialized.push_back(event_orch::serialize_rule(r));
    }
    ConfigBridge::instance().markDirty();
}

// ─── RuleCard inline definitions that depend on EventPage ──────────────────
namespace
{
void RuleCard::wireSignals()
{
    QObject::connect(m_name, &QLineEdit::editingFinished, this, [this] { emitChanged(); });
    QObject::connect(m_enabled, &ToggleSwitch::toggled, this, [this] { emitChanged(); });
    QObject::connect(m_event, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [this] { refreshModeUi(); emitChanged(); });
    QObject::connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [this] { refreshModeUi(); emitChanged(); });
    QObject::connect(m_repeatCount, QOverload<int>::of(&QSpinBox::valueChanged),
                     this, [this] { emitChanged(); });
    QObject::connect(m_repeatInterval, QOverload<int>::of(&QSpinBox::valueChanged),
                     this, [this] { emitChanged(); });
    QObject::connect(m_cooldown, QOverload<int>::of(&QSpinBox::valueChanged),
                     this, [this] { emitChanged(); });
    QObject::connect(m_addActionBtn, &QPushButton::clicked, this, [this] {
        Action a; a.type = ActionType::KeyTap; a.a = 0x51 /*Q*/; a.b = 50;
        appendActionRow(a);
        emitChanged();
    });
}

void RuleCard::emitChanged()
{
    if (m_owner) m_owner->metaObject()->invokeMethod(
        m_owner, "saveToConfig", Qt::QueuedConnection);
}
} // namespace
