#include "pages/HotkeyPage.h"
#include "pages/TargetPage.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "widgets/AdaptiveStack.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"
#include "widgets/TriggerVisualWidget.h"

#include <QShowEvent>
#include <QDropEvent>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/aim_telemetry.h"

// ═══════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════

HotkeyPage::HotkeyPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* leftWidget = new QWidget;
    leftWidget->setFixedWidth(190);
    buildLeftPanel(leftWidget);

    auto* rightWidget = new QWidget;
    buildRightPanel(rightWidget);

    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setSizes({190, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root->addWidget(splitter);

    reloadFromRuntime();

    // 切换全局配置方案后, 分组/热键列表/所有瞄准参数都要按新方案重建。
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &HotkeyPage::reloadFromRuntime);
}

void HotkeyPage::setTargetPage(TargetPage* tp)
{
    m_targetPage = tp;
    if (tp)
        connect(tp, &TargetPage::classFiltersChanged,
                this, &HotkeyPage::onAimClassFiltersChanged);
}

void HotkeyPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // Target 页可能在两次 showEvent 之间改动了 Aim 桶,进本页时兜底刷新一次。
    rebuildAimClassList();
    rebuildAddClassCombo();
}

// 【2026-09-13 删除】refreshCalibration() —— 「每计数像素」的显示刷新
//
// 它读 runtime::calib 的全局量, 把测量进度/拒绝原因/实测 k̂ 显示在界面上。
// 前馈删除后控制器不再消费 k̂, 这整块显示失去意义。
// 详见 docs/aimmagic-comparison.md §6.8。


// ═══════════════════════════════════════════════════════════════════════════
// Left Panel
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildLeftPanel(QWidget* parent)
{
    auto* lay = new QVBoxLayout(parent);
    lay->setContentsMargins(12, 14, 6, 12);
    lay->setSpacing(8);

    auto* groupLabel = new QLabel(QStringLiteral("\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84"));
    groupLabel->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    lay->addWidget(groupLabel);

    auto* groupRow = new QHBoxLayout;
    groupRow->setSpacing(4);
    m_groupCombo = new QComboBox;
    m_groupCombo->setMinimumHeight(30);
    groupRow->addWidget(m_groupCombo, 1);

    const QString smallBtnSS =
        "QPushButton{font-size:16px; color:#71717A; background:transparent;"
        " border:1px solid rgba(0,0,0,0.08); border-radius:4px; padding:0;}"
        "QPushButton:hover{color:#5E6AD2; border-color:#5E6AD2;}";

    auto* addGroupBtn = new QPushButton(QStringLiteral("+"));
    addGroupBtn->setFixedSize(28, 28);
    addGroupBtn->setCursor(Qt::PointingHandCursor);
    addGroupBtn->setStyleSheet(smallBtnSS);
    addGroupBtn->setToolTip(QStringLiteral("\xe6\x96\xb0\xe5\xbb\xba\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84"));
    groupRow->addWidget(addGroupBtn);

    auto* delGroupBtn = new QPushButton(QStringLiteral("\xe2\x88\x92"));
    delGroupBtn->setFixedSize(28, 28);
    delGroupBtn->setCursor(Qt::PointingHandCursor);
    delGroupBtn->setStyleSheet(smallBtnSS);
    delGroupBtn->setToolTip(QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe5\xbd\x93\xe5\x89\x8d\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84"));
    groupRow->addWidget(delGroupBtn);

    lay->addLayout(groupRow);

    connect(addGroupBtn, &QPushButton::clicked, this, &HotkeyPage::onAddGroup);
    connect(delGroupBtn, &QPushButton::clicked, this, &HotkeyPage::onDeleteGroup);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(4, 6, 4, 0);
    m_leftTitle = new QLabel(QStringLiteral("\xe7\x83\xad\xe9\x94\xae"));
    m_leftTitle->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    auto* addBtn = new QPushButton(QString(IconFont::glyph("plus")));
    addBtn->setFixedSize(24, 24);
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setStyleSheet(
        "QPushButton{font-family:\"tabler-icons\"; font-size:16px; color:#71717A;"
        " background:transparent; border:none; padding:0;}"
        "QPushButton:hover{color:#5E6AD2;}");
    header->addWidget(m_leftTitle);
    header->addStretch();
    header->addWidget(addBtn);
    lay->addLayout(header);

    m_profileList = new QListWidget;
    m_profileList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_profileList->setFrameShape(QFrame::NoFrame);
    m_profileList->setStyleSheet(
        "QListWidget{background:transparent; border:none; outline:none; padding:0;}"
        "QListWidget::item{padding:0; margin:0 0 5px 0; border-radius:9px; background:#FFFFFF;"
        " border:1px solid rgba(0,0,0,0.05);}"
        "QListWidget::item:selected{background:#EEF0FC; border:1px solid #EEF0FC;}");
    lay->addWidget(m_profileList, 1);

    connect(m_groupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HotkeyPage::onGroupChanged);
    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &HotkeyPage::onProfileSelected);
    connect(m_profileList, &QListWidget::customContextMenuRequested,
            this, &HotkeyPage::onContextMenu);
    connect(addBtn, &QPushButton::clicked, this, &HotkeyPage::onAddProfile);
}

// ═══════════════════════════════════════════════════════════════════════════
// Right Panel
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildRightPanel(QWidget* parent)
{
    auto* outerLay = new QVBoxLayout(parent);
    outerLay->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    m_rightLayout = new QVBoxLayout(content);
    m_rightLayout->setContentsMargins(16, 16, 16, 16);
    m_rightLayout->setSpacing(12);

    buildKeyBindCard();
    buildFovCard();
    buildTriggerCard();
    buildAimClassCard();
    buildCrosshairCard();
    buildBossAimCard();
    buildTrajectoryCard();

    m_rightLayout->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: 触发按键  (单键下拉选择)
// ═══════════════════════════════════════════════════════════════════════════

struct KeyEntry { const char* id; const char* label; };
static const KeyEntry kKeyEntries[] = {
    {"\xe6\x97\xa0 (\xe5\xa7\x8b\xe7\xb5\x82\xe6\xb4\xbb\xe8\xb7\x83)",  ""},           // 无 (始终活跃)
    {"RightMouseButton",   "\xe9\xbc\xa0\xe6\xa0\x87\xe5\x8f\xb3\xe9\x94\xae (RightMouseButton)"},
    {"X1MouseButton",      "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xbe\xa7\xe9\x94\xae" "4 (X1MouseButton)"},
    {"X2MouseButton",      "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xbe\xa7\xe9\x94\xae" "5 (X2MouseButton)"},
    {"MiddleMouseButton",  "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xb8\xad\xe9\x94\xae (MiddleMouseButton)"},
    {"LeftMouseButton",    "\xe9\xbc\xa0\xe6\xa0\x87\xe5\xb7\xa6\xe9\x94\xae (LeftMouseButton)"},
    {nullptr, nullptr}
};

void HotkeyPage::buildKeyBindCard()
{
    auto* card = new CardWidget(
        QStringLiteral("\xe8\xa7\xa6\xe5\x8f\x91\xe6\x8c\x89\xe9\x94\xae"),
        QStringLiteral("keyboard"));
    auto* cl = card->contentLayout();

    m_keyCombo = new QComboBox;
    for (int i = 0; kKeyEntries[i].id; ++i) {
        // id == label 时只显示 id（字母 / F 键）；否则显示中文标签
        QString label = (kKeyEntries[i].label[0] == '\0')
            ? QString::fromUtf8(kKeyEntries[i].id)   // "无 (始终活跃)" 条目
            : QString::fromUtf8(kKeyEntries[i].label);
        // userData 存 key 名（空字符串 = 无）
        m_keyCombo->addItem(label, QString::fromLatin1(
            i == 0 ? "" : kKeyEntries[i].id));
    }
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe6\x8c\x89\xe9\x94\xae"), m_keyCombo));

    connect(m_keyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: FOV
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildFovCard()
{
    auto* card = new CardWidget(QStringLiteral("\xe8\xa7\x86\xe9\x87\x8e FOV"),
                                QStringLiteral("target"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::sliderRow(QStringLiteral("FOV X"), 10, 640, 106,
                                     m_fovXSlider, m_fovXSpin));
    cl->addWidget(FormKit::sliderRow(QStringLiteral("FOV Y"), 10, 640, 74,
                                     m_fovYSlider, m_fovYSpin));

    cl->addWidget(FormKit::toggleRow(QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe5\x8a\xa8\xe6\x80\x81 FOV"), false, m_dynamicFov));

    m_dynamicFovContainer = new QWidget;
    auto* dynLay = new QVBoxLayout(m_dynamicFovContainer);
    dynLay->setContentsMargins(0, 0, 0, 0);
    dynLay->setSpacing(8);
    QSlider* marginSl = nullptr;
    dynLay->addWidget(FormKit::sliderRowD(QString::fromUtf8(u8"收缩强度"),
                                          0.0, 1.0, 0.60, 0.01, 2, marginSl, m_dynamicFovMargin));
    m_dynamicFovMargin->setToolTip(QString::fromUtf8(
        u8"0 = 始终使用基础 FOV；1 = 靠近已锁目标时最大程度收缩，降低旁侧目标抢锁。"));
    m_dynamicFovContainer->setVisible(false);
    cl->addWidget(m_dynamicFovContainer);
    connect(m_dynamicFov, &ToggleSwitch::toggled,
            m_dynamicFovContainer, &QWidget::setVisible);

    // Write-back: all FOV changes → unified saveUiToCurrentProfile()
    connect(m_fovXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_fovYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_dynamicFov, &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_dynamicFovMargin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: 扳机 (trigger FSM)
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildTriggerCard()
{
    auto* card = new CardWidget(
        QString::fromUtf8(u8"自动扳机"),
        QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(
        QString::fromUtf8(u8"启用自动扳机"),
        false, m_triggerEnabled));

    auto makeSpin = [](int min, int max, const QString& suffix) {
        auto* sp = new QSpinBox;
        sp->setRange(min, max);
        sp->setSuffix(suffix);
        sp->setMinimumHeight(30);
        return sp;
    };

    m_triggerFireDelay      = makeSpin(0,    1000, QStringLiteral(" ms"));
    m_triggerFireDuration   = makeSpin(0,    2000, QStringLiteral(" ms"));
    m_triggerFireInterval   = makeSpin(0,    2000, QStringLiteral(" ms"));
    m_triggerFireDelay->setToolTip(QString::fromUtf8(u8"准星进入命中区后延迟 N ms 才按下, 0 = 立即开火。"));
    m_triggerFireDuration->setToolTip(QString::fromUtf8(
        u8"0 = 长按模式: 进入命中区就按住不松手, 直到准星离开命中区\n"
        u8"(目标真的丢了也会松开), 不会出现按-松的连点。\n"
        u8">0 = 连点模式: 每次按住 N ms 后松手, 再等冷却间隔重按。"));
    m_triggerFireInterval->setToolTip(QString::fromUtf8(
        u8"连点模式的冷却间隔。\n"
        u8"长按模式下用作准星离开命中区后的最短重按间隔 —— 防止在判定\n"
        u8"边缘反复按松形成连点。"));
    m_triggerYPercent       = makeSpin(10,   1000, QStringLiteral(" %"));
    m_triggerDelayJitter    = makeSpin(0,     100, QStringLiteral(" ms"));
    m_triggerDurationJitter = makeSpin(0,     100, QStringLiteral(" ms"));
    m_triggerIntervalJitter = makeSpin(0,     100, QStringLiteral(" ms"));
    m_triggerSwitchCooldown = makeSpin(0,    1000, QStringLiteral(" ms"));

    // 命中判定范围与可视化受击框
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"判定命中范围"),
        m_triggerYPercent));

    m_triggerVisual = new TriggerVisualWidget(card);
    cl->addWidget(m_triggerVisual);

    // 核心时间参数行
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"开火延迟"),
        m_triggerFireDelay));

    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"按住时长 (0 = 长按)"),
        m_triggerFireDuration));

    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"冷却间隔"),
        m_triggerFireInterval));

    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"随机抖动 (防封)"),
        m_triggerDelayJitter));

    // ── 自动开镜 (仿 AimMagic 的「开火方式」) ────────────────────────────────
    m_triggerAutoScope = new QComboBox;
    m_triggerAutoScope->addItems({
        QString::fromUtf8(u8"关闭"),
        QString::fromUtf8(u8"点按右键（只点一下）"),
        QString::fromUtf8(u8"长按右键（按住开镜）"),
    });
    m_triggerAutoScope->setToolTip(QString::fromUtf8(
        u8"开火时自动操作右键开镜（抄 AimMagic 的「开火方式」）。\n"
        u8"• 点按右键：每次接敌开始时点一下右键（按下 → 下一拍抬起），然后就不管了 —— "
        u8"同一次接敌里不会重复点，也不会自动收镜，开镜状态由你自己处理。"
        u8"一次点击跨两拍完成，避免 down/up 挤在同一拍被游戏吞掉。\n"
        u8"• 长按右键：命中区里一直按住，离开时松开 —— 给「按住开镜」的游戏用。\n"
        u8"★ 开镜在按左键【之前】完成，配合「开镜后等待」可以保证第一颗子弹是"
        u8"开着镜打出去的。\n"
        u8"★ 本热键的触发键里如果就有右键（默认就是右键），说明你本来就在按右键"
        u8"开镜，此时自动开镜自动失效，不会把镜切回去。"));
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"自动开镜"),
        m_triggerAutoScope));

    m_triggerScopeDelay = makeSpin(0, 1000, QStringLiteral(" ms"));
    m_triggerScopeDelay->setToolTip(QString::fromUtf8(
        u8"点/按住右键之后，等多久才允许按左键开火（毫秒）。\n"
        u8"• 0 = 同一拍开镜+开火（AM 的默认行为）。\n"
        u8"• 游戏开镜过渡比较慢时填上它：先开镜、等够时间再按左键，"
        u8"保证第一颗子弹吃的是开镜后的准心与灵敏度。\n"
        u8"等待从【点下右键那一刻】算，同一次接敌里离开命中区再回来不会重新计时。\n"
        u8"注意开镜期间准星还在跟，等待不会让瞄点停下来。"));
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"开镜后等待"),
        m_triggerScopeDelay));

    // ── 自动急停 ───────────────────────────────────────────────────────────
    cl->addWidget(FormKit::toggleRow(
        QString::fromUtf8(u8"自动急停（开火时抵消移动）"),
        false, m_triggerAutoStop));
    m_triggerAutoStop->setToolTip(QString::fromUtf8(
        u8"开火那一拍，如果你正按着 WASD，就往盒子里补一个【反方向键】的短按"
        u8"（W→S / S→W / A→D / D→A）。大多数 FPS 引擎里两个相反方向键同时存在"
        u8"就是相互抵消 —— 于是立刻停住，这一枪是站定打出去的。\n"
        u8"不需要松开你手上的键（盒子也做不到），所以是“补键”而不是“抢键”。\n"
        u8"★ 只有输入方式 = MAKCUNEW 才能用（只有它有键盘注入）。用老 MAKCU 时"
        u8"这一项会被自动忽略，全链路日志里会写 auto_stop=unsupported。\n"
        u8"★ 一次只补一个方向键，前/后轴优先。斜向（W+A）只会抵消掉前后轴那一半。\n"
        u8"★ 想让“停稳了再开枪”，把上面的「开火延迟」也设成相近的毫秒数："
        u8"进命中区 → 补反方向键 → 等开火延迟 → 开火。"));

    m_triggerStopMs = makeSpin(20, 300, QStringLiteral(" ms"));
    m_triggerStopMs->setToolTip(QString::fromUtf8(
        u8"反方向键按住多久。太短游戏可能没做完减速，太长会真的往反方向走一段。\n"
        u8"建议 40~80ms（配合开火延迟一起用）。用的是盒子的 KEY_TAP，"
        u8"由固件定时弹起 —— 是自清的，程序出问题也不会把键卡在按下。"));
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"急停时长"),
        m_triggerStopMs));

    connect(m_triggerAutoStop, &ToggleSwitch::toggled,
            this, [this] { saveUiToCurrentProfile(); });

    connect(m_triggerAutoScope, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { saveUiToCurrentProfile(); });

    // 动态联动可视化小部件
    connect(m_triggerYPercent, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                if (m_triggerVisual) m_triggerVisual->setPercent(v);
                saveUiToCurrentProfile();
            });

    connect(m_triggerEnabled, &ToggleSwitch::toggled,
            this, [this] { saveUiToCurrentProfile(); });

    auto wireInt = [this](QSpinBox* sp) {
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
    };
    wireInt(m_triggerFireDelay);
    wireInt(m_triggerFireDuration);
    wireInt(m_triggerFireInterval);
    wireInt(m_triggerDelayJitter);
    wireInt(m_triggerDurationJitter);
    wireInt(m_triggerIntervalJitter);
    wireInt(m_triggerSwitchCooldown);
    wireInt(m_triggerScopeDelay);
    wireInt(m_triggerStopMs);

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: 瞄准类别 (优先级排序 + 每类单独 y_offset / min_conf + 拖动换位)
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildAimClassCard()
{
    m_aimClassCard = new CardWidget(
        QStringLiteral("\xe7\x9e\x84\xe5\x87\x86\xe7\xb1\xbb\xe5\x88\xab (\xe4\xbc\x98\xe5\x85\x88\xe7\xba\xa7\xe6\x8e\x92\xe5\xba\x8f)"),  // 瞄准类别 (优先级排序)
        QStringLiteral("target"));
    auto* cl = m_aimClassCard->contentLayout();

    auto* hint = new QLabel(QString::fromUtf8(
        u8"从「目标类别」页勾选「瞄准」的类别会出现在下方。"
        u8"用 ▲ ▼ 调整优先级（顶部 = 最高），✕ 移除。"));
    hint->setWordWrap(true);
    hint->setProperty("class", "hint");
    cl->addWidget(hint);

    // 「丢失目标缓存」已按需求删除(2026-09-12): 检测丢了就立刻释放, 不留缓存。
    // 运行时固定用 0(见 mouse_thread_loop.cpp), 不再有界面项与配置项。

    // 优先级列表: 普通 QVBoxLayout 承载自定义行卡片。不再用 QListWidget +
    // setItemWidget + InternalMove —— 那套会压扁行 / 横向溢出 / 拖拽后留空行。
    // 换位改由每行的 ▲▼ 按钮完成, 高度天然贴合内容, 由外层页面统一滚动。
    m_aimClassContainer = new QWidget;
    m_aimClassLayout = new QVBoxLayout(m_aimClassContainer);
    m_aimClassLayout->setContentsMargins(0, 0, 0, 0);
    m_aimClassLayout->setSpacing(8);
    cl->addWidget(m_aimClassContainer);

    // "+ 添加" 行
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(6);
    m_addClassCombo = new QComboBox;
    m_addClassCombo->setMinimumWidth(120);
    m_addClassCombo->setMinimumHeight(30);
    addRow->addWidget(m_addClassCombo, 1);

    m_addClassBtn = new QPushButton(QStringLiteral("+ \xe6\xb7\xbb\xe5\x8a\xa0"));  // + 添加
    m_addClassBtn->setFixedHeight(30);
    m_addClassBtn->setCursor(Qt::PointingHandCursor);
    addRow->addWidget(m_addClassBtn);
    cl->addLayout(addRow);

    connect(m_addClassBtn, &QPushButton::clicked, this, [this] {
        int ri = currentRuntimeIndex();
        if (ri < 0) return;
        int classId = m_addClassCombo->currentData().toInt();
        if (classId < 0) return;

        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& ac = config.hotkeys[ri].aim_classes;
            for (const auto& a : ac)
                if (a.class_id == classId) return;   // 已存在
            HotkeyAimClass entry;
            entry.class_id = classId;
            entry.y_offset = 0.65f;   // 1=框顶, 0=框底; 默认锁上半身/头颈
            entry.y_offset_max = 0.65f;
            // 默认预填 AI 页的全局置信度, 让新加类别的显示 = "跟随全局";
            // 用户想收紧就上拉滑条, 拉到 0 → 视作再次退回全局跟随。
            entry.min_conf = static_cast<float>(config.confidence_threshold);
            ac.push_back(entry);
        }

        ConfigBridge::instance().markDirty();
        rebuildAimClassList();
        rebuildAddClassCombo();
    });

    m_rightLayout->addWidget(m_aimClassCard);
}

// ── Target 页把某类别切成 Aim 桶时会调这里,刷新可选下拉+当前列表 ──
void HotkeyPage::onAimClassFiltersChanged()
{
    rebuildAimClassList();
    rebuildAddClassCombo();
}

void HotkeyPage::rebuildAddClassCombo()
{
    if (!m_addClassCombo) return;
    m_addClassCombo->clear();

    int ri = currentRuntimeIndex();
    if (ri < 0) {
        if (m_addClassBtn) m_addClassBtn->setEnabled(false);
        return;
    }

    std::set<int> alreadyAdded;
    std::vector<std::pair<int, std::string>> aimCandidates;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri < static_cast<int>(config.hotkeys.size()))
            for (const auto& a : config.hotkeys[ri].aim_classes)
                alreadyAdded.insert(a.class_id);

        for (const auto& cf : config.class_filters)
            if (cf.bucket == ClassBucket::Aim)
                aimCandidates.emplace_back(cf.class_id, cf.class_name);
    }

    for (auto& [cid, name] : aimCandidates) {
        if (alreadyAdded.count(cid)) continue;
        QString display = name.empty()
            ? QStringLiteral("class_%1").arg(cid)
            : QString::fromUtf8(name.c_str());
        m_addClassCombo->addItem(QStringLiteral("[%1] %2").arg(cid).arg(display), cid);
    }

    if (m_addClassBtn) m_addClassBtn->setEnabled(m_addClassCombo->count() > 0);
}

void HotkeyPage::rebuildAimClassList()
{
    if (!m_aimClassLayout) return;

    // 清空旧行。
    while (m_aimClassLayout->count() > 0) {
        auto* it = m_aimClassLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    int ri = currentRuntimeIndex();
    if (ri < 0) { rebuildAddClassCombo(); return; }

    // 提取随机 Y 范围 + min_conf + 名字,顺便 purge 掉桶已不是 Aim 的旧条目。
    struct Row { int cid; float yMin; float yMax; float c; QString name; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) { rebuildAddClassCombo(); return; }

        std::set<int> aimIds;
        for (const auto& cf : config.class_filters)
            if (cf.bucket == ClassBucket::Aim)
                aimIds.insert(cf.class_id);

        auto& aimClasses = config.hotkeys[ri].aim_classes;
        aimClasses.erase(
            std::remove_if(aimClasses.begin(), aimClasses.end(),
                [&](const HotkeyAimClass& a) { return aimIds.find(a.class_id) == aimIds.end(); }),
            aimClasses.end());

        for (const auto& ac : aimClasses) {
            QString name;
            for (const auto& cf : config.class_filters)
                if (cf.class_id == ac.class_id) {
                    name = cf.class_name.empty()
                        ? QStringLiteral("class_%1").arg(ac.class_id)
                        : QString::fromUtf8(cf.class_name.c_str());
                    break;
                }
            rows.push_back({ ac.class_id, ac.y_offset, ac.y_offset_max,
                             ac.min_conf, name });
        }
    }

    if (rows.empty()) {
        auto* empty = new QLabel(QString::fromUtf8(
            u8"（无瞄准类别 — 先在「目标类别」页把类别切到「瞄准」）"));
        empty->setProperty("class", "hint");
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        empty->setMinimumHeight(36);
        m_aimClassLayout->addWidget(empty);
        rebuildAddClassCombo();
        return;
    }

    // 只读置信标签文本: raw<=0 → "全局"(回退 AI 页阈值), 否则显示 0.00~1.00。
    auto confText = [](int raw) {
        return raw <= 0 ? QString::fromUtf8(u8"全局")
                        : QString::number(raw / 100.0, 'f', 2);
    };

    const int total = static_cast<int>(rows.size());
    for (int idx = 0; idx < total; ++idx) {
        const Row& r = rows[idx];
        const int classId = r.cid;

        auto* rowFrame = new QFrame;
        rowFrame->setObjectName("aimRow");
        rowFrame->setStyleSheet(
            "QFrame#aimRow{background:#FAFAFB; border:1px solid rgba(0,0,0,0.06);"
            " border-radius:8px;}");
        auto* rl = new QVBoxLayout(rowFrame);
        rl->setContentsMargins(12, 8, 10, 10);
        rl->setSpacing(8);

        // ── 第一行: #优先级 + 类名 + 上移 / 下移 / 删除 ──
        auto* top = new QHBoxLayout;
        top->setSpacing(8);

        auto* priLabel = new QLabel(QStringLiteral("#%1").arg(idx + 1));
        priLabel->setFixedWidth(30);
        priLabel->setStyleSheet("color:#5E6AD2; font-size:13px; font-weight:600; border:none;");
        top->addWidget(priLabel);

        auto* nameLabel = new QLabel(QStringLiteral("[%1] %2").arg(r.cid).arg(r.name));
        nameLabel->setStyleSheet("color:#3C3C44; font-size:13px; font-weight:500; border:none;");
        top->addWidget(nameLabel, 1);

        auto makeIconBtn = [](const QString& glyph, const QString& color,
                              const QString& hover, const QString& tip) {
            auto* b = new QPushButton(glyph);
            b->setFixedSize(26, 26);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            b->setStyleSheet(QStringLiteral(
                "QPushButton{color:%1; background:transparent;"
                " border:1px solid rgba(0,0,0,0.08); border-radius:6px;"
                " font-size:13px; padding:0;}"
                "QPushButton:hover{color:%2; border-color:%2;}"
                "QPushButton:disabled{color:#C8C8CE; border-color:rgba(0,0,0,0.05);}")
                .arg(color, hover));
            return b;
        };

        auto* upBtn = makeIconBtn(QString::fromUtf8(u8"▲"), QStringLiteral("#71717A"),
                                  QStringLiteral("#5E6AD2"), QString::fromUtf8(u8"上移（提高优先级）"));
        auto* downBtn = makeIconBtn(QString::fromUtf8(u8"▼"), QStringLiteral("#71717A"),
                                    QStringLiteral("#5E6AD2"), QString::fromUtf8(u8"下移（降低优先级）"));
        auto* delBtn = makeIconBtn(QString::fromUtf8(u8"✕"), QStringLiteral("#D25A5A"),
                                   QStringLiteral("#B83232"), QString::fromUtf8(u8"移除"));
        upBtn->setEnabled(idx > 0);
        downBtn->setEnabled(idx < total - 1);
        top->addWidget(upBtn);
        top->addWidget(downBtn);
        top->addWidget(delBtn);
        rl->addLayout(top);

        // ── 第二行:随机锁点范围。每次新锁定抽一次，锁定期间不重抽。 ──
        auto* rangeRow = new QHBoxLayout;
        rangeRow->setSpacing(8);

        auto makeSlider = [](float v) {
            auto* s = new QSlider(Qt::Horizontal);
            s->setRange(0, 100);
            s->setSingleStep(1);
            s->setPageStep(5);
            s->setValue(std::clamp(static_cast<int>(std::lround(v * 100.0f)), 0, 100));
            s->setMinimumWidth(80);
            return s;
        };
        auto makeOffsetSpin = [](float value) {
            auto* sp = new QDoubleSpinBox;
            sp->setRange(0.0, 1.0);
            sp->setSingleStep(0.01);
            sp->setDecimals(2);
            sp->setValue(value);
            sp->setMinimumHeight(28);
            sp->setMinimumWidth(76);
            return sp;
        };

        auto* yLbl = new QLabel(QString::fromUtf8(u8"随机锁点 Y"));
        yLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
        auto* yMinSpin = makeOffsetSpin(r.yMin);
        auto* yMaxSpin = makeOffsetSpin(r.yMax);
        yMinSpin->setToolTip(QString::fromUtf8(u8"范围下限：1=框顶，0.5=中心，0=框底"));
        yMaxSpin->setToolTip(QString::fromUtf8(u8"范围上限：每次新锁定在上下限之间随机一次"));
        rangeRow->addWidget(yLbl);
        rangeRow->addWidget(yMinSpin);
        rangeRow->addWidget(new QLabel(QString::fromUtf8(u8"—")));
        rangeRow->addWidget(yMaxSpin);
        rangeRow->addStretch();
        rl->addLayout(rangeRow);

        // ── 第三行:最低置信度。 ──
        auto* bottom = new QHBoxLayout;
        bottom->setSpacing(10);
        auto* cLbl = new QLabel(QString::fromUtf8(u8"置信"));
        cLbl->setFixedWidth(32);
        cLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
        auto* cSlider = makeSlider(r.c);
        cSlider->setToolTip(QString::fromUtf8(
            u8"最低置信度：低于此值的框不会夺锁。0 = 跟随 AI 页全局阈值。"));
        auto* cVal = new QLabel(confText(cSlider->value()));
        cVal->setFixedWidth(38);
        cVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cVal->setStyleSheet("color:#3C3C44; font-size:12px; border:none;");
        bottom->addWidget(cLbl);
        bottom->addWidget(cSlider, 1);
        bottom->addWidget(cVal);

        rl->addLayout(bottom);
        m_aimClassLayout->addWidget(rowFrame);

        // ── 回调 ──
        auto persistRange = [this, classId, yMinSpin, yMaxSpin](bool minChanged) {
            if (minChanged && yMinSpin->value() > yMaxSpin->value())
                yMaxSpin->setValue(yMinSpin->value());
            else if (!minChanged && yMaxSpin->value() < yMinSpin->value())
                yMinSpin->setValue(yMaxSpin->value());

            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk2(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId) {
                        a.y_offset = static_cast<float>(yMinSpin->value());
                        a.y_offset_max = static_cast<float>(yMaxSpin->value());
                        break;
                    }
            }
            ConfigBridge::instance().markDirty();
        };
        connect(yMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(true); });
        connect(yMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(false); });

        connect(cSlider, &QSlider::valueChanged, this, [this, classId, cVal, confText](int raw) {
            const float v = static_cast<float>(raw) / 100.0f;
            cVal->setText(confText(raw));
            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk2(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId) { a.min_conf = v; break; }
            }
            ConfigBridge::instance().markDirty();
        });

        connect(upBtn, &QPushButton::clicked, this, [this, idx] { moveAimClass(idx, idx - 1); });
        connect(downBtn, &QPushButton::clicked, this, [this, idx] { moveAimClass(idx, idx + 1); });
        connect(delBtn, &QPushButton::clicked, this, [this, classId] {
            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk2(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                auto& ac2 = config.hotkeys[ri2].aim_classes;
                ac2.erase(std::remove_if(ac2.begin(), ac2.end(),
                    [classId](const HotkeyAimClass& a) { return a.class_id == classId; }),
                    ac2.end());
            }
            ConfigBridge::instance().markDirty();
            rebuildAimClassList();
            rebuildAddClassCombo();
        });
    }

    rebuildAddClassCombo();
}

void HotkeyPage::moveAimClass(int from, int to)
{
    int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        auto& ac = config.hotkeys[ri].aim_classes;
        const int n = static_cast<int>(ac.size());
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
        std::swap(ac[from], ac[to]);
    }
    ConfigBridge::instance().markDirty();
    rebuildAimClassList();
}

// ═══════════════════════════════════════════════════════════════════════════
// Card 4: Crosshair / Laser Detect
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildCrosshairCard()
{
    auto* card = new CardWidget(QStringLiteral("\xe5\x87\x86\xe6\x98\x9f\xe6\x89\xbe\xe8\x89\xb2"),
                                QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe5\x87\x86\xe6\x98\x9f\xe6\x89\xbe\xe8\x89\xb2 (\xe6\xad\xa4\xe7\x83\xad\xe9\x94\xae)"),
        false, m_crosshairDetect));
    connect(m_crosshairDetect,  &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card 5: Boss Aim
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildBossAimCard()
{
    auto* card = new CardWidget(
        QString::fromUtf8(u8"移动锁死瞄准"), QStringLiteral("adjustments"));
    auto* layout = card->contentLayout();

    auto makeDouble = [](double minimum, double maximum, double value,
                         double step, int decimals) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setAlignment(Qt::AlignRight);
        return spin;
    };
    auto makeInt = [](int minimum, int maximum, int value) {
        auto* spin = new QSpinBox;
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setAlignment(Qt::AlignRight);
        return spin;
    };

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    // 槽位语义(新控制器 mouse/aim_pid.h):
    //   [0][1] 瞄准速度 Kp   [2][3] 积分强度 Ki   [4][5] 过冲控制 Kd
    //
    // 2026-09-14: 重新启用【在途自身位移补偿】(beta, 见下方 m_pidfInflight)。
    // 2026-09-13 它曾被删除并注释成"控制器现在是纯反馈" —— 那个结论是错的, 因为
    // 当时它的【实现】把补偿放在像素域、隐含假设 kp = 1/k̂, 于是放大 5 倍并把积分
    // 饿死。改成计数域后 (u -= beta*N/W, 表达式里没有 k̂) 它是 G5 达标的主刹车。
    const double defaults[6] = {35, 35, 1, 1, .00, .00};
    for (int i = 0; i < 6; ++i)
    {
        const int decimals = (i == 0 || i == 1) ? 1 : (i < 4) ? 2 : 3;
        // Kp 计数/(像素*秒); Ki 1/秒; Kd 秒。
        const double maximum = (i < 2) ? 400.0 : (i < 4) ? 60.0 : 0.3;
        m_pidfGain[i] = makeDouble(0.0, maximum, defaults[i], 0.001, decimals);
    }
    for (int i = 0; i < 4; ++i)
        m_pidfInteger[i] = makeInt(0, 1000, 0);

    // ── 在途补偿强度 beta (无量纲, 2026-09-14 新增到界面) ───────────────────
    // 范围 0..3.0: 0 = 关闭(只用于对照实验), 1.6 = 生产默认, 2.0 在 60fps 已发散,
    // 上限 3.0 对应 AimPid 的 kMaxInflightBeta。
    m_pidfInflight[0] = makeDouble(0.0, 3.0, 1.6, 0.05, 2);
    m_pidfInflight[1] = makeDouble(0.0, 3.0, 1.6, 0.05, 2);

    // ── 尺度 s(bbox.height) 增益调度 (mouse/aim_scale.h) ────────────────────
    //
    // ★★ 2026-09-14 改版: 删掉"近处框高/远处框高"两个绝对值输入框 ★★
    //   用户明确指出他【测不出当前框高是多少像素】, 而且在靶场里也无法判断
    //   "这个距离算近还是算远"。换游戏/换分辨率后同一段像素高度对应的距离感
    //   完全不同, 写死两个数换个场景就废。
    //
    //   现在只剩一个基准(base), 且它是【在「自动调参」页按按钮设定】的 ——
    //   见 m_scaleBaseLabel。含义是"标准距离"(h = 基准 时倍率 = 1.0)。
    //   用户一个像素值都不用填。
    m_scaleEnabled = new QCheckBox;
    m_scaleEnabled->setChecked(true);
    m_scaleMax = makeDouble(1.0, 2.0, 1.50, 0.05, 2);
    // 远处下限。★ 允许 < 1.0: 远处目标在画面里的像素速度本来就小(v_px≈f·v_world/d),
    //   按框高等比缩放是正确的距离补偿。默认 1.0 = 不改变现有行为。
    m_scaleMin = makeDouble(0.30, 1.00, 1.00, 0.05, 2);
    // 只读: 显示设定好的基准框高(在「自动调参」页按按钮设置)。不可编辑 —— 用户填不出来。
    m_scaleBaseLabel = new QLabel;

    // 预测补偿的控件 (2026-09-14 重做: 系数范围收紧 + 新增硬上限/噪声门)。
    //   ★ 系数从 ±100 收到 ±0.2: 提前量 = 系数 × 尺寸权重 × 屏幕速度, 想要"死区
    //     46ms 内目标走的距离"需要 0.05~0.2。旧范围会让准星偏出几百到上万像素 ——
    //     正是任务书 §4.2 禁止的"稳态瞄偏随速度线性增长"。
    //   最小/最大预测宽度: 1..2000 px, 默认 20 / 80 (AM 默认值)
    //   方向翻转阻尼: 0..1, 默认 0.25
    m_predictFactor[0] = makeDouble(-0.2, 0.2, 0.0, 0.005, 3);
    m_predictFactor[1] = makeDouble(-0.2, 0.2, 0.0, 0.005, 3);
    m_predictMinW = makeInt(1, 2000, 20);
    m_predictMaxW = makeInt(1, 2000, 80);
    m_predictMinW->setSuffix(QString::fromUtf8(u8" px"));
    m_predictMaxW->setSuffix(QString::fromUtf8(u8" px"));
    m_predictDamp = makeDouble(0.0, 1.0, 0.25, 0.01, 2);
    // 提前量硬上限 (0 = 用内置 12px) 与 速度噪声门
    m_predictMaxPx = makeInt(0, 64, 12);
    m_predictMaxPx->setSuffix(QString::fromUtf8(u8" px"));
    m_predictVelFloor = makeInt(0, 1000, 60);
    m_predictVelFloor->setSuffix(QString::fromUtf8(u8" px/s"));

    // ── PID-EventSync (本档唯一链路; 档位下拉已于 2026-09-16 删除) ──────────
    // ★★ 2026-09-16 二次重写: 跟踪器/预测已按 AimMagic 1.0.30 **逐字移植**
    //    (docs/aimmagic-ground-truth.md)。键集合因此与 AM 的 Group 作用域一一对应:
    //      esync_min_hits / esync_max_age / esync_assoc_iou / esync_vel_sample_ms
    //      esync_pred_factor_x / esync_pred_factor_y / esync_pred_min_w / esync_pred_max_w
    //    删掉的键(它们在 AM 里不存在或已被整条删除的链占用):
    //      esync_assoc_radius_px(AM 没有距离半径) / esync_vel_window_ms(语义不同:
    //      累加窗 vs 持有窗) / esync_counts_per_pixel_x/y / esync_inflight_window_ms /
    //      esync_inflight_beta(AM 的像素域在途链 —— k̂ 双机下测不出来, 整条删除) /
    //      esync_self_motion_gain(并入预测本身, AM 的提前量本来就吃自身位移)
    m_esyncMinHits = makeInt(1, 30, 3);
    m_esyncMaxAge = makeInt(1, 60, 5);
    // AM 的 tracking_iou_threshold: 默认 0.30。
    m_esyncIoU = makeDouble(0.0, 1.0, 0.30, 0.05, 2);
    // AM 的 tracking_velocity_sample_ms: 默认 20, 域 [1, 1000]。
    m_esyncVelSample = makeInt(1, 1000, 20);
    m_esyncVelSample->setSuffix(QString::fromUtf8(u8" ms"));
    // AM 的 prediction_factor_x/y: 无夹取, 默认 0(关闭)。
    m_esyncPredFactorX = makeDouble(-1.0, 1.0, 0.0, 0.05, 2);
    m_esyncPredFactorY = makeDouble(-1.0, 1.0, 0.0, 0.05, 2);
    // AM 的 prediction_min_width / prediction_max_width: 默认 20 / 80。
    m_esyncPredMinW = makeInt(1, 4000, 20);
    m_esyncPredMaxW = makeInt(1, 4000, 80);

    const QString aimSpeedTip = QString::fromUtf8(
        u8"Kp：回路速度，单位 计数/(像素*秒)。唯一与游戏灵敏度挂钩的旋钮 —— 换档次/换游戏"
        u8"时它需要的值差好几倍。手感拖沓就往上加(每次 +50%)，开始抖或绕着目标画圈就退回来。\n"
        u8"★ 追匀速目标时纯 P 回路会有一个固定滞后 = 目标速度 / (Kp x 每计数像素)，"
        u8"但整定完之后剩下的残差主要是【一个采样周期的滞后】(约 0.5 x 目标每拍位移)，"
        u8"它随帧率线性改善、与 Kp 无关 —— 实测把 Kp 从 35 提到 70 残差纹丝不动。\n"
        u8"★ 抖了先退 Kp，再确认「在途补偿强度」没被调成 0；不要靠「移动死区」——死区是主动丢精度。\n"
        u8"★ 临界增益: 死区 46ms 对应每拍增益 g = Kp x dt x 每计数像素 不能超过约 0.26。"
        u8"60fps 下 Kp 约 52 就是上限(按 k̂≈0.593)，120fps 可以更高。");
    const QString integralTip = QString::fromUtf8(
        u8"Ki：积分速率，单位 1/秒，Ti = 1/Ki。负责磨掉匀速移动目标的滞后和残余偏置"
        u8"(“落不到位”就是缺这一项)。调太大会在目标附近来回摆，建议 0.5~4。\n"
        u8"★ 实测它不是越大越好: 300px/s 下 Ki 从 1.0 提到 6.0 反而更差(残差 2.4 -> 17px)，"
        u8"因为滞后地板来自采样周期而不是积分强度。");
    const QString overshootTip = QString::fromUtf8(
        u8"Kd：微分时间，单位 秒，压过冲用。检测框每动一个鼠标计数就是一个台阶，微分会把它"
        u8"看成几十像素/秒的尖峰，所以这个值要小(默认 0.01)，调大反而更容易抖。\n"
        u8"★ 生产默认给 0(关闭) —— 在途补偿已经承担了压过冲的职责，再加微分会放大量化噪声。");
    // leadTip / predictTip 已于 2026-09-13 随「提前量 / 延迟预测」两个输入框一起删除。
    m_pidfGain[0]->setToolTip(aimSpeedTip);
    m_pidfGain[1]->setToolTip(aimSpeedTip);
    m_pidfGain[2]->setToolTip(integralTip);
    m_pidfGain[3]->setToolTip(integralTip);
    m_pidfGain[4]->setToolTip(overshootTip);
    m_pidfGain[5]->setToolTip(overshootTip);
    // 提前量 / 延迟预测 两个输入框已于 2026-09-13 删除(含它们的 leadTip/predictTip)。

    // ── 在途补偿强度 ★ 整个控制器最关键的一个旋钮 ──────────────────────────
    const QString inflightTip = QString::fromUtf8(
        u8"在途补偿强度（无量纲）。作用：把「已经发出去、游戏里已生效、只是画面还没回来」"
        u8"的那批鼠标计数从看到的误差里扣掉，环路于是退化成每拍收缩 1-g 倍，Kp 可以开大"
        u8"而不振荡。\n"
        u8"★★ 0 不是「关掉一个可选优化」，是【拆掉主刹车】—— 实测 400px 甩枪后尾段会变成"
        u8"几百像素的自持极限环(60fps 直接发散)。除非在做对照实验，否则不要填 0。\n"
        u8"★ 推荐 1.6：在 60/120/240/1000fps 四个帧率上尾段都是 0.295px。\n"
        u8"★ 补【不足】只是回到原来的延迟(安全)；补【过头】会把已生效的位移重复扣一遍，"
        u8"变成正反馈发散 —— 2.0 在 60fps 就已经发散，不要往上填。\n"
        u8"★ 表达式全程在计数域，【不含每计数像素 k̂】，所以不存在「标定不准就自激」。");
    m_pidfInflight[0]->setToolTip(inflightTip);
    m_pidfInflight[1]->setToolTip(inflightTip);

    // ── 跟踪器 / PID-EventSync (逐字移植 AimMagic 1.0.30) ────────────────────
    const QString esyncTrackerTip = QString::fromUtf8(
        u8"跟踪器参数。默认值【逐字取自 AimMagic 1.0.30 的 Group 作用域】："
        u8"min_hits=3 / max_age=5 / IoU=0.30 / 速度采样窗=20ms。\n"
        u8"体系：每次推理先做「选靶」，再由跟踪器给这个框一个【跨帧稳定的身份】。"
        u8"身份稳了，提前量才能住在轨迹上、目标闪一下也不用清控制器状态。\n"
        u8"确认命中帧数(min_hits)：`hits` 累计到多少才算「够格输出」。"
        u8"★ 判据是 `min_hits <= hits`，所以 hits 恰好等于它就算够格。"
        u8"★ hits 是【累计】的 —— 漏帧【不清零】，只有轨迹被销毁才归零。\n"
        u8"滑行帧数上限(max_age)：漏帧多少帧后删除轨迹。"
        u8"★ 判据是 `max_age < misses`，所以漏【恰好】 max_age 帧时轨迹【仍然存活】，"
        u8"要到 max_age+1 帧才删 —— 比字面意思多扛一帧。");
    m_esyncMinHits->setToolTip(esyncTrackerTip);
    m_esyncMaxAge->setToolTip(esyncTrackerTip);

    const QString esyncAssocTip = QString::fromUtf8(
        u8"关联门限(IoU)：判断「这一帧的框还是不是同一个目标」。\n"
        u8"★★ 这是逐字移植 AimMagic 的 tracking_iou_threshold，有两条与直觉不同的地方：\n"
        u8"① AIMagic 【没有距离门限】—— 原版那个「关联距离门限(px)」是本项目自己加的，"
        u8"已删除。两个框只要重叠不够就判为新目标，哪怕中心只差 1 像素。\n"
        u8"② 比较用的是【严格大于】：IoU 恰好等于门限时【不算命中】。"
        u8"所以默认 0.30 意味着「重叠必须真的超过 30%」。\n"
        u8"调大的后果：目标高速横穿时相邻两帧重叠变少 → 不停新建轨迹 → 身份乱跳、"
        u8"速度重新开始攒 → 预测永远起不来。调小则容易把两个相邻目标粘成一个。");
    m_esyncIoU->setToolTip(esyncAssocTip);

    const QString esyncVelTip = QString::fromUtf8(
        u8"速度采样窗(ms)：AimMagic 的 tracking_velocity_sample_ms，默认 20。\n"
        u8"★★ 语义是【持有】而不是【累加】：距上次刷新的时间【不到】窗口时，"
        u8"速度直接沿用旧值(而且**不刷新时间戳**，所以窗口会自然到期)。"
        u8"超过窗口才重算，重算值是 `(位移/流逝秒)*0.25 + 旧值*0.75` —— 有 0.75 的惯性。\n"
        u8"★ 这意味着速度是【慢慢逼近】真值的，不是一步到位。而且时间戳只在重算那一支刷新，"
        u8"所以实际间隔会是窗口的好几倍(120fps + 20ms 窗 ⇒ 每 3 帧重算一次)。\n"
        u8"★ 这个量直接决定预测的输入质量：8.3ms 帧间隔下逐帧差分会把 ±0.5px 的框量化噪声"
        u8"放大成几百 px/s 的假速度，喂给预测就是「准星嗡嗡抖」。");
    m_esyncVelSample->setToolTip(esyncVelTip);

    const QString esyncPredTip = QString::fromUtf8(
        u8"提前量系数(AimMagic 的 prediction_factor_x/y)。默认 0 = 关闭。\n"
        u8"★★ 逐字移植后有四处与常见直觉【相反】，请务必先读完：\n"
        u8"① 提前量 = 系数 × 【平台位移】× 尺寸权重 × 状态机系数 ——"
        u8"吃的是【你自己甩枪的速度】，不是目标速度。目标静止时提前量恒为 0。\n"
        u8"② 系数涨落的完整条件是「目标每帧位移 > max(30, 0.8×框高)×(1+1.5×系数)"
        u8"【且】自身每帧位移 > 10px」—— 两个都是【必要条件】，缺一不可。\n"
        u8"③ 涨 0.1/帧、落 0.2/帧，夹在 [0,1]。落比涨快一倍。\n"
        u8"④ 输出还要过一道【重低通】：正常混合 0.05、方向翻转时 0.02。"
        u8"所以提前量是慢慢起来的(时间常数约 20 帧)，不是阶跃。\n"
        u8"★ 尺寸权重吃的是【框高】：h<=下限时权重保持 1.0(不是 0)，"
        u8"h>=上限时才归 0。想让预测生效，框高必须小于下面的上限。");
    m_esyncPredFactorX->setToolTip(esyncPredTip);
    m_esyncPredFactorY->setToolTip(esyncPredTip);

    const QString esyncPredSizeTip = QString::fromUtf8(
        u8"尺寸权重区间(AimMagic 的 prediction_min_width / prediction_max_width，"
        u8"默认 20 / 80)。预测只对「这个尺寸范围内」的目标生效。\n"
        u8"★★ 边界语义容易搞反，原文的形状是：\n"
        u8"  框高 <= 下限  → 权重 = 1.0（【保持满权重】，不是 0）\n"
        u8"  下限 < 框高 < 上限 → 权重 = (上限 − 框高) / (上限 − 下限)\n"
        u8"  框高 >= 上限 → 权重 = 0（提前量被完全吃掉）\n"
        u8"★ 注意权重吃的是【框高】。默认上限 80 意味着框高 80 像素以上的目标"
        u8"完全不预测 —— 想覆盖更大的目标就把上限调大。");
    m_esyncPredMinW->setToolTip(esyncPredSizeTip);
    m_esyncPredMaxW->setToolTip(esyncPredSizeTip);

    // ── 尺度增益调度 ────────────────────────────────────────────────────────
    const QString scaleTip = QString::fromUtf8(
        u8"尺度调度：按检测框高度自动调整等效增益。\n"
        u8"公式： 倍率 = (当前框高 ÷ 基准框高)^γ ，然后夹在【远处下限】与【近处上限】之间。\n"
        u8"基准框高【不用你填】—— 你在靶场整定参数时，程序自动记下那一段的框高中位数，"
        u8"那就是你的标准距离（框高等于它时倍率 = 1.0，行为与你整定时完全一致）。\n"
        u8"框比基准大（更近）→ 倍率上升；比基准小（更远）→ 倍率下降。高度减半则倍率减半。\n"
        u8"★ 上界受临界增益限制：Kp × 近处上限 不能超过约 52(按 k̂≈0.593、60fps)。"
        u8"想让近处更快，要么降 Kp 再提倍数，要么提高帧率。");
    m_scaleEnabled->setToolTip(scaleTip);
    m_scaleMax->setToolTip(QString::fromUtf8(
        u8"近处上限：框高达到基准的若干倍时，增益倍数最多提到这里。1.0 = 关闭尺度调度。\n"
        u8"★ 与 Kp 相乘后不得越过临界增益，见上一行的说明。"));
    m_scaleMin->setToolTip(QString::fromUtf8(
        u8"远处下限：框高比基准小的时候，增益倍数最低降到这里。默认 1.0。\n"
        u8"调低会让远处目标跟得更稳但更迟钝。远处敌人在画面里移动本来就慢，"
        u8"按框高等比缩放是符合几何的，可以放心往下拧。"));
    m_scaleBaseLabel->setToolTip(QString::fromUtf8(
        u8"基准框高：你的参数是在多远的距离上调出来的，就用它当参照（框高等于它时倍率 = 1.0）。\n"
        u8"★ 不是给你填的 —— 你在靶场里看不到检测框的像素高度。\n"
        u8"★ 怎么设置：到「控制 → 自动调参」页，对着你想当标准的那个距离按一下"
        u8"「以当前距离设为基准」按钮。\n"
        u8"显示「未学到」时尺度假定为中性 1.0（等于关闭这一项）。"));
    m_scaleBaseLabel->setStyleSheet(QStringLiteral("QLabel { color:#9aa0a6; }"));

    // 移动限幅: 实测它对追踪能力的影响比任何参数都直接 —— 因为它是硬约束,
    // 限幅不够时目标速度超过"限幅 x 帧率"就根本追不上。
    const QString limitTip = QString::fromUtf8(
        u8"每个控制输出周期最多下发的鼠标计数，0 表示用内置上限(200)。\n"
        u8"限制作用于曲线整形之后；被截掉的位移不会积累成待补发欠账。");
    m_pidfInteger[2]->setToolTip(limitTip);
    m_pidfInteger[3]->setToolTip(limitTip);

    // ── P 项饱和阈值 (原名「移动死区」, 2026-09-14 改名) ─────────────────────
    const QString psatTip = QString::fromUtf8(
        u8"P项饱和阈值，单位 像素。0 = 关闭（默认）。\n"
        u8"含义：|误差| 不超过这个值时 P 项满增益，超过则按 阈值/|误差| 连续衰减"
        u8"（经过原点、没有硬切换）。\n"
        u8"★ 它不是死区。死区会让误差在瞄点周围被永久丢掉，既丢精度又制造输出抖动，"
        u8"已整段删除；这个值是「大误差时限速」，小误差一个像素都不丢。\n"
        u8"★ 实测（400px 甩枪）：它有没有好处【取决于 Kp】——\n"
        u8"   Kp 15/25 时反而让过冲变差（12.1→20.4 / 11.0→21.6）；\n"
        u8"   Kp 35 以上时才变好（43.6→28.2 / 145.6→50.1）。转折点在 Kp≈30。\n"
        u8"   所以生产默认给 0（关闭）：少一个非线性环节，行为更可预测。\n"
        u8"★ 想开就从 60~150 试，别填 10 以下的数——那等于把回路整个变软。");
    m_pidfInteger[0]->setToolTip(psatTip);
    m_pidfInteger[1]->setToolTip(psatTip);

    // ── 预测补偿 (2026-09-13 重做) ──────────────────────────────────────────
    // 对齐 AimMagic 1.0.30「预测补偿」页(逆向报告 §4.2 + 原始 QML 的 UI 语义)。
    // 做法: 把"目标在这一拍会走到哪"算出来, 直接加到瞄点上再送 PID。
    const QString predictFactorTip = QString::fromUtf8(
        u8"预测系数：提前量的力度。提前量 = 系数 × 目标屏幕速度 × 尺寸权重。\n"
        u8"0 = 不预测（默认，和以前完全一样）。\n"
        u8"★ 它解决的是【追移动目标总是差半拍】：指令发出去，画面要过几十毫秒才回来，"
        u8"这段时间里目标已经走掉了，纯靠反馈追就永远落后一截。预测把这截提前补上。\n"
        u8"★★ 范围只有 ±0.2，而且这【不是随便定的】：想要「补上链路死区(46ms)内目标"
        u8"走过的距离」这个物理上刚好合适的量，需要的系数就是 0.05~0.2。\n"
        u8"   旧版本这里给的是 ±100 —— 那个范围下拨到 1.0，300px/s 的目标就会让你提前"
        u8"   瞄出 150 像素（等效 0.5 秒），拨到 100 是 15000 像素。准星会稳定停在目标"
        u8"   前面很远的地方，这就是「瞄偏」。所以范围被收紧了。\n"
        u8"★ 先试 0.1。往上加时注意：如果准星开始【稳定停在目标前面】而不是跟不上，"
        u8"那就说明补过头了，往回收。\n"
        u8"★ 静止目标上它【一个像素都不补】(速度是 0，提前量就是 0)，"
        u8"所以不会影响「远距离拉枪到位停住」这件事。\n"
        u8"★ X/Y 可以分开调（比如只补横向走位、Y 轴留着压枪）。");
    const QString predictCapTip = QString::fromUtf8(
        u8"提前量上限：无论系数和速度多大，提前量也就是准星提前多少像素，"
        u8"最多不超过这个数。默认 12px。\n"
        u8"★ 这是【安全阀】，不是力度旋钮。没有它的话，提前量会随目标速度线性增长 ——"
        u8"目标跑得越快，准星偏得越远，永远不会回到目标身上。\n"
        u8"★ 12px 的依据：46ms 死区内，一个 300px/s 的目标正好走过约 13.8 像素。"
        u8"也就是说「补上一个链路死区」所需的最大提前量就在这个量级。填 0 表示用内置默认。");
    const QString predictVelTip = QString::fromUtf8(
        u8"速度可信门限：目标速度低于这个值时，预测完全不生效（提前量归零）。默认 60px/s。\n"
        u8"★ 为什么必须有：静止目标的「速度」其实是噪声。本项目实测，目标不动时估计出来的"
        u8"速度有 99% 落在 46px/s 以内、最大 52px/s。不设门的话这些噪声会被系数乘成"
        u8"几像素的假提前量，表现就是准星在目标身上【自己嗡嗡抖】。\n"
        u8"★ 60 这个默认值正好挡在噪声上界(52)之上、真实移动速度之下。"
        u8"不要为了「更灵敏」把它调低到 50 以下。");
    const QString predictWidthTip = QString::fromUtf8(
        u8"最小/最大预测宽度：决定【多大的目标才补】。\n"
        u8"框宽小于最小宽度：完全不补；大于最大宽度：也完全不补；中间按线性梯度，"
        u8"目标越小（越远）补得越强。\n"
        u8"★ 为什么远距离要补得更强：目标越远，每帧在画面上的位移越小。40 米外 5 米/秒的"
        u8"横移每帧才动约 0.7 像素，而检测框位置是整数量化的 —— 这么小的位移量不出来，"
        u8"速度估计要么是 0（补不够）要么乱跳。放大正是为了补偿这个。\n"
        u8"★ 反之近距离大框的目标速度估得很准，再补就会冲过头，所以超过最大宽度就不补。\n"
        u8"★ 默认 20 / 80（与 AimMagic 一致）。按你模型框出来的大小调："
        u8"把「典型交战距离下的框宽」放进这个区间里。\n"
        u8"★ 注意这里用的是【框宽】，而上面的「尺度调度」用的是【框高】—— 两者历史来源"
        u8"不同，同时启用时对「远近」的判断会略有差异。");
    const QString predictDampTip = QString::fromUtf8(
        u8"方向翻转阻尼：目标左右横跳时的防抖。\n"
        u8"速度估计在目标变向那一瞬间会反向，提前量跟着从 +X 跳到 -X —— 那是个幅度"
        u8"两倍提前量的突变，看起来就是「准星猛地抽一下」。\n"
        u8"这个值越小，变向时提前量是「滑过去」而不是「跳过去」。\n"
        u8"1 = 不阻尼（直接跳）。默认 0.25。调太小会让变向时补得偏慢。");
    m_predictFactor[0]->setToolTip(predictFactorTip);
    m_predictFactor[1]->setToolTip(predictFactorTip);
    m_predictMinW->setToolTip(predictWidthTip);
    m_predictMaxW->setToolTip(predictWidthTip);
    m_predictDamp->setToolTip(predictDampTip);
    m_predictMaxPx->setToolTip(predictCapTip);
    m_predictVelFloor->setToolTip(predictVelTip);

    // ── 【2026-09-13 删除】「每计数像素」输入框 + 「测量」按钮 + 实测值显示 ────────
    //
    // 删掉的东西: m_pxPerCount[0/1](两个输入框) 、m_measureBtn、
    //   m_measureStatus 、m_measureTimer 、measureRow(按钮+状态那一行) 、以及它们的 tooltip。
    //
    // 为什么: 它们全是为了填/量 k̂(每计数像素)—— 而 k̂ 只有前馈才需要。
    // 前馈已整条移除(见 mouse/aim_pid.h), 现在控制器是纯反馈 PID。
    // 详见 docs/aimmagic-comparison.md §6.8。
    //
    // 现在用户只调 5 个: 下面网格里的追踪增益/积分增益/震荡抑制/移动死区/移动限幅。
    // 框平滑强度(α-β 的 tau)是编译期常数, 故意不暴露——它与 Kp 耦合, 一起调极容易调乱。
    //
    // ⚠ 2026-09-13 修复: 上一次删除时【误删了下面的 fields 数组与 grid->addWidget 循环】,
    //   导致 m_pidfGain/m_pidfInteger 这 10 个控件被创建、被写值、被读值, 但从来没被加进
    //   任何布局 —— 整张「移动锁死瞄准」卡片变成空壳(只剩标题), 用户看不到任何参数。
    //   这就是"前端移动锁死瞄准的参数设置全没了"。教训: 删 UI 块时要确认删掉的范围内
    //   没有【别的控件的布局语句】, 这一步应该用编译 + 实际打开界面验证, 不能只看编译过。
    //
    // 注意: 这里必须是【偶数】个, 才能让 2 列网格对齐。
    // 2026-09-14: 14 个(加入在途补偿强度 X/Y; 「移动死区」改名「P项饱和」)。
    const std::array<std::pair<const char*, QWidget*>, 14> fields = {{
        {u8"瞄准速度 X", m_pidfGain[0]}, {u8"瞄准速度 Y", m_pidfGain[1]},
        {u8"积分强度 X", m_pidfGain[2]}, {u8"积分强度 Y", m_pidfGain[3]},
        {u8"过冲控制 X", m_pidfGain[4]}, {u8"过冲控制 Y", m_pidfGain[5]},
        {u8"在途补偿 X", m_pidfInflight[0]}, {u8"在途补偿 Y", m_pidfInflight[1]},
        {u8"P项饱和 X", m_pidfInteger[0]}, {u8"P项饱和 Y", m_pidfInteger[1]},
        {u8"移动限幅 X", m_pidfInteger[2]}, {u8"移动限幅 Y", m_pidfInteger[3]},
        {u8"预测系数 X", m_predictFactor[0]}, {u8"预测系数 Y", m_predictFactor[1]}
    }};
    for (int i = 0; i < static_cast<int>(fields.size()); ++i)
        grid->addWidget(FormKit::fieldRow(
            QString::fromUtf8(fields[i].first), fields[i].second), i / 2, i % 2);
    // 尺寸区间与阻尼是两轴共用的, 各占一整行(跨两列)。
    int extra_row = static_cast<int>(fields.size()) / 2;
    grid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"最小预测宽度"), m_predictMinW), extra_row, 0);
    grid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"最大预测宽度"), m_predictMaxW), extra_row, 1);
    grid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"方向翻转阻尼"), m_predictDamp), extra_row + 1, 0);
    grid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"提前量上限"), m_predictMaxPx), extra_row + 1, 1);
    grid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"速度可信门限"), m_predictVelFloor), extra_row + 2, 0, 1, 2);
    layout->addLayout(grid);

    // ── PID-EventSync: 跟踪器 + 每轨预测 + 事件驱动消费 ─────────────────────
    // 单独一段: 它是【链路结构】的一组参数(身份怎么粘、提前量住在哪),
    // 和上面那些连续量旋钮不是一回事, 混在同一个网格里会被当成"又一个参数"。
    auto* esyncTitle = new QLabel(QString::fromUtf8(u8"跟踪与提前量（PID-EventSync）"));
    esyncTitle->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(esyncTitle);

    auto* esyncGrid = new QGridLayout;
    esyncGrid->setContentsMargins(0, 0, 0, 0);
    esyncGrid->setHorizontalSpacing(12);
    esyncGrid->setVerticalSpacing(6);
    esyncGrid->setColumnStretch(0, 1);
    esyncGrid->setColumnStretch(1, 1);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"确认命中帧数"), m_esyncMinHits), 0, 0);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"滑行帧数上限"), m_esyncMaxAge), 0, 1);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"关联重叠门限"), m_esyncIoU), 1, 0);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"速度采样窗"), m_esyncVelSample), 1, 1);
    // 预测(AM 的 prediction_factor_x/y + 尺寸权重区间)。
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"提前量系数 X"), m_esyncPredFactorX), 2, 0);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"提前量系数 Y"), m_esyncPredFactorY), 2, 1);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"预测尺寸下限"), m_esyncPredMinW), 3, 0);
    esyncGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"预测尺寸上限"), m_esyncPredMaxW), 3, 1);
    layout->addLayout(esyncGrid);

    // ── 尺度调度 (2026-09-14 新增) ──────────────────────────────────────────
    // 单独一段, 避免和上面的 PID 网格混在一起。
    auto* scaleTitle = new QLabel(QString::fromUtf8(u8"尺度调度（近大远小）"));
    scaleTitle->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(scaleTitle);

    auto* scaleGrid = new QGridLayout;
    scaleGrid->setContentsMargins(0, 0, 0, 0);
    scaleGrid->setHorizontalSpacing(12);
    scaleGrid->setVerticalSpacing(6);
    scaleGrid->setColumnStretch(0, 1);
    scaleGrid->setColumnStretch(1, 1);
    scaleGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"启用"), m_scaleEnabled), 0, 0);
    scaleGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"近处上限"), m_scaleMax), 0, 1);
    scaleGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"远处下限"), m_scaleMin), 1, 0);
    scaleGrid->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"基准框高"), m_scaleBaseLabel), 1, 1);
    layout->addLayout(scaleGrid);

    for (auto* spin : m_pidfGain)
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : m_pidfInteger)
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : m_pidfInflight)
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : m_predictFactor)
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : {static_cast<QWidget*>(m_predictMinW),
                       static_cast<QWidget*>(m_predictMaxW)})
        connect(static_cast<QSpinBox*>(spin), QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_predictDamp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : {m_predictMaxPx, m_predictVelFloor})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    // 尺度调度的控件同样要落盘。
    connect(m_scaleEnabled, &QCheckBox::toggled,
            this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_scaleMax, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_scaleMin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);
    // 跟踪器 / 预测 (2026-09-16 逐字移植后重写)。
    for (auto* spin : {m_esyncMinHits, m_esyncMaxAge, m_esyncVelSample,
                       m_esyncPredMinW, m_esyncPredMaxW})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : {m_esyncIoU, m_esyncPredFactorX, m_esyncPredFactorY})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}


// ═══════════════════════════════════════════════════════════════════════════
// Card 6: 轨迹曲线 (mouse/aim_path.h)
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildTrajectoryCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"轨迹曲线"),
                                QStringLiteral("vector-spline"));
    auto* layout = card->contentLayout();

    m_aimPathMode = new QComboBox;
    m_aimPathMode->addItems({
        QString::fromUtf8(u8"直线（关闭曲线）"),
        QString::fromUtf8(u8"贝塞尔"),
        QString::fromUtf8(u8"自定义手绘"),
        QString::fromUtf8(u8"WindMouse 拟人曲线"),
    });
    m_aimPathMode->setToolTip(QString::fromUtf8(
        u8"在控制器输出之后做轨迹整形。★ 四种模式都只旋转方向、不改变幅值 —— "
        u8"力度仍由上面的 PID 决定，所以开曲线不会和控制器打架。\n"
        u8"WindMouse 是仿 AimMagic 的 enable_mouse_curve：用重力/风力/步长/距离"
        u8"生成一条像人甩出来的路径。"));
    layout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"模式"), m_aimPathMode));

    auto* influenceRow = FormKit::sliderRow(
        QString::fromUtf8(u8"曲线影响"), 0, 100, 25,
        m_aimPathInfluenceSlider, m_aimPathInfluenceSpin, QStringLiteral("%"));
    m_aimPathInfluenceSpin->setToolTip(QString::fromUtf8(
        u8"曲线对 PIDF 主方向的影响比例。0% = 完全透传控制器(等于关曲线)，"
        u8"100% = 完整采用曲线切线。默认 25% —— 曲线只做点缀，不接管移动。"));
    m_aimPathInfluenceSlider->setToolTip(m_aimPathInfluenceSpin->toolTip());
    layout->addWidget(influenceRow);

    // ── WindMouse (mode=3) ──
    m_windMouseContainer = new QWidget;
    auto* windLayout = new QVBoxLayout(m_windMouseContainer);
    windLayout->setContentsMargins(0, 0, 0, 0);
    windLayout->setSpacing(8);

    auto makeDouble = [](double minimum, double maximum, double value,
                         double step, int decimals) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setAlignment(Qt::AlignRight);
        return spin;
    };

    m_windGravity  = makeDouble(0.0, 200.0, 5.0, 0.5, 2);
    m_windWind     = makeDouble(0.0, 200.0, 2.0, 0.5, 2);
    m_windStep     = makeDouble(0.1, 200.0, 10.0, 0.5, 2);
    m_windDistance = makeDouble(0.1, 200.0, 8.0, 0.5, 2);

    const QString gravityTip = QString::fromUtf8(
        u8"重力 G0（像素）：路径朝目标的吸引强度。越大越“坚决”、轨迹越直；"
        u8"越小越飘、越像随手甩。AM 默认 5。");
    const QString windTip = QString::fromUtf8(
        u8"风力 W0（像素）：横向随机游走的幅度。这是“拟人”的主要来源，"
        u8"调大轨迹会明显拐弯/画弧。AM 默认 2。");
    const QString stepTip = QString::fromUtf8(
        u8"步长 M0（像素）：单步最大长度。越小路径越碎、越慢、越像人；"
        u8"太大会变成一笔直冲。AM 默认 10。");
    const QString distanceTip = QString::fromUtf8(
        u8"距离 D0（像素）：进入这个距离后风力开始衰减（越接近目标越稳），"
        u8"保证尾部能收干净。AM 默认 8。");
    m_windGravity->setToolTip(gravityTip);
    m_windWind->setToolTip(windTip);
    m_windStep->setToolTip(stepTip);
    m_windDistance->setToolTip(distanceTip);

    windLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"重力 G0"), m_windGravity));
    windLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"风力 W0"), m_windWind));
    windLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"步长 M0"), m_windStep));
    windLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"距离 D0"), m_windDistance));

    m_windThreshold = new QSpinBox;
    m_windThreshold->setRange(0, 100);
    m_windThreshold->setValue(10);
    m_windThreshold->setSuffix(QString::fromUtf8(u8" px"));
    m_windThreshold->setAlignment(Qt::AlignRight);
    m_windThreshold->setToolTip(QString::fromUtf8(
        u8"门控阈值（AM 的 curve_threshold）：X/Y 两个轴的误差都不超过它时，"
        u8"整段曲线被旁路、直接走直线。\n"
        u8"默认 10px —— 微修正保精度，只有大甩枪才走拟人路径。填 0 = 永远走曲线。"));
    windLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"曲线门控"), m_windThreshold));

    auto* windNote = new QLabel(QString::fromUtf8(
        u8"曲线只改方向不改力度，也不会让命中变慢：门控之内仍是控制器的一拍到位。"
        u8"想更“像人”就加大风力；想更稳就加大重力或调高门控。"));
    windNote->setWordWrap(true);
    windNote->setProperty("class", "tertiary");
    windLayout->addWidget(windNote);

    layout->addWidget(m_windMouseContainer);

    // WindMouse 组只在选中该模式时可见。
    auto syncWindVisibility = [this](int mode) {
        m_windMouseContainer->setVisible(mode == 3);
    };
    syncWindVisibility(m_aimPathMode->currentIndex());

    connect(m_aimPathMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, syncWindVisibility](int mode) {
        syncWindVisibility(mode);
        saveUiToCurrentProfile();
    });
    connect(m_aimPathInfluenceSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
        QSignalBlocker b(m_aimPathInfluenceSlider);
        m_aimPathInfluenceSlider->setValue(v);
        saveUiToCurrentProfile();
    });
    connect(m_aimPathInfluenceSlider, &QSlider::valueChanged,
            this, [this](int v) {
        QSignalBlocker b(m_aimPathInfluenceSpin);
        m_aimPathInfluenceSpin->setValue(v);
        saveUiToCurrentProfile();
    });
    for (auto* spin : {m_windGravity, m_windWind, m_windStep, m_windDistance})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_windThreshold, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Group / Profile Management
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::reloadFromRuntime()
{
    rebuildGroupCombo();

    // ★★★ 必须把控件也按新配置重读一遍 (2026-09-15 修) ★★★
    //
    //   原来这里【只有】 rebuildGroupCombo() —— 它只重建分组下拉和热键列表,
    //   **完全不碰瞄准参数那六个 spinbox**。而本页的写回函数
    //   saveUiToCurrentProfile() 是把【控件当前值】整片写回 config 的:
    //
    //       hp.pidf_kp_x = m_pidfGain[0]->value();
    //       hp.pidf_kp_y = m_pidfGain[1]->value();   // <- Y 用的是陈旧控件值
    //
    //   于是形成一条静默的"陈旧值回灌"链路:
    //     ① 自动调参(或外部 live_tune)改了配置文件, 触发 configLoaded;
    //     ② 本页收到信号只重建下拉, 六个 spinbox 仍显示【旧值】;
    //     ③ 此后用户在本页改【任何】一个别的控件(视野/扳机/曲线...),
    //        都会走 saveUiToCurrentProfile(), 顺手把那六个陈旧值写回去;
    //     ④ 参数看起来"被改回去了", 而且不报错、不留痕。
    //
    //   ★ 实机日志证据 (chain_live.log, 12 条 pid_params):
    //       x=(kp=60.0,...) y=(kp=15.0,...)   反复出现
    //     x 每次都被调参改到新值, 而 **y 永远停在会话开始时的 15.0** ——
    //     因为调参是同时写 x/y 的, 只有"陈旧控件值把 y 盖回去"能解释这个
    //     不对称。用户侧的现象就是"一直没能真正生效"。
    //
    //   ★ 为什么别的页面没这个毛病: TargetPage/SessionPage/HardwarePage/
    //     CrosshairPage/CapturePage/AiModelPage/DebugPage 都在 configLoaded 上
    //     重读控件, 只有本页漏了(注释里写着"所有瞄准参数都要按新方案重建",
    //     但代码没做)。
    reloadProfileFromConfig();
}

// 把 config 里的当前热键档整片重读到控件上。
// ★ 与 loadProfileToUi() 的区别只在"取哪一档": 这里按当前选中的档位取值,
//   并且带 m_loading 守卫 —— loadProfileToUi() 内部会置 m_loading, 期间
//   setValue 触发的 valueChanged 不会反过来写回 config。
void HotkeyPage::reloadProfileFromConfig()
{
    int ri = currentRuntimeIndex();
    if (ri < 0) ri = 0;
    loadProfileToUi(ri);
}

void HotkeyPage::rebuildGroupCombo()
{
    m_groupCombo->blockSignals(true);
    QString prev = m_groupCombo->currentText();
    m_groupCombo->clear();

    std::vector<std::string> groups;
    std::string savedGroup;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const auto& hp : config.hotkeys) {
            const auto& g = hp.group;
            if (std::find(groups.begin(), groups.end(), g) == groups.end())
                groups.push_back(g);
        }
        savedGroup = config.active_hotkey_group;
    }
    if (groups.empty())
        groups.push_back(u8"\xe9\xbb\x98\xe8\xae\xa4");

    for (const auto& g : groups)
        m_groupCombo->addItem(QString::fromUtf8(g.data(), static_cast<int>(g.size())));

    if (prev.isEmpty())
        prev = QString::fromUtf8(savedGroup.data(), static_cast<int>(savedGroup.size()));
    int idx = m_groupCombo->findText(prev);
    m_groupCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_groupCombo->blockSignals(false);

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.active_hotkey_group = m_groupCombo->currentText().toStdString();
    }

    repopulateProfileList();
}

void HotkeyPage::repopulateProfileList()
{
    m_profileList->blockSignals(true);
    m_profileList->clear();

    QString groupFilter = m_groupCombo->currentText();
    std::string groupStd = groupFilter.toStdString();

    std::lock_guard<std::recursive_mutex> lk(configMutex);
    for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i) {
        if (config.hotkeys[i].group == groupStd)
            addProfileItem(i);
    }

    m_profileList->blockSignals(false);
    if (m_profileList->count() > 0) {
        m_profileList->setCurrentRow(0);
        onProfileSelected(0);
    }
}

void HotkeyPage::addProfileItem(int runtimeIndex)
{
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (runtimeIndex >= static_cast<int>(config.hotkeys.size())) return;
    const auto& hp = config.hotkeys[runtimeIndex];

    QString keyStr;
    for (const auto& k : hp.keys) {
        if (!keyStr.isEmpty()) keyStr += QStringLiteral(" / ");
        keyStr += QString::fromUtf8(k.c_str());
    }
    if (keyStr.isEmpty()) keyStr = QStringLiteral("None");

    auto* item = new QListWidgetItem(m_profileList);
    item->setData(Qt::UserRole, runtimeIndex);

    auto* w = new QWidget;
    w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(11, 8, 11, 8);
    v->setSpacing(3);

    auto* nameLbl = new QLabel(QString::fromUtf8(hp.name.c_str()));
    nameLbl->setObjectName("pname");
    auto* keyLbl = new QLabel(keyStr);
    keyLbl->setObjectName("pkey");

    v->addWidget(nameLbl);
    v->addWidget(keyLbl);

    item->setSizeHint(w->sizeHint());
    m_profileList->setItemWidget(item, w);
}

void HotkeyPage::restyleProfileItems()
{
    for (int i = 0; i < m_profileList->count(); ++i) {
        auto* w = m_profileList->itemWidget(m_profileList->item(i));
        if (!w) continue;
        const bool sel = (i == m_profileList->currentRow());
        if (auto* n = w->findChild<QLabel*>("pname"))
            n->setStyleSheet(sel ? "color:#4A55C8; font-size:13px; font-weight:500;"
                                 : "color:#3C3C44; font-size:13px;");
        if (auto* k = w->findChild<QLabel*>("pkey"))
            k->setStyleSheet(sel ? "color:#7E88D8; font-size:11px;"
                                 : "color:#A1A1AA; font-size:11px;");
    }
}

int HotkeyPage::currentRuntimeIndex() const
{
    int row = m_profileList->currentRow();
    if (row < 0 || row >= m_profileList->count()) return -1;
    return m_profileList->item(row)->data(Qt::UserRole).toInt();
}

void HotkeyPage::onGroupChanged(int)
{
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.active_hotkey_group = m_groupCombo->currentText().toStdString();
    }
    ConfigBridge::instance().markDirty();
    repopulateProfileList();
}

void HotkeyPage::onProfileSelected(int row)
{
    restyleProfileItems();
    int ri = currentRuntimeIndex();
    if (ri >= 0) loadProfileToUi(ri);
}

void HotkeyPage::loadProfileToUi(int runtimeIndex)
{
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (runtimeIndex >= static_cast<int>(config.hotkeys.size())) return;
    const auto& hp = config.hotkeys[runtimeIndex];

    m_loading = true;

    m_fovXSpin->setValue(hp.fovX);
    m_fovYSpin->setValue(hp.fovY);

    m_dynamicFov->setChecked(hp.dynamic_fov_enabled);
    m_dynamicFovContainer->setVisible(hp.dynamic_fov_enabled);
    m_dynamicFovMargin->setValue(static_cast<double>(hp.dynamic_fov_strength));

    m_crosshairDetect->setChecked(hp.crosshair_detect_enabled);

    // ── 轨迹曲线 ──
    m_aimPathMode->setCurrentIndex(std::clamp(hp.aim_path_mode, 0, 3));
    m_aimPathInfluenceSpin->setValue(std::clamp(hp.aim_path_influence, 0, 100));
    m_aimPathInfluenceSlider->setValue(std::clamp(hp.aim_path_influence, 0, 100));
    m_windGravity->setValue(static_cast<double>(hp.aim_path_wind_gravity));
    m_windWind->setValue(static_cast<double>(hp.aim_path_wind_wind));
    m_windStep->setValue(static_cast<double>(hp.aim_path_wind_step));
    m_windDistance->setValue(static_cast<double>(hp.aim_path_wind_distance));
    m_windThreshold->setValue(hp.aim_path_wind_threshold);
    if (m_windMouseContainer)
        m_windMouseContainer->setVisible(hp.aim_path_mode == 3);

    const float pg[6] = {hp.pidf_kp_x,hp.pidf_kp_y,hp.pidf_ki_x,hp.pidf_ki_y,hp.pidf_kd_x,
                         hp.pidf_kd_y};
    for (int i=0;i<6;++i) m_pidfGain[i]->setValue(pg[i]);
    // 在途补偿强度 (2026-09-14 恢复读回)。
    // ★ 负值 = "配置里没填", 界面显示生产默认 1.6 —— 与 mouse_thread_loop.cpp 的
    //   pid_params() 回落逻辑一致。若这里显示 0 而运行时也用 1.6, 就会出现
    //   "界面写 0、实际在补偿" 的欺骗性状态。
    const double kInflightUiDefault = 1.6;
    const float ib[2] = {hp.pidf_inflight_x, hp.pidf_inflight_y};
    for (int i=0;i<2;++i)
        m_pidfInflight[i]->setValue(
            (std::isfinite(ib[i]) && ib[i] >= 0.0f)
                ? static_cast<double>(ib[i]) : kInflightUiDefault);
    // (「每计数像素」输入框已删除: 前馈移除后它不再影响任何行为。)
    const int pi[4] = {hp.pidf_psat_x,hp.pidf_psat_y,
                       hp.pidf_limit_x,hp.pidf_limit_y};
    for (int i=0;i<4;++i) m_pidfInteger[i]->setValue(pi[i]);

    // 尺度调度 (2026-09-14; 同日改为"单基准"设计)
    m_scaleEnabled->setChecked(hp.aim_scale_enabled != 0);
    m_scaleMax->setValue(static_cast<double>(hp.aim_scale_max));
    m_scaleMin->setValue(static_cast<double>(hp.aim_scale_min));
    // 基准框高是【只读显示】—— 用户看不到检测框像素高度, 填不出来。
    // ★ 设置入口在「控制 → 自动调参」页的按钮上(与 agent 开关无关)。
    if (hp.aim_scale_base_h > 0.0f)
        m_scaleBaseLabel->setText(
            QString::fromUtf8(u8"%1 px（在自动调参页设置）")
                .arg(static_cast<double>(hp.aim_scale_base_h), 0, 'f', 1));
    else
        m_scaleBaseLabel->setText(
            QString::fromUtf8(u8"未学到 —— 去「自动调参」页按一下按钮"));

    // 预测补偿 (2026-09-13 重做, 对齐 AimMagic 1.0.30)
    m_predictFactor[0]->setValue(static_cast<double>(hp.pidf_predict_x));
    m_predictFactor[1]->setValue(static_cast<double>(hp.pidf_predict_y));
    m_predictMinW->setValue(hp.pidf_predict_min_w);
    m_predictMaxW->setValue(hp.pidf_predict_max_w);
    m_predictDamp->setValue(static_cast<double>(hp.pidf_predict_damp));
    m_predictMaxPx->setValue(hp.pidf_predict_max_px);
    m_predictVelFloor->setValue(hp.pidf_predict_vel_floor);

    // 跟踪器 / 预测 (2026-09-16; 档位下拉已于 2026-09-16 删除)
    m_esyncMinHits->setValue(hp.esync_min_hits);
    m_esyncMaxAge->setValue(hp.esync_max_age);
    m_esyncIoU->setValue(static_cast<double>(hp.esync_assoc_iou));
    m_esyncVelSample->setValue(hp.esync_vel_sample_ms);
    m_esyncPredFactorX->setValue(static_cast<double>(hp.esync_pred_factor_x));
    m_esyncPredFactorY->setValue(static_cast<double>(hp.esync_pred_factor_y));
    m_esyncPredMinW->setValue(hp.esync_pred_min_w);
    m_esyncPredMaxW->setValue(hp.esync_pred_max_w);

    // ── Trigger ──
    m_triggerEnabled->setChecked(hp.trigger_enabled);
    m_triggerFireDelay->setValue(hp.trigger_fire_delay);
    m_triggerFireDuration->setValue(hp.trigger_fire_duration);
    m_triggerFireInterval->setValue(hp.trigger_fire_interval);
    m_triggerYPercent->setValue(hp.trigger_y_percent);
    if (m_triggerVisual) m_triggerVisual->setPercent(hp.trigger_y_percent);
    m_triggerDelayJitter->setValue(hp.trigger_delay_jitter_ms);
    m_triggerDurationJitter->setValue(hp.trigger_duration_jitter_ms);
    m_triggerIntervalJitter->setValue(hp.trigger_interval_jitter_ms);
    m_triggerSwitchCooldown->setValue(hp.trigger_switch_cooldown_ms);
    // 自动开镜 (0 关 / 1 点按右键 / 2 长按右键)
    m_triggerAutoScope->setCurrentIndex(std::clamp(hp.trigger_auto_scope, 0, 2));
    m_triggerScopeDelay->setValue(std::clamp(hp.trigger_scope_delay_ms, 0, 1000));
    // 自动急停
    m_triggerAutoStop->setChecked(hp.trigger_auto_stop > 0);
    m_triggerStopMs->setValue(std::clamp(hp.trigger_stop_ms, 20, 300));


    // 通过 rebuildAimClassList() 重建 (需要读 config)。loadProfileToUi 已经
    // 持有 configMutex, 而 rebuildAimClassList 会重新拿一次同一把递归锁,
    // 这里直接调没问题。
    rebuildAimClassList();


    if (m_keyCombo) {
        m_keyCombo->blockSignals(true);
        if (hp.keys.empty()) {
            m_keyCombo->setCurrentIndex(0);
        } else {
            int ki = m_keyCombo->findData(QString::fromStdString(hp.keys[0]));
            m_keyCombo->setCurrentIndex(ki >= 0 ? ki : 0);
        }
        m_keyCombo->blockSignals(false);
    }

    m_loading = false;
}

void HotkeyPage::saveUiToCurrentProfile()
{
    if (m_loading) return;
    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (ri >= static_cast<int>(config.hotkeys.size())) return;
    auto& hp = config.hotkeys[ri];

    hp.fovX = m_fovXSpin->value();
    hp.fovY = m_fovYSpin->value();
    hp.dynamic_fov_enabled = m_dynamicFov->isChecked();
    hp.dynamic_fov_strength = static_cast<float>(m_dynamicFovMargin->value());
    hp.crosshair_detect_enabled  = m_crosshairDetect->isChecked();
    // ── 轨迹曲线 ──
    hp.aim_path_mode = std::clamp(m_aimPathMode->currentIndex(), 0, 3);
    hp.aim_path_influence = std::clamp(m_aimPathInfluenceSpin->value(), 0, 100);
    hp.aim_path_wind_gravity   = static_cast<float>(m_windGravity->value());
    hp.aim_path_wind_wind      = static_cast<float>(m_windWind->value());
    hp.aim_path_wind_step      = static_cast<float>(m_windStep->value());
    hp.aim_path_wind_distance  = static_cast<float>(m_windDistance->value());
    hp.aim_path_wind_threshold = m_windThreshold->value();
    hp.pidf_kp_x=m_pidfGain[0]->value(); hp.pidf_kp_y=m_pidfGain[1]->value();
    hp.pidf_ki_x=m_pidfGain[2]->value(); hp.pidf_ki_y=m_pidfGain[3]->value();
    hp.pidf_kd_x=m_pidfGain[4]->value(); hp.pidf_kd_y=m_pidfGain[5]->value();
    // 在途补偿强度 (2026-09-14 恢复写入)。0 是合法值(关闭), 所以这里直接写控件值;
    // "用默认 1.6" 只由【配置文件里没有这个键】或负值来表达。
    hp.pidf_inflight_x=static_cast<float>(m_pidfInflight[0]->value());
    hp.pidf_inflight_y=static_cast<float>(m_pidfInflight[1]->value());
    // ★ aim_px_per_count_x/y (每计数像素 k̂) 已整条删除, 这里不再写入。
    //   保留那个键会诱导后来的人重新把 k̂ 接回控制器, 而它是游戏机的属性、无法测量。
    hp.pidf_psat_x=m_pidfInteger[0]->value(); hp.pidf_psat_y=m_pidfInteger[1]->value();
    hp.pidf_limit_x=m_pidfInteger[2]->value(); hp.pidf_limit_y=m_pidfInteger[3]->value();
    // 尺度调度 (2026-09-14; 同日改为"单基准"设计)。
    // 关闭时把两端都写 1.0 —— 这样"关掉"等价于"中性", 配置文件里也一眼看得出
    // 是关的, 不需要额外加一个开关键。
    hp.aim_scale_enabled = m_scaleEnabled->isChecked() ? 1 : 0;
    hp.aim_scale_max = static_cast<float>(m_scaleEnabled->isChecked()
        ? m_scaleMax->value() : 1.0);
    hp.aim_scale_min = static_cast<float>(m_scaleEnabled->isChecked()
        ? m_scaleMin->value() : 1.0);
    // ★ aim_scale_base_h 【不在这里写】—— 它在「自动调参」页由用户按按钮设定
    //   并单独落盘(Runtime::set_baseline_from_recent)。这一页只读显示。
    //   这里如果写就会把它清掉(用户每次改任何参数都会触发落盘)。
    // 预测补偿 (2026-09-14): 硬上限与速度门。
    hp.pidf_predict_max_px = m_predictMaxPx->value();
    hp.pidf_predict_vel_floor = m_predictVelFloor->value();
    // 预测补偿 (2026-09-13 重做, 对齐 AimMagic 1.0.30)
    hp.pidf_predict_x=static_cast<float>(m_predictFactor[0]->value());
    hp.pidf_predict_y=static_cast<float>(m_predictFactor[1]->value());
    hp.pidf_predict_min_w=m_predictMinW->value();
    hp.pidf_predict_max_w=m_predictMaxW->value();
    hp.pidf_predict_damp=static_cast<float>(m_predictDamp->value());

    // 跟踪器 / 预测 (2026-09-16 逐字移植后重写)
    hp.esync_min_hits = m_esyncMinHits->value();
    hp.esync_max_age = m_esyncMaxAge->value();
    hp.esync_assoc_iou = static_cast<float>(m_esyncIoU->value());
    hp.esync_vel_sample_ms = m_esyncVelSample->value();
    hp.esync_pred_factor_x = static_cast<float>(m_esyncPredFactorX->value());
    hp.esync_pred_factor_y = static_cast<float>(m_esyncPredFactorY->value());
    hp.esync_pred_min_w = m_esyncPredMinW->value();
    hp.esync_pred_max_w = m_esyncPredMaxW->value();

    // ── Trigger ──
    hp.trigger_enabled       = m_triggerEnabled->isChecked();
    hp.trigger_fire_delay    = m_triggerFireDelay->value();
    hp.trigger_fire_duration = m_triggerFireDuration->value();
    hp.trigger_fire_interval = m_triggerFireInterval->value();
    hp.trigger_y_percent     = m_triggerYPercent->value();
    hp.trigger_delay_jitter_ms    = m_triggerDelayJitter->value();
    hp.trigger_duration_jitter_ms = m_triggerDurationJitter->value();
    hp.trigger_interval_jitter_ms = m_triggerIntervalJitter->value();
    hp.trigger_switch_cooldown_ms = m_triggerSwitchCooldown->value();
    // 自动开镜 (仿 AimMagic 的「开火方式」)
    hp.trigger_auto_scope     = std::clamp(m_triggerAutoScope->currentIndex(), 0, 2);
    hp.trigger_scope_delay_ms = std::clamp(m_triggerScopeDelay->value(), 0, 1000);
    // 自动急停 (只有 MAKCUNEW 有键盘通道)
    hp.trigger_auto_stop = m_triggerAutoStop->isChecked() ? 1 : 0;
    hp.trigger_stop_ms   = std::clamp(m_triggerStopMs->value(), 20, 300);


    // 目标选择: 每条目的 class_id / y_offset / min_conf 由行内滑条回调直接写入
    // config.hotkeys[ri].aim_classes, 顺序由 ▲▼ 的 moveAimClass 维护
    // (见 rebuildAimClassList / moveAimClass), 此处无需再写。

    // Key binding
    if (m_keyCombo) {
        QString key = m_keyCombo->currentData().toString();
        hp.keys.clear();
        if (!key.isEmpty())
            hp.keys.push_back(key.toStdString());
    }

    ConfigBridge::instance().markDirty();
}

// ═══════════════════════════════════════════════════════════════════════════
// Profile Add / Delete / Copy / Rename
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::onAddProfile()
{
    QString currentGroup = m_groupCombo->currentText();

    HotkeyProfile hp;
    hp.name = QStringLiteral("\xe6\x96\xb0\xe7\x83\xad\xe9\x94\xae").toStdString();
    hp.group = currentGroup.toStdString();

    int newIdx;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.hotkeys.push_back(hp);
        newIdx = static_cast<int>(config.hotkeys.size()) - 1;
    }
    ConfigBridge::instance().markDirty();

    repopulateProfileList();
    for (int i = 0; i < m_profileList->count(); ++i) {
        if (m_profileList->item(i)->data(Qt::UserRole).toInt() == newIdx) {
            m_profileList->setCurrentRow(i);
            break;
        }
    }
}

void HotkeyPage::onDeleteProfile()
{
    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    auto answer = QMessageBox::question(this,
        QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe7\x83\xad\xe9\x94\xae"),
        QStringLiteral("\xe7\xa1\xae\xe5\xae\x9a\xe5\x88\xa0\xe9\x99\xa4\xef\xbc\x9f"));
    if (answer != QMessageBox::Yes) return;

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri < static_cast<int>(config.hotkeys.size()))
            config.hotkeys.erase(config.hotkeys.begin() + ri);
    }
    ConfigBridge::instance().markDirty();
    repopulateProfileList();
}

void HotkeyPage::onCopyProfile()
{
    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    int newIdx;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile copy = config.hotkeys[ri];
        copy.name += " (\xe5\x89\xaf\xe6\x9c\xac)";
        config.hotkeys.push_back(copy);
        newIdx = static_cast<int>(config.hotkeys.size()) - 1;
    }
    ConfigBridge::instance().markDirty();

    repopulateProfileList();
    for (int i = 0; i < m_profileList->count(); ++i) {
        if (m_profileList->item(i)->data(Qt::UserRole).toInt() == newIdx) {
            m_profileList->setCurrentRow(i);
            break;
        }
    }
}

void HotkeyPage::onAddGroup()
{
    bool ok = false;
    QString name = QInputDialog::getText(this,
        QStringLiteral("\xe6\x96\xb0\xe5\xbb\xba\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84"),
        QStringLiteral("\xe7\xbb\x84\xe5\x90\x8d:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    HotkeyProfile hp;
    hp.name = QStringLiteral("\xe6\x96\xb0\xe7\x83\xad\xe9\x94\xae").toStdString();
    hp.group = name.toStdString();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.hotkeys.push_back(hp);
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
    int idx = m_groupCombo->findText(name);
    if (idx >= 0) m_groupCombo->setCurrentIndex(idx);
}

void HotkeyPage::onDeleteGroup()
{
    QString group = m_groupCombo->currentText();
    if (group.isEmpty()) return;

    auto answer = QMessageBox::question(this,
        QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe7\x83\xad\xe9\x94\xae\xe7\xbb\x84"),
        QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4\xe7\xbb\x84\xe2\x80\x9c%1\xe2\x80\x9d\xe5\x8f\x8a\xe5\x85\xb6\xe4\xb8\x8b\xe6\x89\x80\xe6\x9c\x89\xe7\x83\xad\xe9\x94\xae\xef\xbc\x9f").arg(group));
    if (answer != QMessageBox::Yes) return;

    std::string groupStd = group.toStdString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.hotkeys.erase(
            std::remove_if(config.hotkeys.begin(), config.hotkeys.end(),
                [&](const HotkeyProfile& h) { return h.group == groupStd; }),
            config.hotkeys.end());

        if (config.hotkeys.empty()) {
            HotkeyProfile hp;
            hp.name = "Aim";
            hp.group = u8"\xe9\xbb\x98\xe8\xae\xa4";
            config.hotkeys.push_back(hp);
        }
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
}

void HotkeyPage::onContextMenu(const QPoint& pos)
{
    if (!m_profileList->itemAt(pos)) return;

    QMenu menu;
    auto* renameAct = menu.addAction(QStringLiteral("\xe9\x87\x8d\xe5\x91\xbd\xe5\x90\x8d"));
    auto* copyAct = menu.addAction(QStringLiteral("\xe5\xa4\x8d\xe5\x88\xb6"));
    auto* deleteAct = menu.addAction(QStringLiteral("\xe5\x88\xa0\xe9\x99\xa4"));

    auto* chosen = menu.exec(m_profileList->viewport()->mapToGlobal(pos));
    if (chosen == renameAct) onRenameProfile();
    else if (chosen == copyAct) onCopyProfile();
    else if (chosen == deleteAct) onDeleteProfile();
}

void HotkeyPage::onRenameProfile()
{
    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    QString current;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        current = QString::fromUtf8(config.hotkeys[ri].name.c_str());
    }

    bool ok = false;
    auto name = QInputDialog::getText(this,
        QStringLiteral("\xe9\x87\x8d\xe5\x91\xbd\xe5\x90\x8d"),
        QStringLiteral("\xe6\x96\xb0\xe5\x90\x8d\xe7\xa7\xb0:"),
        QLineEdit::Normal, current, &ok);
    if (!ok || name.isEmpty()) return;

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri < static_cast<int>(config.hotkeys.size()))
            config.hotkeys[ri].name = name.toStdString();
    }
    ConfigBridge::instance().markDirty();
    repopulateProfileList();
}
