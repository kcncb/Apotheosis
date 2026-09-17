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
#include <QSplitter>       // 左栏/右栏可拖动分隔（旧页的写法）
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

// ★★ 悬停说明（2026-09-17）: 用户要求每个参数都能"停留看到说明"。
//
// 做法: tip 同时挂到【整行 widget】和【行内每一个子控件】上。
//   只挂行 widget 是不够的 —— 鼠标停在 QSpinBox/QComboBox 这些子控件上时,
//   事件由子控件接收, 父 widget 的 tooltip 不会弹出。所以必须递归挂。
void AimSettingsPage::attachTip(QWidget* row, const QString& tip)
{
    if (!row || tip.isEmpty()) return;
    row->setToolTip(tip);
    const auto kids = row->findChildren<QWidget*>();
    for (QWidget* w : kids)
        w->setToolTip(tip);
}

QWidget* AimSettingsPage::makeDoubleRowTip(const char* obj, const char* label,
                                           double lo, double hi, double step,
                                           double def, const QString& tip)
{
    auto* row = makeDoubleRow(obj, label, lo, hi, step, def);
    attachTip(row, tip);
    return row;
}

QWidget* AimSettingsPage::makeIntRow(const char* obj, const char* label, int lo, int hi,
                                     int step, int def, const QString& tip)
{
    // ★ 按 objectName 前缀分派到【正确的收集器】——
    //   收集错了会让某张卡的 commit lambda 去写不属于它的控件
    //   (findChild 找不到就崩, 或者读到另一张卡的值)。
    std::vector<QSpinBox*>* sink = &m_ctlInts;
    const QString name = QString::fromUtf8(obj);
    if (name.startsWith(QLatin1String("trigger")))      sink = &m_triggerInts;
    else if (name.startsWith(QLatin1String("wind")) ||
             name.startsWith(QLatin1String("aimPath"))) sink = &m_pathInts;

    auto* sp = new QSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setObjectName(name);
    sp->setValue(def);
    sink->push_back(sp);
    auto* row = FormKit::fieldRow(QString::fromUtf8(label), sp);
    attachTip(row, tip);
    return row;
}

AimSettingsPage::AimSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ★★ 左栏宽度与分割方式按旧页 (HotkeyPage) 恢复：
    //    · 旧页是 190px，新页写成了 260px —— 宽了 37%，这就是"热键组那一列过宽"。
    //    · 旧页用 QSplitter（可拖动分隔条），新页是普通固定宽 QWidget ⇒ 拖不动。
    //    setStretchFactor(0,0)/(1,1) 保证拉窗口时宽度只给右栏。
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* left = new QWidget;
    left->setFixedWidth(190);
    buildLeftPanel(left);

    auto* right = new QWidget;
    buildRightPanel(right);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setSizes({190, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root->addWidget(splitter);

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
    buildTriggerCard();        // ★ 自动扳机（2026-09-17 恢复）
    buildTrajectoryCard();     // ★ 轨迹曲线 / 风力曲线（2026-09-17 恢复）

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
// ★★ 2026-09-17 第四轮续: 按【旧页外观】重建，并把逐类参数真的接进后端。
//
//   改之前这里是一个 QListWidget + 「加入/移除」两个按钮 —— 用户明确说
//   「还是喜欢原来的」。旧页的形态是每类一张行卡片，行内有：
//     #优先级 + [id] 类名 + ▲▼✕  +  「随机锁点 Y」双 spin + 「置信」滑块
//   那些逐类参数（y_offset / y_offset_max / min_conf）本来就在 config 里
//   存着，重建时只是没接后端 —— 现在接上了（见 control/aim_controller.cpp
//   的 classAimPoints 查表 与 control/selector.cpp 的逐类置信度门槛）。
void AimSettingsPage::buildAimClassCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准类别 (优先级排序)"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    cl->addWidget(makeHint(QString::fromUtf8(
        u8"从「目标类别」页勾选「瞄准」的类别会出现在下方。"
        u8"用 ▲ ▼ 调整优先级（顶部 = 最高），✕ 移除。")));

    // ★ 优先级列表：普通 QWidget + QVBoxLayout 承载自定义行卡片。
    //   旧页注释明确记着不用 QListWidget + setItemWidget + InternalMove ——
    //   那套会压扁行 / 横向溢出 / 拖拽后留空行。换位改由每行的 ▲▼ 完成，
    //   高度天然贴合内容，由外层页面统一滚动。
    m_aimClassContainer = new QWidget;
    m_aimClassLayout = new QVBoxLayout(m_aimClassContainer);
    m_aimClassLayout->setContentsMargins(0, 0, 0, 0);
    m_aimClassLayout->setSpacing(8);
    cl->addWidget(m_aimClassContainer);

    // 「+ 添加」行
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(6);
    m_addClassCombo = new QComboBox;
    m_addClassCombo->setMinimumWidth(120);
    m_addClassCombo->setMinimumHeight(30);
    addRow->addWidget(m_addClassCombo, 1);

    m_addClassBtn = new QPushButton(QString::fromUtf8(u8"+ 添加"));
    m_addClassBtn->setFixedHeight(30);
    m_addClassBtn->setCursor(Qt::PointingHandCursor);
    addRow->addWidget(m_addClassBtn);
    cl->addLayout(addRow);

    connect(m_addClassBtn, &QPushButton::clicked, this, [this] {
        const int ri = currentRuntimeIndex();
        if (ri < 0 || m_addClassCombo->currentIndex() < 0) return;
        const int cid = m_addClassCombo->currentData().toInt();
        if (cid < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& acs = config.hotkeys[ri].aim_classes;
            for (const auto& a : acs)
                if (a.class_id == cid) return;   // 已存在
            HotkeyAimClass a;
            a.class_id = cid;
            // ★ 默认 0.65 = 偏上半身/头颈，与旧页一致（旧页注释原话：
            //   "1=框顶, 0=框底; 默认锁上半身/头颈"）。
            a.y_offset = 0.65f;
            a.y_offset_max = 0.65f;
            // ★ 默认预填 AI 页的全局置信度，让新加类别显示 = "跟随全局"；
            //   用户想收紧就上拉滑条，拉到 0 → 视作再次退回全局跟随。
            a.min_conf = static_cast<float>(config.confidence_threshold);
            acs.push_back(a);
        }
        ConfigBridge::instance().markDirty();
        rebuildAimClassRows();
    });

    m_rightLayout->addWidget(card);
    rebuildAimClassRows();
}

// 重建「+ 添加」下拉：来源是全局 class_filters（Target 页维护的那份）。
void AimSettingsPage::rebuildAddClassCombo()
{
    if (!m_addClassCombo) return;
    m_addClassCombo->clear();
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    for (const auto& cf : config.class_filters)
    {
        const QString nm = cf.class_name.empty()
            ? QStringLiteral("class_%1").arg(cf.class_id)
            : QString::fromUtf8(cf.class_name.c_str());
        m_addClassCombo->addItem(QStringLiteral("[%1] %2").arg(cf.class_id).arg(nm), cf.class_id);
    }
}

// 交换两个类别在优先级列表里的位置（★ 顺序 = 优先级，index 0 最高）。
void AimSettingsPage::moveAimClass(int from, int to)
{
    const int ri = currentRuntimeIndex();
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
    rebuildAimClassRows();
}

// 按 config.hotkeys[当前].aim_classes 重建整段行卡片。
void AimSettingsPage::rebuildAimClassRows()
{
    if (!m_aimClassLayout) return;

    // 清空（连同旧 widget 一起删）
    while (QLayoutItem* it = m_aimClassLayout->takeAt(0))
    {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }

    struct Row { int cid; float yMin; float yMax; float c; QString name; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        const int ri = currentRuntimeIndex();
        if (ri >= 0 && ri < static_cast<int>(config.hotkeys.size()))
        {
            for (const auto& ac : config.hotkeys[ri].aim_classes)
            {
                QString name = QStringLiteral("class_%1").arg(ac.class_id);
                for (const auto& cf : config.class_filters)
                    if (cf.class_id == ac.class_id && !cf.class_name.empty())
                    { name = QString::fromUtf8(cf.class_name.c_str()); break; }
                rows.push_back({ ac.class_id, ac.y_offset, ac.y_offset_max, ac.min_conf, name });
            }
        }
    }

    if (rows.empty())
    {
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

    // 只读置信标签: raw <= 0 → 「全局」（回退 AI 页阈值），否则 0.00~1.00。
    auto confText = [](int raw) {
        return raw <= 0 ? QString::fromUtf8(u8"全局")
                        : QString::number(raw / 100.0, 'f', 2);
    };

    const int total = static_cast<int>(rows.size());
    for (int idx = 0; idx < total; ++idx)
    {
        const Row r = rows[idx];
        const int classId = r.cid;

        auto* rowFrame = new QFrame;
        rowFrame->setObjectName("aimRow");
        rowFrame->setStyleSheet(
            "QFrame#aimRow{background:#FAFAFB; border:1px solid rgba(0,0,0,0.06);"
            " border-radius:8px;}");
        auto* rl = new QVBoxLayout(rowFrame);
        rl->setContentsMargins(12, 8, 10, 10);
        rl->setSpacing(8);

        // ── 第一行: #优先级 + [id] 类名 + ▲ ▼ ✕ ──
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
                                  QStringLiteral("#5E6AD2"),
                                  QString::fromUtf8(u8"上移（提高优先级）"));
        auto* downBtn = makeIconBtn(QString::fromUtf8(u8"▼"), QStringLiteral("#71717A"),
                                    QStringLiteral("#5E6AD2"),
                                    QString::fromUtf8(u8"下移（降低优先级）"));
        auto* delBtn = makeIconBtn(QString::fromUtf8(u8"✕"), QStringLiteral("#D25A5A"),
                                   QStringLiteral("#B83232"), QString::fromUtf8(u8"移除"));
        upBtn->setEnabled(idx > 0);
        downBtn->setEnabled(idx < total - 1);
        top->addWidget(upBtn);
        top->addWidget(downBtn);
        top->addWidget(delBtn);
        rl->addLayout(top);

        // ── 第二行: 随机锁点 Y 范围（每次新锁定抽一次，锁定期间不重抽）──
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

        auto* rangeRow = new QHBoxLayout;
        rangeRow->setSpacing(8);
        auto* yLbl = new QLabel(QString::fromUtf8(u8"随机锁点 Y"));
        yLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
        auto* yMinSpin = makeOffsetSpin(r.yMin);
        auto* yMaxSpin = makeOffsetSpin(r.yMax);
        yMinSpin->setToolTip(QString::fromUtf8(
            u8"范围下限：1=框顶，0.5=中心，0=框底。\n"
            u8"★ 该类的值会覆盖「控制器」卡里的热键级瞄点 Y（只在设了该类时）。"));
        yMaxSpin->setToolTip(QString::fromUtf8(
            u8"范围上限：每次新锁定在上下限之间随机一次。\n"
            u8"★ 等于下限时不随机（固定打同一个点）。"));
        rangeRow->addWidget(yLbl);
        rangeRow->addWidget(yMinSpin);
        rangeRow->addWidget(new QLabel(QString::fromUtf8(u8"—")));
        rangeRow->addWidget(yMaxSpin);
        rangeRow->addStretch();
        rl->addLayout(rangeRow);

        // ── 第三行: 最低置信度（准入）──
        auto* cSlider = new QSlider(Qt::Horizontal);
        cSlider->setRange(0, 100);
        cSlider->setSingleStep(1);
        cSlider->setPageStep(5);
        cSlider->setValue(std::clamp(static_cast<int>(std::lround(r.c * 100.0f)), 0, 100));
        cSlider->setMinimumWidth(80);
        cSlider->setToolTip(QString::fromUtf8(
            u8"最低置信度：低于此值的框不会夺锁。0 = 跟随 AI 页全局阈值。\n"
            u8"★ 与全局阈值是「都要过」的关系 —— 这里是额外收紧，不替代它。"));

        auto* bottom = new QHBoxLayout;
        bottom->setSpacing(10);
        auto* cLbl = new QLabel(QString::fromUtf8(u8"置信"));
        cLbl->setFixedWidth(32);
        cLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
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
        // ★ m_loading 期间不回写：reloadProfileToUi() 会重建这些控件，
        //   而 setValue 会触发 valueChanged ⇒ 若无条件回写，加载过程会把
        //   刚读出来的值再写一遍（并 markDirty），变成"假脏"。
        auto persistRange = [this, classId, yMinSpin, yMaxSpin](bool minChanged) {
            if (m_loading) return;
            // ★ 交叉钳制：改下限时若越过了上限，把上限顶上去（反之亦然）。
            //   不这么做的话会把一个 lo>hi 的区间写进配置，运行时再靠
            //   computeAnchor 内部 swap 兜底 —— 界面显示就与实际不符了。
            if (minChanged && yMinSpin->value() > yMaxSpin->value())
                yMaxSpin->setValue(yMinSpin->value());
            else if (!minChanged && yMaxSpin->value() < yMinSpin->value())
                yMinSpin->setValue(yMaxSpin->value());

            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId)
                    {
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

        connect(cSlider, &QSlider::valueChanged, this,
                [this, classId, cVal, confText](int raw) {
            cVal->setText(confText(raw));
            if (m_loading) return;
            const float v = static_cast<float>(raw) / 100.0f;
            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId) { a.min_conf = v; break; }
            }
            ConfigBridge::instance().markDirty();
        });

        connect(upBtn, &QPushButton::clicked, this,
                [this, idx] { moveAimClass(idx, idx - 1); });
        connect(downBtn, &QPushButton::clicked, this,
                [this, idx] { moveAimClass(idx, idx + 1); });
        connect(delBtn, &QPushButton::clicked, this, [this, classId] {
            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                auto& ac2 = config.hotkeys[ri2].aim_classes;
                ac2.erase(std::remove_if(ac2.begin(), ac2.end(),
                    [classId](const HotkeyAimClass& a) { return a.class_id == classId; }),
                    ac2.end());
            }
            ConfigBridge::instance().markDirty();
            rebuildAimClassRows();
        });
    }

    rebuildAddClassCombo();
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
    // ★ tip 是用户要求的"停留说明"（鼠标停在行或控件上都会弹出）。
    struct D { const char* obj; const char* label; double lo, hi, step, def; const char* tip; };
    const D gains[] = {
        { "ctlKpX", "Kp · 水平", 0.0, 500.0, 0.5, 35.0,
          "比例增益（跟枪 / 水平轴）。误差乘以它 = 本拍要走的位移。\n"
          "调大：贴上去更快，但太大（配合 s_max 过高）会开始左右摆动。\n"
          "★ 这是唯一非零的默认增益，调参先只动它。" },
        { "ctlKpY", "Kp · 垂直", 0.0, 500.0, 0.5, 35.0,
          "比例增益（压枪 / 垂直轴）。\n"
          "和 Kp·水平分开，是因为压枪和跟枪的手感需求不同。\n"
          "默认与水平相同。" },
        { "ctlKiX", "Ki · 水平", 0.0, 100.0, 0.01, 0.0,
          "积分增益（水平轴）。累积残差，用来消掉匀速目标留下的稳态滞后。\n"
          "★ 默认 0（关闭）。开太大遇到目标急停会过冲、来回甩。\n"
          "只在确认有稳态滞后时才加。" },
        { "ctlKiY", "Ki · 垂直", 0.0, 100.0, 0.01, 0.0,
          "积分增益（垂直轴）。默认 0（关闭），理由同水平轴。" },
        { "ctlKdX", "Kd · 水平", 0.0, 100.0, 0.01, 0.0,
          "微分增益（水平轴）。按误差变化速度提前刹车，抑制过冲。\n"
          "★ 默认 0。它对检测噪声很敏感 —— 调大之前先确认框是稳的。" },
        { "ctlKdY", "Kd · 垂直", 0.0, 100.0, 0.01, 0.0,
          "微分增益（垂直轴）。默认 0，理由同水平轴。" },
        { "ctlPFullScalePx", "P 项饱和 (像素, 0=不限)", 0.0, 2000.0, 1.0, 0.0,
          "P 项连续饱和阈值（像素）。误差超过它之后 P 项不再增大。\n"
          "★ 它负责『末段不冲过头』，取代了早期的死区。\n"
          "★ 0 = 不限。设成 0 以外的值会让大甩枪的力度被削平。" },
        { "ctlTauUnwindSec", "积分回吐时间常数 (秒)", 0.001, 5.0, 0.005, 0.030,
          "误差【反向】时积分按 exp(-dt/τ) 回吐的时间常数（秒）。\n"
          "越小 = 回吐越快，越不容易在目标变向时被旧积分顶着走。\n"
          "★ 30ms 是历史调整后的起点，无实测依据。" },
        { "ctlTauDerivSec", "D 项低通时间常数 (秒)", 0.0, 5.0, 0.005, 0.020,
          "D 项的低通时间常数（秒）。目标急停时误差导数会出现尖峰，\n"
          "低通用来削掉它，免得准星被朝『目标原来运动的方向』猛推一下。\n"
          "★ 0 = 不低通（噪声会直接进 D 项）。" },
        { "ctlIMax", "积分上限 (0=用输出限幅)", 0.0, 5000.0, 1.0, 0.0,
          "积分项的上限。0 = 直接用输出限幅当上限。\n"
          "限制积分是为了防止长时间同向误差把积分喂得过大，\n"
          "一旦反向就变成一大坨甩不掉的输出。" },
    };
    for (const D& d : gains)
        cl->addWidget(makeDoubleRowTip(d.obj, d.label, d.lo, d.hi, d.step, d.def,
                                       QString::fromUtf8(d.tip)));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"输出与瞄点")));
    // ★★ 2026-09-17 第四轮续: 这里原本有一对【热键级】的「瞄点 Y 偏移」/
    //   「Y 偏移上限」。按用户要求"改成和原来一样"，它们被删掉了 ——
    //   因为旧页【没有】热键级的 Y 设置，Y 只存在于「瞄准类别」卡里每一类的
    //   「随机锁点 Y」上（旧页全文搜 ctl_ 零命中，y_offset 只出现在逐类行里）。
    //
    //   ★ 删掉不是丢功能: 逐类 y_offset / y_offset_max 现在已经真的接进后端了
    //     （aim_controller 按锁定目标的 classId 查表，见 control::ClassAimPoint）。
    //   ★ 热键级的 ctl_y_offset / ctl_y_offset_max 字段仍在 config 里、仍会被
    //     搬运，作为【没设该类别时】的兜底值 —— 界面不再直接编辑它们，
    //     它们的值由配置文件保留（默认 0.5 = 框中心，正是"不干预"的语义）。

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"选靶与稳定器")));
    const D targetAndStab[] = {
        { "ctlHysteresisRatio", "选靶滞回倍数", 1.0, 10.0, 0.05, 1.3,
          "已锁定目标时，新候选必须比它『近这么多倍』才会换目标。\n"
          "★ 1.0 = 没有滞回（每帧都选最近的）。\n"
          "★ 不加滞回时，两个目标交替成为『最近』会让滤波器每帧复位 ——\n"
          "  滤波等于白做。所以它不是一个手感旋钮，是滤波能否生效的前提。" },
        { "ctlMaxDistancePx", "选靶距离上限 (0=不限)", 0.0, 5000.0, 5.0, 0.0,
          "距准星超过这个距离（检测像素）的候选不参与选靶。\n"
          "★ 0 = 不限制。默认 0，因为距离门控目前由 FOV 椭圆承担，\n"
          "  这里再设一道是重复的。" },
        { "ctlMatchCenterRatio", "稳定器·认目标中心系数", 0.001, 10.0, 0.05, 0.5,
          "『这一帧的框和上一帧是同一个目标吗』的判据：\n"
          "中心距离 < 上一帧框对角线 × 该系数 就算同一个目标。\n"
          "调大 = 更容易认成同一个（目标跳一下也接着跟）；\n"
          "调小 = 更容易判成换目标（会触发滤波复位）。" },
        { "ctlAreaRatioTol", "稳定器·面积容差倍数", 1.0, 100.0, 0.1, 2.0,
          "面积比超出 [1/该值, 该值] 就判为换目标。\n"
          "目标跑远/跑近时框面积本来就会变，这个容差就是留给它的。\n"
          "★ 不能小于 1（那是个自相矛盾的区间）。" },
        { "ctlKSnapMult", "稳定器·突变系数", 0.001, 100.0, 0.05, 1.15,
          "本帧位移超过『上一帧框对角线 × 该系数』就判为瞬移（Snap）。\n"
          "瞬移会触发滤波与 PID 的硬重置。\n"
          "调小 = 更敏感（真的换目标时反应快，但抖动也可能误判）；\n"
          "调大 = 更宽容（可能把真换目标当成目标在快速移动）。" },
        { "ctlMinAspect", "稳定器·最小宽高比", 0.001, 100.0, 0.05, 0.2,
          "宽高比低于它就当作离谱误检丢掉（太细太长）。\n"
          "★ 会自动与最大值排序，保证 min ≤ max。" },
        { "ctlMaxAspect", "稳定器·最大宽高比", 0.001, 100.0, 0.05, 5.0,
          "宽高比高于它就当作离谱误检丢掉（太扁太宽）。" },
    };
    for (const D& d : targetAndStab)
        cl->addWidget(makeDoubleRowTip(d.obj, d.label, d.lo, d.hi, d.step, d.def,
                                       QString::fromUtf8(d.tip)));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"输出限幅与随机化")));
    cl->addWidget(makeIntRow("ctlMaxOutputCounts", "单拍最大位移 (计数)", 1, 1000, 1, 200,
        QString::fromUtf8(u8"一拍最多发多少个鼠标计数（1 计数 = 链路的最小位移）。\n"
        "它是最后一道安全闸：不管 PID 算出多大的值，单拍都不会超过它。\n"
        "★ 调小 = 更安全但更慢；调大 = 甩枪更猛，但错的时候也更猛。\n"
        "★ 第一次打开控制器建议先设小一点。")));
    cl->addWidget(makeIntRow("ctlRandomSeed", "瞄点随机种子 (0=固定)", 0, 999999, 1, 0,
        QString::fromUtf8(u8"瞄点 Y 随机抖动的种子。0 = 用内部固定常数（同一帧可复现）。\n"
        "★ 非 0 时每次启动都会得到不同的抖动序列。\n"
        "★ 只在「瞄准类别」里某一类的『随机锁点 Y』上下限【不相等】时才有意义。")));

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
        // ★★ 2026-09-17 第四轮续: ctl_y_offset / ctl_y_offset_max 不再从界面写。
        //   热键级 Y 这对控件已按"和旧页一样"删除（旧页没有它们，Y 只逐类设）。
        //   ★ 刻意【不】在这里写 hp.ctl_y_offset —— 保持配置里已有的值不动。
        //     写 0.5 的话会把用户配置文件里可能存在的兜底值静默冲掉。
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
// ★★ 自动扳机 (2026-09-17 恢复)
//
// 后端: mouse/trigger_fsm.h（状态机）+ trigger_scope.h（自动开镜）
//       + auto_stop.h（自动急停）。接线: runtime/aim_loop.cpp。
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::buildTriggerCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"自动扳机"),
                                QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    auto* enable = new QCheckBox(QStringLiteral("启用自动扳机（准星进入命中区就开火）"));
    enable->setObjectName("triggerEnabled");
    cl->addWidget(enable);
    attachTip(enable, QString::fromUtf8(
        u8"总开关。关着的时候不会碰左键，也不会碰右键。\n"
        u8"★ 命中区 = 以【检测框】为基准的一个区间，与瞄点无关。\n"
        u8"★ 打开后本程序会真的开火 —— 请先确认瞄准控制器已经调好。"));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"开火时机")));
    cl->addWidget(makeIntRow("triggerYPercent", "命中区占框的百分比 (%)", 10, 300, 5, 100,
        QString::fromUtf8(u8"命中区的高度 = 框高 × 该百分比，宽度同理（等比）。\n"
        u8"★ 100 = 整框；>100 = 框上方也算（预开火，会打得更早）；\n"
        u8"  <100 = 只有框中间一条算（更严格的「打到才开火」）。\n"
        u8"★ 这个区间同时管横向和纵向，所以它是一个正方形比例。")));
    cl->addWidget(makeIntRow("triggerFireDelay", "进区后延迟开火 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"进入命中区之后等这么多毫秒才按下左键。\n"
        u8"★ 0 = 进区那一拍立刻开火（机械级瞬发）。\n"
        u8"★ 想「停稳了再开枪」就调大它 —— 配合自动急停一起用。")));
    cl->addWidget(makeIntRow("triggerFireDuration", "单次按住时长 (ms, 0=长按)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"连点模式下，每发按住左键多久。\n"
        u8"★ 0 = 长按模式：只要还在命中区就一直按着，离开才松手。\n"
        u8"★ 非 0 = 连点模式：按住这么久就松开，然后走一次冷却间隔。")));
    cl->addWidget(makeIntRow("triggerFireInterval", "连点冷却间隔 (ms)", 1, 2000, 5, 200,
        QString::fromUtf8(u8"两发之间的冷却。\n"
        u8"★ 连点模式：松开左键后等这么久才能再开火。\n"
        u8"★ 长按模式：离开命中区松手后等这么久。\n"
        u8"★ 不要设成 0 —— 那会在命中区里退化成每拍 press/release 的抖动。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"抖动（破除机械感）")));
    cl->addWidget(makeIntRow("triggerDelayJitter", "开火延迟 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「进区后延迟开火」加一个 ±N ms 的随机抖动，\n"
        u8"让每枪的节奏不完全一致。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerDurationJitter", "按住时长 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「单次按住时长」加 ±N ms 随机抖动。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerIntervalJitter", "冷却间隔 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「连点冷却间隔」加 ±N ms 随机抖动。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerSwitchCooldown", "换目标冷却 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"目标身份变化后，等这么久才允许再次开火。\n"
        u8"★ 0 = 不冷却，换目标立刻可以打。\n"
        u8"★ 只在「换目标且当前不在命中区」时生效 —— 转火后新目标就在准星上时\n"
        u8"  会立刻接力开火，不会为了冷却卡一下。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"自动开镜")));
    auto* scopeCombo = new QComboBox;
    scopeCombo->setObjectName("triggerAutoScope");
    scopeCombo->addItem(QStringLiteral("关闭"), 0);
    scopeCombo->addItem(QStringLiteral("点按右键一下（不收镜）"), 1);
    scopeCombo->addItem(QStringLiteral("长按右键（按住开镜）"), 2);
    auto* scopeRow = FormKit::fieldRow(QStringLiteral("开火方式"), scopeCombo);
    attachTip(scopeRow, QString::fromUtf8(
        u8"仿 AimMagic 的「开火方式」。\n"
        u8"★ 点按：每次接敌开始时点一下右键，之后就不再碰它 —— 不自动收镜，\n"
        u8"  开镜状态由你自己负责（实机反馈：自动收镜会和你的操作打架）。\n"
        u8"★ 长按：命中区里一直按住，离开时松开。\n"
        u8"★ 如果你的热键本身绑了右键，这一项一律不生效（否则会把镜切回去）。"));
    cl->addWidget(scopeRow);

    cl->addWidget(makeIntRow("triggerScopeDelay", "开镜后等多久才开火 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"按下右键之后等这么多毫秒才允许开左键 ——\n"
        u8"保证第一颗子弹是【开着镜】打出去的。\n"
        u8"★ 0 = 不等（同一拍就开火，可能第一枪还没进镜）。\n"
        u8"★ 本项目有意与 AimMagic 不同：AM 是同一拍先左键再右键，\n"
        u8"  那第一枪其实没开镜。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"自动急停")));
    auto* stopCombo = new QComboBox;
    stopCombo->setObjectName("triggerAutoStop");
    stopCombo->addItem(QStringLiteral("关闭"), 0);
    stopCombo->addItem(QStringLiteral("开启（开火时补反方向键）"), 1);
    auto* stopRow = FormKit::fieldRow(QStringLiteral("开关"), stopCombo);
    attachTip(stopRow, QString::fromUtf8(
        u8"开火那一拍如果你正按着 WASD，就往盒子里补一个【反方向键】的短按\n"
        u8"（W→S / S→W / A→D / D→A）。多数 FPS 里相反方向键同时存在 = 抵消 = 立刻停住，\n"
        u8"这一枪才是站定打的。\n"
        u8"★ 是「补键」不是「抢键」 —— 盒子没有屏蔽你物理按键的能力。\n"
        u8"★ 只有带键盘通道的输入方式支持（MAKCUNEW / KMBOXNET）；\n"
        u8"  其它输入方式会自动跳过并在日志里记为 unsupported。\n"
        u8"★ 一次只补一个键，斜向移动（W+A）只会抵消掉前后轴那一半。"));
    cl->addWidget(stopRow);

    cl->addWidget(makeIntRow("triggerStopMs", "反方向键短按时长 (ms)", 20, 300, 5, 60,
        QString::fromUtf8(u8"补的那个反方向键按住多久。\n"
        u8"★ 用固件定时弹起（KEY_TAP），是自清的 —— 就算上位机崩了键也会被放开。\n"
        u8"★ 太短：固件来不及弹起；太长：你被推着倒退一段。范围 20~300。")));

    auto* note = makeHint(QString::fromUtf8(
        u8"★ 扳机用的是【以框为基准】的命中区，和瞄点解耦 —— 换瞄点（胸口/头部）"
        u8"不会改变触发几何。\n"
        u8"★ 判定输入是【原始准星】，不是平滑过的值。"));
    cl->addWidget(note);

    // ── 写回 ──────────────────────────────────────────────────────────
    auto commit = [this, enable, scopeCombo, stopCombo]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.trigger_enabled            = enable->isChecked();
        hp.trigger_y_percent          = i("triggerYPercent");
        hp.trigger_fire_delay         = i("triggerFireDelay");
        hp.trigger_fire_duration      = i("triggerFireDuration");
        hp.trigger_fire_interval      = i("triggerFireInterval");
        hp.trigger_delay_jitter_ms    = i("triggerDelayJitter");
        hp.trigger_duration_jitter_ms = i("triggerDurationJitter");
        hp.trigger_interval_jitter_ms = i("triggerIntervalJitter");
        hp.trigger_switch_cooldown_ms = i("triggerSwitchCooldown");
        hp.trigger_auto_scope         = scopeCombo->currentData().toInt();
        hp.trigger_scope_delay_ms     = i("triggerScopeDelay");
        hp.trigger_auto_stop          = stopCombo->currentData().toInt();
        hp.trigger_stop_ms            = i("triggerStopMs");

        ConfigBridge::instance().markDirty();
    };

    connect(enable, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    connect(stopCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    for (auto* sp : m_triggerInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// ★★ 轨迹曲线 / 风力曲线 (2026-09-17 恢复)
//
// 后端: mouse/aim_path.h。接线: runtime/aim_loop.cpp。
// ★ 四种模式【只旋转不缩放】控制器输出 —— 曲线只提供局部切线方向，
//   幅值仍由 PID 决定。
// ═══════════════════════════════════════════════════════════════════════════

void AimSettingsPage::buildTrajectoryCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"轨迹曲线"),
                                QStringLiteral("vector-spline"));
    auto* cl = card->contentLayout();

    auto* modeCombo = new QComboBox;
    modeCombo->setObjectName("aimPathMode");
    modeCombo->addItem(QStringLiteral("直线（透传，不整形）"), 0);
    modeCombo->addItem(QStringLiteral("贝塞尔曲线"), 1);
    modeCombo->addItem(QStringLiteral("自定义手绘"), 2);
    modeCombo->addItem(QStringLiteral("WindMouse（风力曲线）"), 3);
    auto* modeRow = FormKit::fieldRow(QStringLiteral("轨迹模式"), modeCombo);
    attachTip(modeRow, QString::fromUtf8(
        u8"移动轨迹的整形方式。\n"
        u8"★ 直线 = 完全透传控制器输出，与不开这个功能逐位一致。\n"
        u8"★ 其余三种都【只旋转不缩放】：曲线只决定「往哪个方向走」，\n"
        u8"  每拍走多远仍然由 PID 决定。所以它不会把控制器拖成振荡。\n"
        u8"★ 风力曲线额外带一个门控（见下面的门控阈值）：误差很小时整段\n"
        u8"  旁路走直线 —— 小修正保精度，只有大甩枪才走拟人路径。"));
    cl->addWidget(modeRow);

    auto* infl = new QSpinBox;
    infl->setRange(0, 100);
    infl->setObjectName("aimPathInfluence");
    infl->setValue(25);
    infl->setSuffix(QStringLiteral(" %"));
    m_pathInts.push_back(infl);
    auto* inflRow = FormKit::fieldRow(QStringLiteral("曲线影响量"), infl);
    attachTip(inflRow, QString::fromUtf8(
        u8"曲线对控制器原始方向的影响程度。\n"
        u8"★ 0% = 完全透传 PID（等于直线，但模式仍算开启）。\n"
        u8"★ 100% = 完整采用曲线切线方向。\n"
        u8"★ 默认 25%，避免曲线完全接管移动。"));
    cl->addWidget(inflRow);

    // ── 贝塞尔控制点 ──────────────────────────────────────────────────
    // ★ 这里【不能】用 makeDoubleRowTip: 它会把控件收进 m_ctlDoubles,
    //   而这两个 lambda 的 commit 是分开的 —— 收错集合会导致控制器卡
    //   在保存时把贝塞尔控制点当成控制器参数写一遍(值恰好同名就会互相覆盖)。
    //   所以贝塞尔/风力这两组用自己的收集器 m_pathDoubles。
    m_pathSectionBezier = makeSectionTitle(QString::fromUtf8(u8"贝塞尔控制点"));
    cl->addWidget(m_pathSectionBezier);
    attachTip(m_pathSectionBezier, QString::fromUtf8(
        u8"只用「贝塞尔曲线」模式时生效。\n"
        u8"曲线从起点 (0,0) 到终点 (1,0)，两个控制点决定它弯成什么样。\n"
        u8"X 是「走到全程的百分之几」，Y 是「横向偏出弦长的多少倍」。"));

    struct PD { const char* obj; const char* label; double lo, hi, step, def; const char* tip; };
    const PD beziers[] = {
        { "pathCx1", "控制点 1 · X", 0.0, 1.0, 0.05, 0.30,
          "第一个控制点的 X（0~1，沿起点→终点方向的位置）。" },
        { "pathCy1", "控制点 1 · Y", -1.0, 1.0, 0.05, 0.0,
          "第一个控制点的 Y（-1~1，垂直方向的偏移比例）。\n正负决定往哪一侧弯。" },
        { "pathCx2", "控制点 2 · X", 0.0, 1.0, 0.05, 0.70,
          "第二个控制点的 X。" },
        { "pathCy2", "控制点 2 · Y", -1.0, 1.0, 0.05, 0.0,
          "第二个控制点的 Y。\n两个 Y 同号 = 往一侧弯；异号 = S 形。" },
        // ── WindMouse ─────────────────────────────────────────────────
        { "windGravity", "重力 G（WindMouse）", 0.1, 100.0, 0.5, 5.0,
          "向目标的吸引强度（WindMouse 的 G0，单位像素）。\n"
          "★ 越大越「坚决」，路径越直、越快贴上目标。\n"
          "★ 越小越飘。它和「风力」是一对：重力管收束，风力管发散。" },
        { "windWind", "风力 W（WindMouse）", 0.0, 100.0, 0.5, 2.0,
          "横向随机游走的幅度（WindMouse 的 W0，单位像素）。\n"
          "★ 越大路径越「飘」、越像人甩出来的。\n"
          "★ 调太大准星会明显绕着走，贴身精度会下降。" },
        { "windStep", "步长 M（WindMouse）", 1.0, 200.0, 1.0, 10.0,
          "单步最大长度（WindMouse 的 M0，单位像素）。\n"
          "★ 越小路径越碎、越慢；越大越接近直线。" },
        { "windDistance", "风力衰减距离 D（WindMouse）", 1.0, 200.0, 1.0, 8.0,
          "离目标多近时风力开始衰减（WindMouse 的 D0，单位像素）。\n"
          "★ 越接近目标风越小 —— 保证最后一小段能稳住，不会在锚点附近乱飘。" },
    };
    for (const PD& d : beziers)
    {
        auto* sp = new QDoubleSpinBox;
        sp->setRange(d.lo, d.hi);
        sp->setSingleStep(d.step);
        sp->setDecimals(3);
        sp->setObjectName(QString::fromUtf8(d.obj));
        sp->setValue(d.def);
        m_pathDoubles.push_back(sp);
        auto* row = FormKit::fieldRow(QString::fromUtf8(d.label), sp);
        attachTip(row, QString::fromUtf8(d.tip));
        cl->addWidget(row);
    }

    cl->addWidget(makeIntRow("windThreshold", "门控阈值 (像素)", 0, 500, 1, 10,
        QString::fromUtf8(u8"两个轴的误差都【不超过】它时，整段曲线旁路，直接走直线。\n"
        u8"★ 这是 AimMagic 的 curve_threshold。\n"
        u8"★ 10px 的默认值意味着：微修正（贴住目标之后的抖动）走直线保精度，\n"
        u8"  只有大甩枪才走拟人路径。\n"
        u8"★ 设成 0 = 任何误差都走曲线（不是「关闭曲线」）。")));

    auto* note = makeHint(QString::fromUtf8(
        u8"★ 轨迹只有在【瞄准控制器开启】时才有意义 —— 它整形的是控制器算出来的位移。\n"
        u8"★ 四种模式都只旋转不缩放：起段让第一拍恒等于控制器输出，"
        u8"接近锚点时回退直线。"));
    cl->addWidget(note);

    auto commit = [this, modeCombo]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto d = [this](const char* n) { return findChild<QDoubleSpinBox*>(n)->value(); };
        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.aim_path_mode          = modeCombo->currentData().toInt();
        hp.aim_path_influence     = i("aimPathInfluence");
        hp.aim_path_bezier_cx1    = static_cast<float>(d("pathCx1"));
        hp.aim_path_bezier_cy1    = static_cast<float>(d("pathCy1"));
        hp.aim_path_bezier_cx2    = static_cast<float>(d("pathCx2"));
        hp.aim_path_bezier_cy2    = static_cast<float>(d("pathCy2"));
        hp.aim_path_wind_gravity  = static_cast<float>(d("windGravity"));
        hp.aim_path_wind_wind     = static_cast<float>(d("windWind"));
        hp.aim_path_wind_step     = static_cast<float>(d("windStep"));
        hp.aim_path_wind_distance = static_cast<float>(d("windDistance"));
        hp.aim_path_wind_threshold = i("windThreshold");

        ConfigBridge::instance().markDirty();
    };

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    for (auto* sp : m_pathInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    for (auto* sp : m_pathDoubles)
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [commit](double) { commit(); });

    m_rightLayout->addWidget(card);
}

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
            if (keys.isEmpty()) keys = QStringLiteral("None");

            // ★ 旧页的列表行是【两行自定义 widget】(名字 + 按键)，不是
            //   "名字\n按键" 的单条 item —— 后者两行共用同一个字体/颜色，
            //   没有主次层级。这里按旧页恢复：QListWidgetItem 只当作容器，
            //   真正显示的是 setItemWidget 挂上去的两行 QLabel。
            auto* item = new QListWidgetItem(m_profileList);
            item->setData(Qt::UserRole, i);

            auto* w = new QWidget;
            w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            auto* v = new QVBoxLayout(w);
            v->setContentsMargins(11, 8, 11, 8);
            v->setSpacing(3);

            auto* nameLbl = new QLabel(QString::fromUtf8(hp.name.c_str()));
            nameLbl->setObjectName("pname");
            auto* keyLbl = new QLabel(keys);
            keyLbl->setObjectName("pkey");

            v->addWidget(nameLbl);
            v->addWidget(keyLbl);

            item->setSizeHint(w->sizeHint());
            m_profileList->setItemWidget(item, w);
        }
    }
    m_profileList->blockSignals(false);
    if (m_profileList->count() > 0)
    {
        m_profileList->setCurrentRow(0);
        onProfileSelected(0);
    }
    restyleProfileItems();
}

// ★ 选中态的两行颜色。旧页这段(line 1594)随页面一起被删了 ——
//   没有它，选中行的名字和按键都保持同一个颜色，看不出哪一行被选中。
void AimSettingsPage::restyleProfileItems()
{
    if (!m_profileList) return;
    for (int i = 0; i < m_profileList->count(); ++i)
    {
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

void AimSettingsPage::onGroupChanged(int)
{
    rebuildProfileList();
}

void AimSettingsPage::onProfileSelected(int)
{
    restyleProfileItems();   // ★ 选中态换色, 必须在 reload 之前/之后都刷一次
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
            // ★★ ctlYOffset / ctlYOffsetMax 的 sd() 已删 —— 那对控件不存在了。
            //   逐类 Y 由 rebuildAimClassRows() 从 hp.aim_classes 直接重建。
            sd("ctlHysteresisRatio", hp.ctl_hysteresis_ratio);
            sd("ctlMaxDistancePx", hp.ctl_max_distance_px);
            sd("ctlMatchCenterRatio", hp.ctl_match_center_ratio);
            sd("ctlAreaRatioTol", hp.ctl_area_ratio_tol);
            sd("ctlKSnapMult", hp.ctl_k_snap_mult);
            sd("ctlMinAspect", hp.ctl_min_aspect);
            sd("ctlMaxAspect", hp.ctl_max_aspect);
            si("ctlMaxOutputCounts", hp.ctl_max_output_counts);
            si("ctlRandomSeed", hp.ctl_random_seed);

            // ── ★ 自动扳机 13 项 (2026-09-17 恢复) ──────────────────────
            // ★ 同样"漏一个就是静默失效" —— 而且这里更危险: 扳机是唯一
            //   会真的点左键的东西, 显示旧值会让用户以为自己关掉了。
            if (auto* c = findChild<QCheckBox*>("triggerEnabled"))
                c->setChecked(hp.trigger_enabled);
            if (auto* c = findChild<QComboBox*>("triggerAutoScope"))
            {
                const int k = c->findData(hp.trigger_auto_scope);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            if (auto* c = findChild<QComboBox*>("triggerAutoStop"))
            {
                const int k = c->findData(hp.trigger_auto_stop > 0 ? 1 : 0);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            si("triggerYPercent",         hp.trigger_y_percent);
            si("triggerFireDelay",        hp.trigger_fire_delay);
            si("triggerFireDuration",     hp.trigger_fire_duration);
            si("triggerFireInterval",     hp.trigger_fire_interval);
            si("triggerDelayJitter",      hp.trigger_delay_jitter_ms);
            si("triggerDurationJitter",   hp.trigger_duration_jitter_ms);
            si("triggerIntervalJitter",   hp.trigger_interval_jitter_ms);
            si("triggerSwitchCooldown",   hp.trigger_switch_cooldown_ms);
            si("triggerScopeDelay",       hp.trigger_scope_delay_ms);
            si("triggerStopMs",           hp.trigger_stop_ms);

            // ── ★ 轨迹曲线 11 项 (2026-09-17 恢复) ──────────────────────
            if (auto* c = findChild<QComboBox*>("aimPathMode"))
            {
                const int k = c->findData(hp.aim_path_mode);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            si("aimPathInfluence",    hp.aim_path_influence);
            si("windThreshold",       hp.aim_path_wind_threshold);
            sd("pathCx1",             hp.aim_path_bezier_cx1);
            sd("pathCy1",             hp.aim_path_bezier_cy1);
            sd("pathCx2",             hp.aim_path_bezier_cx2);
            sd("pathCy2",             hp.aim_path_bezier_cy2);
            sd("windGravity",         hp.aim_path_wind_gravity);
            sd("windWind",            hp.aim_path_wind_wind);
            sd("windStep",            hp.aim_path_wind_step);
            sd("windDistance",        hp.aim_path_wind_distance);

            // 已选类别行卡片（★ 整段重建：逐类参数按 classId 对应，
            //   沿用旧控件指针在增删/换位后会错位）。
            rebuildAimClassRows();
        }
    }

    // ★ 旧的 aimClassList / classCombo 命名查找已随 QListWidget 版卡片一起删除。
    //   现在的类别行由 rebuildAimClassRows() 自持，下拉由 rebuildAddClassCombo()
    //   重建（来源仍是全局 class_filters），两者都在上面那次调用里刷新过了。

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
