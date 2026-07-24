#include "pages/HotkeyPage.h"
#include "pages/TargetPage.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "widgets/AdaptiveStack.h"
#include "widgets/BezierEditor.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/FreehandCurveEditor.h"
#include "widgets/NeuralCurveTrainer.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"

#include <QShowEvent>
#include <QDropEvent>

#include <QAbstractItemView>
#include <QButtonGroup>
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
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
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
    buildAimPathCard();

    m_rightLayout->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: 触发按键  (单键下拉选择)
// ═══════════════════════════════════════════════════════════════════════════

struct KeyEntry { const char* id; const char* label; };
static const KeyEntry kKeyEntries[] = {
    // 鼠标
    {"\xe6\x97\xa0 (\xe5\xa7\x8b\xe7\xb5\x82\xe6\xb4\xbb\xe8\xb7\x83)",  ""},           // 无 (始终活跃)
    {"RightMouseButton",   "\xe9\xbc\xa0\xe6\xa0\x87\xe5\x8f\xb3\xe9\x94\xae (RightMouseButton)"},
    {"X1MouseButton",      "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xbe\xa7\xe9\x94\xae" "4 (X1MouseButton)"},
    {"X2MouseButton",      "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xbe\xa7\xe9\x94\xae" "5 (X2MouseButton)"},
    {"MiddleMouseButton",  "\xe9\xbc\xa0\xe6\xa0\x87\xe4\xb8\xad\xe9\x94\xae (MiddleMouseButton)"},
    {"LeftMouseButton",    "\xe9\xbc\xa0\xe6\xa0\x87\xe5\xb7\xa6\xe9\x94\xae (LeftMouseButton)"},
    // 功能键
    {"CapsLock",           "CapsLock (\xe5\xa4\xa7\xe5\xb0\x8f\xe5\x86\x99\xe9\x94\xae)"},
    {"Tab",                "Tab"},
    {"Escape",             "Escape"},
    {"Space",              "Space (\xe7\xa9\xba\xe6\xa0\xbc)"},
    // Shift / Alt / Ctrl
    {"LeftShift",          "LeftShift"},
    {"RightShift",         "RightShift"},
    {"LeftAlt",            "LeftAlt"},
    {"RightAlt",           "RightAlt"},
    {"LeftControl",        "LeftControl"},
    {"RightControl",       "RightControl"},
    // F 键
    {"F1","F1"},{"F2","F2"},{"F3","F3"},{"F4","F4"},
    {"F5","F5"},{"F6","F6"},{"F7","F7"},{"F8","F8"},
    {"F9","F9"},{"F10","F10"},{"F11","F11"},{"F12","F12"},
    // 字母
    {"A","A"},{"B","B"},{"C","C"},{"D","D"},{"E","E"},
    {"F","F"},{"G","G"},{"H","H"},{"I","I"},{"J","J"},
    {"K","K"},{"L","L"},{"M","M"},{"N","N"},{"O","O"},
    {"P","P"},{"Q","Q"},{"R","R"},{"S","S"},{"T","T"},
    {"U","U"},{"V","V"},{"W","W"},{"X","X"},{"Y","Y"},{"Z","Z"},
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
        QStringLiteral("\xe6\x89\xb3\xe6\x9c\xba"),        // 扳机
        QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8"),         // 启用
        false, m_triggerEnabled));

    auto makeSpin = [](int min, int max, const QString& suffix) {
        auto* sp = new QSpinBox;
        sp->setRange(min, max);
        sp->setSuffix(suffix);
        sp->setMinimumHeight(30);
        return sp;
    };

    m_triggerFireDelay      = makeSpin(0,    5000, QStringLiteral(" ms"));
    m_triggerFireDuration   = makeSpin(1,    5000, QStringLiteral(" ms"));
    m_triggerFireInterval   = makeSpin(0,    5000, QStringLiteral(" ms"));
    m_triggerYPercent       = makeSpin(1,     500, QStringLiteral(" %"));   // 上限 500% 支持预开火
    m_triggerDelayJitter    = makeSpin(0,     500, QStringLiteral(" ms"));
    m_triggerDurationJitter = makeSpin(0,     500, QStringLiteral(" ms"));
    m_triggerIntervalJitter = makeSpin(0,     500, QStringLiteral(" ms"));
    m_triggerSwitchCooldown = makeSpin(0,    5000, QStringLiteral(" ms"));

    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\xbb\xb6\xe8\xbf\x9f"),                   // 延迟
        m_triggerFireDelay));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\xbb\xb6\xe8\xbf\x9f\xe6\x8a\x96\xe5\x8a\xa8 \xc2\xb1"),  // 延迟抖动 ±
        m_triggerDelayJitter));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe6\x8c\x89\xe4\xbd\x8f\xe6\x97\xb6\xe9\x95\xbf"),  // 按住时长
        m_triggerFireDuration));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe6\x8c\x89\xe4\xbd\x8f\xe6\x8a\x96\xe5\x8a\xa8 \xc2\xb1"),  // 按住抖动 ±
        m_triggerDurationJitter));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\x86\xb7\xe5\x8d\xb4"),                   // 冷却
        m_triggerFireInterval));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\x86\xb7\xe5\x8d\xb4\xe6\x8a\x96\xe5\x8a\xa8 \xc2\xb1"),  // 冷却抖动 ±
        m_triggerIntervalJitter));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe8\xbd\xac\xe7\x81\xab\xe5\xbb\xb6\xe8\xbf\x9f"),           // 转火延迟
        m_triggerSwitchCooldown));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\x91\xbd\xe4\xb8\xad\xe5\x8c\xba"),       // 命中区
        m_triggerYPercent));

    m_triggerYPercent->setToolTip(QStringLiteral(
        "\xe5\x91\xbd\xe4\xb8\xad\xe5\x8c\xba\xe5\x8d\xa0 bbox \xe7\x99\xbe\xe5\x88\x86\xe6\xaf\x94\xef\xbc\x9a\n"
        "100 = \xe6\x95\xb4\xe6\xa1\x86\xef\xbc\x9b\n"
        "> 100 = \xe6\x89\xa9\xe5\xa4\xa7\xe5\x88\xb0 bbox \xe5\xa4\x96\xef\xbc\x88\xe9\xa2\x84\xe5\xbc\x80\xe7\x81\xab\xef\xbc\x89\xe3\x80\x82"));
    m_triggerDelayJitter->setToolTip(QStringLiteral(
        "\xe4\xb8\xba\xe5\xbb\xb6\xe8\xbf\x9f\xe5\x8a\xa0\xe4\xb8\x80\xe4\xb8\xaa \xc2\xb1N ms \xe7\x9a\x84\xe9\x9a\x8f\xe6\x9c\xba\xe6\x8a\x96\xe5\x8a\xa8\xef\xbc\x8c\xe7\xa0\xb4\xe9\x99\xa4\xe6\x9c\xba\xe6\xa2\xb0\xe6\x84\x9f\xe3\x80\x82"));
    m_triggerSwitchCooldown->setToolTip(QStringLiteral(
        "\xe7\x9b\xae\xe6\xa0\x87 track_id \xe5\x8f\x98\xe5\x8c\x96\xe6\x97\xb6\xe7\x9a\x84\xe8\xbd\xac\xe7\x81\xab\xe5\x86\xb7\xe5\x8d\xb4\xef\xbc\x8c\xe6\xb6\x88\xe9\x99\xa4\xe7\x9e\xac\xe5\x88\x87\xe6\x84\x9f\xe3\x80\x82"));

    connect(m_triggerEnabled, &ToggleSwitch::toggled,
            this, [this] { saveUiToCurrentProfile(); });
    auto wireInt = [this](QSpinBox* sp) {
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
    };
    wireInt(m_triggerFireDelay);
    wireInt(m_triggerFireDuration);
    wireInt(m_triggerFireInterval);
    wireInt(m_triggerYPercent);
    wireInt(m_triggerDelayJitter);
    wireInt(m_triggerDurationJitter);
    wireInt(m_triggerIntervalJitter);
    wireInt(m_triggerSwitchCooldown);

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

    m_lostTargetCacheFrames = new QSpinBox;
    m_lostTargetCacheFrames->setRange(0, 240);
    m_lostTargetCacheFrames->setValue(5);
    m_lostTargetCacheFrames->setSuffix(QString::fromUtf8(u8" 帧"));
    m_lostTargetCacheFrames->setMinimumHeight(30);
    m_lostTargetCacheFrames->setToolTip(QString::fromUtf8(
        u8"检测暂时丢失时保留同一目标的帧数；0 = 当帧释放。"));
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"丢失目标缓存"), m_lostTargetCacheFrames));
    connect(m_lostTargetCacheFrames, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &HotkeyPage::saveUiToCurrentProfile);

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

        auto* upBtn = makeIconBtn(QStringLiteral("▲"), QStringLiteral("#71717A"),
                                  QStringLiteral("#5E6AD2"), QString::fromUtf8(u8"上移（提高优先级）"));
        auto* downBtn = makeIconBtn(QStringLiteral("▼"), QStringLiteral("#71717A"),
                                    QStringLiteral("#5E6AD2"), QString::fromUtf8(u8"下移（降低优先级）"));
        auto* delBtn = makeIconBtn(QStringLiteral("✕"), QStringLiteral("#D25A5A"),
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
    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe9\x95\xad\xe5\xb0\x84\xe6\x89\xbe\xe8\x89\xb2 (\xe6\xad\xa4\xe7\x83\xad\xe9\x94\xae)"),
        false, m_laserDetect));
    cl->addWidget(FormKit::toggleRow(
        QString::fromUtf8(u8"启用寻光检测（此热键）"),
        false, m_flashlightDetect));
    cl->addWidget(FormKit::toggleRow(
        QString::fromUtf8(u8"启用玻璃过滤（此热键）"),
        false, m_glassFilter));

    connect(m_crosshairDetect,  &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_laserDetect,      &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_flashlightDetect, &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_glassFilter,      &ToggleSwitch::toggled, this, &HotkeyPage::saveUiToCurrentProfile);

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

    const double defaults[10] = {1, 1, 0, 0, .01, .01, 0, 0, 0, 0};
    for (int i = 0; i < 10; ++i)
    {
        const int decimals = (i == 0 || i == 1 || i == 6 || i == 7) ? 2 : 3;
        // AVA 没有给这些 QDoubleSpinBox 额外设置 range，沿用 Qt 的
        // 原生默认范围 0..99.99。此前把 LR 人为限制到 1.0 会截断
        // AVA 界面本来允许写入的预测速度。
        m_pidfGain[i] = makeDouble(0.0, 99.99,
                                   defaults[i], 0.001, decimals);
    }
    for (int i = 0; i < 4; ++i)
        m_pidfInteger[i] = makeInt(0, 1000, 0);

    const QString lockTip = QString::fromUtf8(
        u8"AVA 的 Kf：决定速度前馈/提前量强度；为 0 时预测速度不会改变输出。");
    const QString predictionTip = QString::fromUtf8(
        u8"AVA 的 LR：控制 Kf 前馈状态的学习速度，不是独立提前量；需配合非零锁定强度。");
    m_pidfGain[6]->setToolTip(lockTip);
    m_pidfGain[7]->setToolTip(lockTip);
    m_pidfGain[8]->setToolTip(predictionTip);
    m_pidfGain[9]->setToolTip(predictionTip);

    // AVA 的 Ki 不在界面中出现，并在配置加载时固定为 0。
    m_pidfGain[2]->setParent(card);
    m_pidfGain[3]->setParent(card);
    m_pidfGain[2]->hide();
    m_pidfGain[3]->hide();

    const std::array<std::pair<const char*, QWidget*>, 12> fields = {{
        {u8"瞄准速度 X", m_pidfGain[0]}, {u8"瞄准速度 Y", m_pidfGain[1]},
        {u8"过冲控制 X", m_pidfGain[4]}, {u8"过冲控制 Y", m_pidfGain[5]},
        {u8"锁定强度 X", m_pidfGain[6]}, {u8"锁定强度 Y", m_pidfGain[7]},
        {u8"预测速度 X", m_pidfGain[8]}, {u8"预测速度 Y", m_pidfGain[9]},
        {u8"移动死区 X", m_pidfInteger[0]}, {u8"移动死区 Y", m_pidfInteger[1]},
        {u8"移动限幅 X", m_pidfInteger[2]}, {u8"移动限幅 Y", m_pidfInteger[3]}
    }};
    for (int i = 0; i < static_cast<int>(fields.size()); ++i)
        grid->addWidget(FormKit::fieldRow(
            QString::fromUtf8(fields[i].first), fields[i].second), i / 2, i % 2);
    layout->addLayout(grid);

    for (auto* spin : m_pidfGain)
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    for (auto* spin : m_pidfInteger)
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);

    m_rightLayout->addWidget(card);
}

// Card: 瞄准轨迹（保持现有实现，不改变 AVA 控制链）
void HotkeyPage::buildAimPathCard()
{
    auto* card = new CardWidget(
        QStringLiteral("\xe7\x9e\x84\xe5\x87\x86\xe8\xbd\xa8\xe8\xbf\xb9"),
        QStringLiteral("activity"));
    card->setCollapsible(true);
    auto* cl = card->contentLayout();

    auto* hint = new QLabel(QStringLiteral(
        "\xe5\x85\x89\xe6\xa0\x87\xe4\xbb\x8e\xe5\xbd\x93\xe5\x89\x8d\xe4\xbd\x8d\xe7\xbd\xae"
        "\xe5\x88\xb0\xe7\x9b\xae\xe6\xa0\x87\xe7\x9a\x84\xe8\xbd\xa8\xe8\xbf\xb9\xe3\x80\x82"
        "X=\xe8\xbf\x9b\xe5\xba\xa6, Y=\xe5\x9e\x82\xe7\x9b\xb4\xe5\x81\x8f\xe7\xa6\xbb"));
    hint->setProperty("class", "secondary");
    hint->setWordWrap(true);
    cl->addWidget(hint);

    // ── Mode selector (radio row) ──
    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(12);
    m_aimPathModeLinear = new QRadioButton(QStringLiteral("\xe7\x9b\xb4\xe7\xba\xbf"));
    m_aimPathModeBezier = new QRadioButton(QStringLiteral("\xe8\xb4\x9d\xe5\xa1\x9e\xe5\xb0\x94"));
    m_aimPathModeCustom = new QRadioButton(QStringLiteral("\xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89"));
    m_aimPathModeLinear->setChecked(true);
    m_aimPathModeGroup = new QButtonGroup(this);
    m_aimPathModeGroup->addButton(m_aimPathModeLinear, 0);
    m_aimPathModeGroup->addButton(m_aimPathModeBezier, 1);
    m_aimPathModeGroup->addButton(m_aimPathModeCustom, 2);
    modeRow->addWidget(m_aimPathModeLinear);
    modeRow->addWidget(m_aimPathModeBezier);
    modeRow->addWidget(m_aimPathModeCustom);
    modeRow->addStretch();
    cl->addLayout(modeRow);

    m_aimPathInfluence = new QSpinBox;
    m_aimPathInfluence->setRange(0, 100);
    m_aimPathInfluence->setValue(25);
    m_aimPathInfluence->setSuffix(QStringLiteral("%"));
    m_aimPathInfluence->setToolTip(QString::fromUtf8(
        u8"曲线只按该比例影响 PIDF 原始方向；不会强制鼠标完全沿曲线行走。"));
    cl->addWidget(FormKit::fieldRow(
        QString::fromUtf8(u8"曲线影响"), m_aimPathInfluence));

    // ── Editor stack (only one visible per mode) ──
    m_aimPathEditorStack = new QStackedWidget;

    // Page 0 — Linear: just a placeholder note.
    auto* linearNote = new QLabel(QStringLiteral(
        "\xe7\x9b\xb4\xe7\xba\xbf\xe6\xa8\xa1\xe5\xbc\x8f\xe6\x97\xa0\xe9\x9c\x80"
        "\xe5\x8f\x82\xe6\x95\xb0\xe3\x80\x82"));
    linearNote->setProperty("class", "secondary");
    linearNote->setAlignment(Qt::AlignCenter);
    linearNote->setMinimumHeight(200);
    m_aimPathEditorStack->addWidget(linearNote);

    // Page 1 — Bezier editor.
    m_aimPathBezier = new BezierEditor;
    m_aimPathEditorStack->addWidget(m_aimPathBezier);

    // Page 2 — Freehand editor.
    m_aimPathFreehand = new FreehandCurveEditor;
    m_aimPathEditorStack->addWidget(m_aimPathFreehand);

    cl->addWidget(m_aimPathEditorStack);

    auto* neuralTrainButton = new QPushButton(QString::fromUtf8(u8"神经网络训练曲线"));
    neuralTrainButton->setToolTip(QString::fromUtf8(
        u8"通过多轮随机目标鼠标移动，学习你的平均轨迹并生成自定义曲线。"));
    cl->addWidget(neuralTrainButton);

    // ── Wiring ──
    auto onModeChanged = [this](int id) {
        if (id < 0) id = 0;
        if (id > 2) id = 2;
        m_aimPathEditorStack->setCurrentIndex(id);
        saveUiToCurrentProfile();
    };
    connect(m_aimPathModeGroup, &QButtonGroup::idClicked, this, onModeChanged);
    connect(m_aimPathInfluence, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { saveUiToCurrentProfile(); });

    connect(m_aimPathBezier, &BezierEditor::curveChanged,
            this, [this](float, float, float, float) {
                saveUiToCurrentProfile();
            });
    connect(m_aimPathFreehand, &FreehandCurveEditor::curveChanged,
            this, [this](const std::array<float, FreehandCurveEditor::kSampleCount>&) {
                m_neuralCurveActive = false;
                m_neuralCurveWeights.fill(0.0f);
                saveUiToCurrentProfile();
            });
    connect(neuralTrainButton, &QPushButton::clicked, this, [this] {
        auto* trainer = new NeuralCurveTrainerDialog(this);
        trainer->setAttribute(Qt::WA_DeleteOnClose);
        connect(trainer, &NeuralCurveTrainerDialog::curveTrained,
                this, [this](const std::array<float, NeuralCurveTrainerDialog::kSampleCount>& samples,
                             const std::array<float, 25>& weights) {
                    std::array<float, FreehandCurveEditor::kSampleCount> curve{};
                    std::copy(samples.begin(), samples.end(), curve.begin());
                    m_aimPathFreehand->setSamples(curve);
                    m_aimPathModeCustom->setChecked(true);
                    m_aimPathEditorStack->setCurrentIndex(2);
                    m_neuralCurveActive = true;
                    m_neuralCurveWeights = weights;
                    saveUiToCurrentProfile();
                });
        trainer->show();
        trainer->raise();
        trainer->activateWindow();
    });

    m_rightLayout->addWidget(card);
}


// ═══════════════════════════════════════════════════════════════════════════
// Group / Profile Management
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::reloadFromRuntime()
{
    rebuildGroupCombo();
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
    m_laserDetect->setChecked(hp.laser_detect_enabled);
    m_flashlightDetect->setChecked(hp.flashlight_detect_enabled);
    m_glassFilter->setChecked(hp.glass_filter_enabled);

    const float pg[10] = {hp.pidf_kp_x,hp.pidf_kp_y,0.0f,0.0f,hp.pidf_kd_x,
                          hp.pidf_kd_y,hp.pidf_kf_x,hp.pidf_kf_y,hp.pidf_lr_x,hp.pidf_lr_y};
    for (int i=0;i<10;++i) m_pidfGain[i]->setValue(pg[i]);
    const int pi[4] = {hp.pidf_deadzone_x,hp.pidf_deadzone_y,
                       hp.pidf_limit_x,hp.pidf_limit_y};
    for (int i=0;i<4;++i) m_pidfInteger[i]->setValue(pi[i]);

    m_lostTargetCacheFrames->setValue(hp.lost_target_cache_frames);

    // ── Trigger ──
    m_triggerEnabled->setChecked(hp.trigger_enabled);
    m_triggerFireDelay->setValue(hp.trigger_fire_delay);
    m_triggerFireDuration->setValue(hp.trigger_fire_duration);
    m_triggerFireInterval->setValue(hp.trigger_fire_interval);
    m_triggerYPercent->setValue(hp.trigger_y_percent);
    m_triggerDelayJitter->setValue(hp.trigger_delay_jitter_ms);
    m_triggerDurationJitter->setValue(hp.trigger_duration_jitter_ms);
    m_triggerIntervalJitter->setValue(hp.trigger_interval_jitter_ms);
    m_triggerSwitchCooldown->setValue(hp.trigger_switch_cooldown_ms);

    // 通过 rebuildAimClassList() 重建 (需要读 config)。loadProfileToUi 已经
    // 持有 configMutex, 而 rebuildAimClassList 会重新拿一次同一把递归锁,
    // 这里直接调没问题。
    rebuildAimClassList();


    // 用户 AimPath 配置。
    if (m_aimPathModeGroup)
    {
        int mode = std::clamp(hp.aim_path_mode, 0, 2);
        if (auto* btn = m_aimPathModeGroup->button(mode))
            btn->setChecked(true);
        m_aimPathEditorStack->setCurrentIndex(mode);
        m_aimPathInfluence->setValue(hp.aim_path_influence);
        m_aimPathBezier->setCurve(
            hp.aim_path_bezier_cx1, hp.aim_path_bezier_cy1,
            hp.aim_path_bezier_cx2, hp.aim_path_bezier_cy2);
        std::array<float, FreehandCurveEditor::kSampleCount> samples{};
        samples.fill(0.0f);
        static const std::vector<float> kEmptyCurve;
        const auto& source = hp.aim_path_custom_samples
            ? *hp.aim_path_custom_samples : kEmptyCurve;
        if (!source.empty()) {
            for (int i = 0; i < FreehandCurveEditor::kSampleCount; ++i) {
                const double pos = static_cast<double>(i) * (source.size() - 1)
                                 / (FreehandCurveEditor::kSampleCount - 1);
                const size_t i0 = static_cast<size_t>(std::floor(pos));
                const size_t i1 = std::min(i0 + 1, source.size() - 1);
                const double f = pos - static_cast<double>(i0);
                samples[i] = static_cast<float>(source[i0] + (source[i1] - source[i0]) * f);
            }
        }
        m_aimPathFreehand->setSamples(samples);
        m_neuralCurveActive = hp.aim_path_neural_enabled;
        m_neuralCurveWeights = hp.aim_path_neural_weights;
    }

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
    hp.laser_detect_enabled      = m_laserDetect->isChecked();
    hp.flashlight_detect_enabled = m_flashlightDetect->isChecked();
    hp.glass_filter_enabled      = m_glassFilter->isChecked();
    hp.pidf_kp_x=m_pidfGain[0]->value(); hp.pidf_kp_y=m_pidfGain[1]->value();
    hp.pidf_ki_x=0.0f; hp.pidf_ki_y=0.0f;
    hp.pidf_kd_x=m_pidfGain[4]->value(); hp.pidf_kd_y=m_pidfGain[5]->value();
    hp.pidf_kf_x=m_pidfGain[6]->value(); hp.pidf_kf_y=m_pidfGain[7]->value();
    hp.pidf_lr_x=m_pidfGain[8]->value(); hp.pidf_lr_y=m_pidfGain[9]->value();
    hp.pidf_deadzone_x=m_pidfInteger[0]->value(); hp.pidf_deadzone_y=m_pidfInteger[1]->value();
    hp.pidf_limit_x=m_pidfInteger[2]->value(); hp.pidf_limit_y=m_pidfInteger[3]->value();

    hp.lost_target_cache_frames = m_lostTargetCacheFrames->value();

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

    // 目标选择: 每条目的 class_id / y_offset / min_conf 由行内滑条回调直接写入
    // config.hotkeys[ri].aim_classes, 顺序由 ▲▼ 的 moveAimClass 维护
    // (见 rebuildAimClassList / moveAimClass), 此处无需再写。

    // 用户 AimPath 配置。
    if (m_aimPathModeGroup)
    {
        const int mode_id = m_aimPathModeGroup->checkedId();
        hp.aim_path_mode = (mode_id < 0) ? 0 : std::clamp(mode_id, 0, 2);
        hp.aim_path_influence = m_aimPathInfluence->value();
        hp.aim_path_bezier_cx1 = m_aimPathBezier->cx1();
        hp.aim_path_bezier_cy1 = m_aimPathBezier->cy1();
        hp.aim_path_bezier_cx2 = m_aimPathBezier->cx2();
        hp.aim_path_bezier_cy2 = m_aimPathBezier->cy2();
        const auto& samples = m_aimPathFreehand->samples();
        hp.aim_path_custom_samples = std::make_shared<const std::vector<float>>(
            samples.begin(), samples.end());
        hp.aim_path_neural_enabled = m_neuralCurveActive;
        hp.aim_path_neural_weights = m_neuralCurveWeights;
    }

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
