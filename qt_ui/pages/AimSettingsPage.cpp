#include "pages/AimSettingsPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>          // QFrame::NoFrame
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>       // QLineEdit::Normal (QInputDialog 参数)
#include <QListWidget>
#include <QListWidgetItem> // 显式包含, 不依赖 QListWidget 的传递包含
#include <QMessageBox>     // 删除热键组的确认框
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>      // showEvent 的参数类型
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>

#include "Apotheosis.h"          // config / configMutex
#include "config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "pages/TargetPage.h"    // setTargetPage(): 取 &TargetPage::classFiltersChanged 需要完整定义
#include "runtime/config_snapshot.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

// 触发按键候选。★ 与旧页一致 —— 这是键盘监听侧真的认得的名字。
namespace
{
struct KeyEntry { const char* id; const char* label; };
const KeyEntry kKeyEntries[] = {
    { "",                 "无 (始终活跃)" },
    { "RightMouseButton", "鼠标右键 (RightMouseButton)" },
    { "X1MouseButton",    "鼠标侧键4 (X1MouseButton)" },
    { "X2MouseButton",    "鼠标侧键5 (X2MouseButton)" },
    { "MiddleMouseButton","鼠标中键 (MiddleMouseButton)" },
    { "LeftMouseButton",  "鼠标左键 (LeftMouseButton)" },
    { nullptr, nullptr }
};

// ★ 稳定读数工具: QDoubleSpinBox 的 valueChanged 会在 setValue 时也触发。
//   载入期间用 m_loading 挡住写回，这是铁律 (a) 的实现手段。
} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 两个视觉小工具（旧页的写法，恢复回来）
// ═══════════════════════════════════════════════════════════════════════════

// ★★ 必须用 setProperty("class", "hint")，不能用 setObjectName("hint")。
//    theme.qss 的选择器是 QLabel[class="hint"]（theme.qss:38），
//    它匹配的是 Qt property 而不是 objectName —— 写成 objectName 时
//    【不匹配任何规则】，4 条提示会静默退化成无样式正文，看起来"页面很丑"。
//    这是从旧页(QStringLiteral)搬过来时最容易丢的一处。
QLabel* AimSettingsPage::makeHint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setProperty("class", "hint");
    return l;
}

// 卡片内分段标题（旧页 BossAim 卡里"跟踪与提前量"/"尺度调度"用的那种）。
// 一组相关参数单独起一段，避免和上一个网格混在一起被当成"又一个参数"。
QLabel* AimSettingsPage::makeSectionTitle(const QString& text)
{
    auto* l = new QLabel(text);
    l->setProperty("class", "heading");
    return l;
}

QWidget* AimSettingsPage::makeDoubleRow(const char* obj, const char* label,
                                        double lo, double hi, double step, double def)
{
    auto* sp = new QDoubleSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setDecimals(3);
    sp->setObjectName(QString::fromUtf8(obj));
    sp->setValue(def);
    m_ctlDoubles.push_back(sp);   // ★ 必须收集, 否则 reload/commit 会漏掉它
    return FormKit::fieldRow(QString::fromUtf8(label), sp);
}

AimSettingsPage::AimSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* left = new QWidget;
    left->setFixedWidth(260);
    buildLeftPanel(left);
    root->addWidget(left);

    auto* right = new QWidget;
    buildRightPanel(right);
    root->addWidget(right, 1);

    // ★★ configLoaded：任何外部改配置（切方案 / 调参 / live_tune）都要重读控件。
    //    铁律 (a) 的落点。漏了它，本页就成了"随时把旧值灌回去的缓存"。
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &AimSettingsPage::reloadFromRuntime);

    rebuildGroupCombo();
    reloadFromRuntime();
}

void AimSettingsPage::setTargetPage(TargetPage* tp)
{
    m_targetPage = tp;
    if (m_targetPage)
        connect(m_targetPage, &TargetPage::classFiltersChanged,
                this, &AimSettingsPage::onTargetClassesChanged);
}

void AimSettingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    reloadFromRuntime();
}

// ═══════════════════════════════════════════════════════════════════════════
// 左栏: 热键方案列表
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::buildLeftPanel(QWidget* parent)
{
    auto* lay = new QVBoxLayout(parent);
    lay->setContentsMargins(12, 14, 6, 12);
    lay->setSpacing(8);

    // ── 热键组 ────────────────────────────────────────────────────────
    auto* groupLabel = new QLabel(QStringLiteral("热键组"));
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
    addGroupBtn->setToolTip(QStringLiteral("新建热键组"));
    groupRow->addWidget(addGroupBtn);

    auto* delGroupBtn = new QPushButton(QStringLiteral("−"));
    delGroupBtn->setFixedSize(28, 28);
    delGroupBtn->setCursor(Qt::PointingHandCursor);
    delGroupBtn->setStyleSheet(smallBtnSS);
    delGroupBtn->setToolTip(QStringLiteral("删除当前热键组"));
    groupRow->addWidget(delGroupBtn);

    lay->addLayout(groupRow);

    connect(m_groupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AimSettingsPage::onGroupChanged);
    connect(addGroupBtn, &QPushButton::clicked, this, &AimSettingsPage::onAddGroup);
    connect(delGroupBtn, &QPushButton::clicked, this, &AimSettingsPage::onDeleteGroup);

    // ── 热键列表 ──────────────────────────────────────────────────────
    auto* header = new QHBoxLayout;
    header->setContentsMargins(4, 6, 4, 0);
    m_leftTitle = new QLabel(QStringLiteral("热键"));
    m_leftTitle->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    header->addWidget(m_leftTitle);
    header->addStretch();
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

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &AimSettingsPage::onProfileSelected);

    auto* row = new QHBoxLayout;
    auto* addBtn = new QPushButton(QStringLiteral("+"));
    auto* delBtn = new QPushButton(QStringLiteral("−"));
    auto* cpyBtn = new QPushButton(QStringLiteral("复制"));
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    row->addWidget(cpyBtn);
    lay->addLayout(row);

    connect(addBtn, &QPushButton::clicked, this, &AimSettingsPage::onAddProfile);
    connect(delBtn, &QPushButton::clicked, this, &AimSettingsPage::onDeleteProfile);
    connect(cpyBtn, &QPushButton::clicked, this, &AimSettingsPage::onCopyProfile);
}

// ═══════════════════════════════════════════════════════════════════════════
// 右栏: 卡片
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::buildRightPanel(QWidget* parent)
{
    auto* outer = new QVBoxLayout(parent);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    m_rightLayout = new QVBoxLayout(content);
    m_rightLayout->setContentsMargins(16, 16, 16, 16);
    m_rightLayout->setSpacing(12);

    buildKeyBindCard();
    buildFovCard();
    buildAimClassCard();
    buildCrosshairCard();
    buildDynamicFovCard();
    buildControllerCard();     // ★★ 当前后端真的在跑的那套参数

    m_rightLayout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
}

// ── 触发按键 ───────────────────────────────────────────────────────────────
void AimSettingsPage::buildKeyBindCard()
{
    auto* card = new CardWidget(QStringLiteral("触发按键"), QStringLiteral("keyboard"));
    auto* cl = card->contentLayout();

    auto* combo = new QComboBox;
    for (int i = 0; kKeyEntries[i].id; ++i)
        combo->addItem(QString::fromUtf8(kKeyEntries[i].label), QString::fromUtf8(kKeyEntries[i].id));
    combo->setObjectName("keyCombo");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("按住此键时瞄准生效"), combo));

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo](int) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            const QString id = combo->currentData().toString();
            config.hotkeys[ri].keys.clear();
            if (!id.isEmpty())
                config.hotkeys[ri].keys.push_back(id.toStdString());
        }
        ConfigBridge::instance().markDirty();
        rebuildProfileList();
    });

    m_rightLayout->addWidget(card);
}

// ── 视野 FOV ───────────────────────────────────────────────────────────────
void AimSettingsPage::buildFovCard()
{
    auto* card = new CardWidget(QStringLiteral("视野 FOV"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    auto* fx = new QSpinBox; fx->setRange(1, 4096); fx->setObjectName("fovX");
    auto* fy = new QSpinBox; fy->setRange(1, 4096); fy->setObjectName("fovY");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("水平直径 (检测像素)"), fx));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("垂直直径 (检测像素)"), fy));

    auto commit = [this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        config.hotkeys[ri].fovX = findChild<QSpinBox*>("fovX")->value();
        config.hotkeys[ri].fovY = findChild<QSpinBox*>("fovY")->value();
        ConfigBridge::instance().markDirty();
    };
    connect(fx, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    connect(fy, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

// ── 瞄准类别 ───────────────────────────────────────────────────────────────
void AimSettingsPage::buildAimClassCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准类别 (优先目标)"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    auto* note = makeHint(QString::fromUtf8(
        u8"这些类别会被控制器当作【可瞄目标】（Aim 桶）。\n"
        u8"★ 顺序无关 —— 控制器按「离准星最近」选，不按列表顺序。\n"
        u8"★ 不在这里、也不在「目标」页设为可见的类别，一律不瞄。"));
    cl->addWidget(note);

    auto* list = new QListWidget;
    list->setObjectName("aimClassList");
    list->setMaximumHeight(120);
    cl->addWidget(list);

    auto* row = new QHBoxLayout;
    auto* combo = new QComboBox; combo->setObjectName("classCombo");
    auto* addBtn = new QPushButton(QStringLiteral("加入"));
    auto* delBtn = new QPushButton(QStringLiteral("移除"));
    row->addWidget(combo, 1);
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    cl->addLayout(row);

    connect(addBtn, &QPushButton::clicked, this, [this, combo]() {
        const int ri = currentRuntimeIndex();
        if (ri < 0 || combo->currentIndex() < 0) return;
        const int cid = combo->currentData().toInt();
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& acs = config.hotkeys[ri].aim_classes;
            const bool exists = std::any_of(acs.begin(), acs.end(),
                [cid](const HotkeyAimClass& a) { return a.class_id == cid; });
            if (!exists)
            {
                HotkeyAimClass a;
                a.class_id = cid;
                a.y_offset = 0.5f;
                a.y_offset_max = 0.5f;
                a.min_conf = 0.0f;
                acs.push_back(a);
            }
        }
        ConfigBridge::instance().markDirty();
        reloadProfileToUi();
    });

    connect(delBtn, &QPushButton::clicked, this, [this, list]() {
        const int ri = currentRuntimeIndex();
        auto* item = list->currentItem();
        if (ri < 0 || !item) return;
        const int cid = item->data(Qt::UserRole).toInt();
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& acs = config.hotkeys[ri].aim_classes;
            acs.erase(std::remove_if(acs.begin(), acs.end(),
                [cid](const HotkeyAimClass& a) { return a.class_id == cid; }), acs.end());
        }
        ConfigBridge::instance().markDirty();
        reloadProfileToUi();
    });

    m_rightLayout->addWidget(card);
}

// ── 准星找色 ───────────────────────────────────────────────────────────────
void AimSettingsPage::buildCrosshairCard()
{
    auto* card = new CardWidget(QStringLiteral("准星找色"), QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    auto* chk = new QCheckBox(QStringLiteral("启用找色（用检测到的准星位置代替画面中心）"));
    chk->setObjectName("crosshairChk");
    cl->addWidget(chk);

    auto* note = makeHint(QString::fromUtf8(
        u8"★ 关：准星 = 画面中心（静态常量）。\n"
        u8"★ 开：用找色结果；找色失效时【退回画面中心】并跳过本拍控制。"));
    cl->addWidget(note);

    connect(chk, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].crosshair_detect_enabled = v;
        }
        ConfigBridge::instance().markDirty();
    });

    m_rightLayout->addWidget(card);
}

// ── 动态 FOV ───────────────────────────────────────────────────────────────
void AimSettingsPage::buildDynamicFovCard()
{
    auto* card = new CardWidget(QStringLiteral("动态 FOV"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    auto* chk = new QCheckBox(QStringLiteral("启用（锁定后收紧瞄准区域，防止别的目标抢锁）"));
    chk->setObjectName("dynFovChk");
    cl->addWidget(chk);

    auto* spin = new QDoubleSpinBox;
    spin->setRange(0.0, 1.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    spin->setObjectName("dynFovStrength");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("收敛强度 (0=不收缩, 1=紧贴目标框)"), spin));

    connect(chk, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_enabled = v;
        }
        ConfigBridge::instance().markDirty();
    });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_strength = static_cast<float>(v);
        }
        ConfigBridge::instance().markDirty();
    });

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// ★★ 通用控制器层卡片 —— 当前后端真的在跑的那套参数
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::buildControllerCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准控制器（通用控制器层）"),
                                QStringLiteral("adjustments"));
    auto* cl = card->contentLayout();

    // ── 总开关 ────────────────────────────────────────────────────────
    auto* enable = new QCheckBox(QStringLiteral("★ 启用控制器（会真的往游戏机发鼠标位移）"));
    enable->setObjectName("ctlEnabled");
    cl->addWidget(enable);

    auto* warn = makeHint(QString::fromUtf8(
        u8"⚠️ 默认关闭。打开后本程序会真的动鼠标 —— 参数未在真机标定过，"
        u8"第一次打开请先把最大位移调小、并准备好随时关掉。"));
    cl->addWidget(warn);

    // ── 六个增益（全部分方向）────────────────────────────────────────
    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"增益（水平 x = 跟枪 / 垂直 y = 压枪）")));

    // ★ 分段: 旧页把"链路结构"的一组参数单独起一段，理由是不这么做
    //   它会被当成"又一个连续量旋钮"混在同一个网格里。这里沿用同一套分段：
    //   增益 / 积分与微分 / 输出与瞄点 / 选靶与稳定器。
    struct D { const char* obj; const char* label; double lo, hi, step, def; };
    const D gains[] = {
        { "ctlKpX",             "Kp · 水平",            0.0, 500.0, 0.5,  35.0  },
        { "ctlKpY",             "Kp · 垂直",            0.0, 500.0, 0.5,  35.0  },
        { "ctlKiX",             "Ki · 水平",            0.0, 100.0, 0.01, 0.0   },
        { "ctlKiY",             "Ki · 垂直",            0.0, 100.0, 0.01, 0.0   },
        { "ctlKdX",             "Kd · 水平",            0.0, 100.0, 0.01, 0.0   },
        { "ctlKdY",             "Kd · 垂直",            0.0, 100.0, 0.01, 0.0   },
        { "ctlPFullScalePx",    "P 项饱和 (像素, 0=不限)", 0.0, 2000.0, 1.0, 0.0 },
        { "ctlTauUnwindSec",    "积分回吐时间常数 (秒)", 0.001, 5.0, 0.005, 0.030 },
        { "ctlTauDerivSec",     "D 项低通时间常数 (秒)", 0.0,   5.0, 0.005, 0.020 },
        { "ctlIMax",            "积分上限 (0=用输出限幅)", 0.0, 5000.0, 1.0, 0.0 },
    };
    for (const D& d : gains)
        cl->addWidget(makeDoubleRow(d.obj, d.label, d.lo, d.hi, d.step, d.def));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"输出与瞄点")));
    const D outAndAnchor[] = {
        { "ctlYOffset",         "瞄点 Y 偏移 (1=框顶, 0=框底)", 0.0, 1.0, 0.05, 0.5 },
        { "ctlYOffsetMax",      "Y 偏移上限（随机抖动用）", 0.0, 1.0, 0.05, 0.5 },
    };
    for (const D& d : outAndAnchor)
        cl->addWidget(makeDoubleRow(d.obj, d.label, d.lo, d.hi, d.step, d.def));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"选靶与稳定器")));
    const D targetAndStab[] = {
        { "ctlHysteresisRatio", "选靶滞回倍数",          1.0, 10.0, 0.05, 1.3 },
        { "ctlMaxDistancePx",   "选靶距离上限 (0=不限)", 0.0, 5000.0, 5.0, 0.0 },
        { "ctlMatchCenterRatio","稳定器·认目标中心系数", 0.001, 10.0, 0.05, 0.5 },
        { "ctlAreaRatioTol",    "稳定器·面积容差倍数",   1.0, 100.0, 0.1, 2.0 },
        { "ctlKSnapMult",       "稳定器·突变系数",       0.001, 100.0, 0.05, 1.15 },
        { "ctlMinAspect",       "稳定器·最小宽高比",     0.001, 100.0, 0.05, 0.2 },
        { "ctlMaxAspect",       "稳定器·最大宽高比",     0.001, 100.0, 0.05, 5.0 },
    };
    for (const D& d : targetAndStab)
        cl->addWidget(makeDoubleRow(d.obj, d.label, d.lo, d.hi, d.step, d.def));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"输出限幅与随机化")));
    const D ints[] = {
        { "ctlMaxOutputCounts", "单拍最大位移 (计数)", 1.0, 1000.0, 1.0, 200.0 },
        { "ctlRandomSeed",      "瞄点随机种子 (0=固定)", 0.0, 999999.0, 1.0, 0.0 },
    };
    for (const D& d : ints)
    {
        auto* sp = new QSpinBox;
        sp->setRange(static_cast<int>(d.lo), static_cast<int>(d.hi));
        sp->setSingleStep(static_cast<int>(d.step));
        sp->setObjectName(QString::fromUtf8(d.obj));
        sp->setValue(static_cast<int>(d.def));
        m_ctlInts.push_back(sp);
        cl->addWidget(FormKit::fieldRow(QString::fromUtf8(d.label), sp));
    }

    // ★ 稳定器 5 项与"认目标"判据是【占位值】—— 必须让用户看见这一点。
    auto* note = makeHint(QString::fromUtf8(
        u8"★ 「稳定器」那 5 项与滞回倍数目前都是【占位值】，没有实测依据，"
        u8"默认值只保证「程序能跑」。\n"
        u8"★ 六个增益默认 Kp=35 / 其余 0，等价于历史单套行为 —— 是安全起点。\n"
        u8"★ 改完立即生效：控制器每拍重读配置，不用重启会话。"));
    cl->addWidget(note);

    // ── 写回 ──────────────────────────────────────────────────────────
    // ★ 一个 lambda 走完全部控件，不逐个 connect —— 那样才不会漏。
    auto commit = [this, enable]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto d = [this](const char* n) { return findChild<QDoubleSpinBox*>(n)->value(); };
        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.ctl_enabled          = enable->isChecked();
        hp.ctl_kp_x             = d("ctlKpX");
        hp.ctl_kp_y             = d("ctlKpY");
        hp.ctl_ki_x             = d("ctlKiX");
        hp.ctl_ki_y             = d("ctlKiY");
        hp.ctl_kd_x             = d("ctlKdX");
        hp.ctl_kd_y             = d("ctlKdY");
        hp.ctl_tau_unwind_sec   = d("ctlTauUnwindSec");
        hp.ctl_tau_deriv_sec    = d("ctlTauDerivSec");
        hp.ctl_i_max            = d("ctlIMax");
        hp.ctl_max_output_counts= i("ctlMaxOutputCounts");
        hp.ctl_p_full_scale_px  = d("ctlPFullScalePx");
        hp.ctl_y_offset         = d("ctlYOffset");
        hp.ctl_y_offset_max     = d("ctlYOffsetMax");
        hp.ctl_hysteresis_ratio = d("ctlHysteresisRatio");
        hp.ctl_max_distance_px  = d("ctlMaxDistancePx");
        hp.ctl_random_seed      = i("ctlRandomSeed");
        hp.ctl_match_center_ratio = d("ctlMatchCenterRatio");
        hp.ctl_area_ratio_tol   = d("ctlAreaRatioTol");
        hp.ctl_k_snap_mult      = d("ctlKSnapMult");
        hp.ctl_min_aspect       = d("ctlMinAspect");
        hp.ctl_max_aspect       = d("ctlMaxAspect");

        ConfigBridge::instance().markDirty();
    };

    connect(enable, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    for (auto* sp : m_ctlDoubles)
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [commit](double) { commit(); });
    for (auto* sp : m_ctlInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// 载入 / 写回
// ═══════════════════════════════════════════════════════════════════════════

int AimSettingsPage::currentRuntimeIndex() const
{
    auto* item = m_profileList ? m_profileList->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

void AimSettingsPage::rebuildGroupCombo()
{
    if (!m_groupCombo) return;
    const QString cur = m_groupCombo->currentText();
    m_groupCombo->blockSignals(true);
    m_groupCombo->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const auto& hp : config.hotkeys)
        {
            const QString g = QString::fromUtf8(hp.group.c_str());
            if (m_groupCombo->findText(g) < 0)
                m_groupCombo->addItem(g);
        }
    }
    const int idx = m_groupCombo->findText(cur);
    if (idx >= 0) m_groupCombo->setCurrentIndex(idx);
    m_groupCombo->blockSignals(false);
}

void AimSettingsPage::rebuildProfileList()
{
    if (!m_profileList) return;
    m_profileList->blockSignals(true);
    m_profileList->clear();

    const QString group = m_groupCombo ? m_groupCombo->currentText() : QString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i)
        {
            const auto& hp = config.hotkeys[i];
            if (QString::fromUtf8(hp.group.c_str()) != group) continue;
            QString keys;
            for (const auto& k : hp.keys)
            {
                if (!keys.isEmpty()) keys += QStringLiteral(" / ");
                keys += QString::fromUtf8(k.c_str());
            }
            auto* item = new QListWidgetItem(
                QStringLiteral("%1\n%2").arg(QString::fromUtf8(hp.name.c_str()),
                                             keys.isEmpty() ? QStringLiteral("(无)") : keys));
            item->setData(Qt::UserRole, i);
            m_profileList->addItem(item);
        }
    }
    m_profileList->blockSignals(false);
    if (m_profileList->count() > 0)
        m_profileList->setCurrentRow(0);
}

void AimSettingsPage::onGroupChanged(int)
{
    rebuildProfileList();
}

void AimSettingsPage::onProfileSelected(int)
{
    reloadProfileToUi();
}

void AimSettingsPage::onTargetClassesChanged()
{
    reloadProfileToUi();
}

// ★★ 铁律 (a) 的核心：整片重读控件。
//    m_loading 守卫让 setValue 不反过来触发写回。
void AimSettingsPage::reloadProfileToUi()
{
    m_loading = true;
    const int ri = currentRuntimeIndex();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= 0 && ri < static_cast<int>(config.hotkeys.size()))
        {
            const HotkeyProfile& hp = config.hotkeys[ri];

            if (auto* c = findChild<QComboBox*>("keyCombo"))
            {
                const QString want = hp.keys.empty() ? QString()
                                                     : QString::fromUtf8(hp.keys.front().c_str());
                const int k = c->findData(want);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            if (auto* s = findChild<QSpinBox*>("fovX")) s->setValue(hp.fovX);
            if (auto* s = findChild<QSpinBox*>("fovY")) s->setValue(hp.fovY);
            if (auto* c = findChild<QCheckBox*>("crosshairChk"))
                c->setChecked(hp.crosshair_detect_enabled);
            if (auto* c = findChild<QCheckBox*>("dynFovChk"))
                c->setChecked(hp.dynamic_fov_enabled);
            if (auto* s = findChild<QDoubleSpinBox*>("dynFovStrength"))
                s->setValue(hp.dynamic_fov_strength);

            // ── ★★ 控制器 22 项 ────────────────────────────────────────
            // ★ 漏一个 = 那个控件永远显示旧值；用户改别的控件时整片写回，
            //   会把参数悄悄改回去。不报错、不留痕 —— HotkeyPage 当年就是这么出事的。
            if (auto* c = findChild<QCheckBox*>("ctlEnabled")) c->setChecked(hp.ctl_enabled);

            auto sd = [this](const char* n, double v) {
                if (auto* s = findChild<QDoubleSpinBox*>(n)) s->setValue(v);
            };
            auto si = [this](const char* n, int v) {
                if (auto* s = findChild<QSpinBox*>(n)) s->setValue(v);
            };
            sd("ctlKpX", hp.ctl_kp_x);
            sd("ctlKpY", hp.ctl_kp_y);
            sd("ctlKiX", hp.ctl_ki_x);
            sd("ctlKiY", hp.ctl_ki_y);
            sd("ctlKdX", hp.ctl_kd_x);
            sd("ctlKdY", hp.ctl_kd_y);
            sd("ctlTauUnwindSec", hp.ctl_tau_unwind_sec);
            sd("ctlTauDerivSec", hp.ctl_tau_deriv_sec);
            sd("ctlIMax", hp.ctl_i_max);
            sd("ctlPFullScalePx", hp.ctl_p_full_scale_px);
            sd("ctlYOffset", hp.ctl_y_offset);
            sd("ctlYOffsetMax", hp.ctl_y_offset_max);
            sd("ctlHysteresisRatio", hp.ctl_hysteresis_ratio);
            sd("ctlMaxDistancePx", hp.ctl_max_distance_px);
            sd("ctlMatchCenterRatio", hp.ctl_match_center_ratio);
            sd("ctlAreaRatioTol", hp.ctl_area_ratio_tol);
            sd("ctlKSnapMult", hp.ctl_k_snap_mult);
            sd("ctlMinAspect", hp.ctl_min_aspect);
            sd("ctlMaxAspect", hp.ctl_max_aspect);
            si("ctlMaxOutputCounts", hp.ctl_max_output_counts);
            si("ctlRandomSeed", hp.ctl_random_seed);

            // 已选类别列表
            if (auto* l = findChild<QListWidget*>("aimClassList"))
            {
                l->clear();
                for (const auto& ac : hp.aim_classes)
                {
                    auto* it = new QListWidgetItem(QString::number(ac.class_id));
                    it->setData(Qt::UserRole, ac.class_id);
                    l->addItem(it);
                }
            }
        }
    }

    // 可选类别下拉：来自全局 class_filters（Target 页维护的那份）
    if (auto* c = findChild<QComboBox*>("classCombo"))
    {
        c->clear();
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const auto& cf : config.class_filters)
        {
            const QString nm = cf.class_name.empty()
                ? QStringLiteral("class_%1").arg(cf.class_id)
                : QString::fromUtf8(cf.class_name.c_str());
            c->addItem(QStringLiteral("[%1] %2").arg(cf.class_id).arg(nm), cf.class_id);
        }
    }

    m_loading = false;
}

void AimSettingsPage::reloadFromRuntime()
{
    rebuildGroupCombo();
    rebuildProfileList();
    reloadProfileToUi();
}

// ═══════════════════════════════════════════════════════════════════════════
// 方案增删改
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::onAddProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建热键"),
        QStringLiteral("名称"), QLineEdit::Normal, QStringLiteral("Aim"), &ok);
    if (!ok || name.isEmpty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        HotkeyProfile hp;
        hp.name = name.toStdString();
        hp.group = m_groupCombo->currentText().toStdString();
        hp.keys = { "RightMouseButton" };
        config.hotkeys.push_back(std::move(hp));
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
}

void AimSettingsPage::onDeleteProfile()
{
    const int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (static_cast<int>(config.hotkeys.size()) <= 1) return;   // 至少留一个
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        config.hotkeys.erase(config.hotkeys.begin() + ri);
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
}

void AimSettingsPage::onCopyProfile()
{
    const int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile copy = config.hotkeys[ri];
        copy.name += " 副本";
        config.hotkeys.push_back(std::move(copy));
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
}

// ═══════════════════════════════════════════════════════════════════════════
// 热键组: 新建 / 删除  (旧页这两个按钮随 HotkeyPage 一起被删掉了)
// ═══════════════════════════════════════════════════════════════════════════
//
// ★ 组就是 HotkeyProfile::group 这个字符串。新建组 = 往 config.hotkeys
//   塞一个属于该组的条目(否则这个组名没有任何条目引用它, 下次
//   rebuildGroupCombo() 就"消失"了)。所以这里刻意建一个占位条目。

void AimSettingsPage::onAddGroup()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, QStringLiteral("新建热键组"),
        QStringLiteral("组名:"), QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    name = name.trimmed();
    if (name.isEmpty()) return;

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        HotkeyProfile hp;
        hp.name  = QStringLiteral("新热键").toStdString();
        hp.group = name.toStdString();
        config.hotkeys.push_back(std::move(hp));
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
    const int idx = m_groupCombo->findText(name);
    if (idx >= 0) m_groupCombo->setCurrentIndex(idx);
}

void AimSettingsPage::onDeleteGroup()
{
    const QString group = m_groupCombo->currentText();
    if (group.isEmpty()) return;

    // ★ 删除是破坏性的(整组热键一起没), 必须确认。
    const auto answer = QMessageBox::question(this, QStringLiteral("删除热键组"),
        QStringLiteral("删除组「%1」及其下所有热键？").arg(group));
    if (answer != QMessageBox::Yes) return;

    const std::string groupStd = group.toStdString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.hotkeys.erase(
            std::remove_if(config.hotkeys.begin(), config.hotkeys.end(),
                [&](const HotkeyProfile& h) { return h.group == groupStd; }),
            config.hotkeys.end());

        // ★ 不能把配置删成空的 —— 控制器/界面都假设至少有一条热键。
        if (config.hotkeys.empty())
        {
            HotkeyProfile hp;
            hp.name  = "Aim";
            hp.group = QStringLiteral("默认").toStdString();
            config.hotkeys.push_back(std::move(hp));
        }
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
}
