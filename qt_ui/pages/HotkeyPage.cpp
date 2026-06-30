#include "pages/HotkeyPage.h"
#include "pages/TargetPage.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "widgets/BezierEditor.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/FreehandCurveEditor.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"

#include <QShowEvent>

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
#include <cmath>
#include <mutex>
#include <set>
#include <string>

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
}

void HotkeyPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
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
        "QPushButton{font-size:16px; color:#8E8E96; background:transparent;"
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
        "QPushButton{font-family:\"tabler-icons\"; font-size:16px; color:#8E8E96;"
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
    m_rightLayout->setContentsMargins(12, 8, 12, 8);
    m_rightLayout->setSpacing(12);

    buildKeyBindCard();
    buildFovCard();
    buildDeadzoneCard();
    buildTriggerCard();
    buildTargetSelectionCard();
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
    dynLay->addWidget(FormKit::sliderRowD(QStringLiteral("\xe5\x86\x85\xe8\xbe\xb9\xe8\xb7\x9d\xe7\xb3\xbb\xe6\x95\xb0"),
                                          1.0, 2.0, 1.10, 0.01, 2, marginSl, m_dynamicFovMargin));
    QSlider* minRadSl = nullptr;
    dynLay->addWidget(FormKit::sliderRowD(QStringLiteral("\xe6\x9c\x80\xe5\xb0\x8f\xe5\x8d\x8a\xe5\xbe\x84\xe7\xb3\xbb\xe6\x95\xb0"),
                                          0.05, 1.0, 0.20, 0.01, 2, minRadSl, m_dynamicFovMinRadius));
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
// Card: 死区 (shared deadzone)
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildDeadzoneCard()
{
    auto* card = new CardWidget(
        QStringLiteral("\xe6\xad\xbb\xe5\x8c\xba"),        // 死区
        QStringLiteral("target"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8"),         // 启用
        false, m_deadzoneEnabled));

    QSlider* dzSl = nullptr;
    QSpinBox* dzSp = nullptr;
    cl->addWidget(FormKit::sliderRow(
        QStringLiteral("\xe6\xad\xbb\xe5\x8c\xba\xe6\xaf\x94\xe4\xbe\x8b (%)"),  // 死区比例 (%)
        0, 100, 0, dzSl, dzSp));
    m_deadzonePercent = dzSl;

    connect(m_deadzoneEnabled, &ToggleSwitch::toggled,
            this, [this] { saveUiToCurrentProfile(); });
    connect(dzSp, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });

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

    m_triggerFireDelay    = makeSpin(0,    5000, QStringLiteral(" ms"));
    m_triggerFireDuration = makeSpin(1,    5000, QStringLiteral(" ms"));
    m_triggerFireInterval = makeSpin(0,    5000, QStringLiteral(" ms"));
    m_triggerYPercent     = makeSpin(1,     100, QStringLiteral(" %"));

    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\xbb\xb6\xe8\xbf\x9f"),                   // 延迟
        m_triggerFireDelay));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe6\x8c\x89\xe4\xbd\x8f\xe6\x97\xb6\xe9\x95\xbf"),  // 按住时长
        m_triggerFireDuration));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\x86\xb7\xe5\x8d\xb4"),                   // 冷却
        m_triggerFireInterval));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe5\x91\xbd\xe4\xb8\xad\xe5\x8c\xba"),       // 命中区
        m_triggerYPercent));

    connect(m_triggerEnabled, &ToggleSwitch::toggled,
            this, [this] { saveUiToCurrentProfile(); });
    connect(m_triggerFireDelay, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });
    connect(m_triggerFireDuration, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });
    connect(m_triggerFireInterval, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });
    connect(m_triggerYPercent, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: 目标选择 (3-slot target selection)
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildTargetSelectionCard()
{
    auto* card = new CardWidget(
        QStringLiteral("\xe7\x9b\xae\xe6\xa0\x87\xe9\x80\x89\xe6\x8b\xa9"),  // 目标选择
        QStringLiteral("list-numbers"));
    auto* cl = card->contentLayout();

    // Helper: build one target slot
    auto buildSlot = [&](const QString& header,
                         QSpinBox*& classSpin,
                         QSlider*& yTopSlider,
                         QSlider*& yBotSlider,
                         QSlider*& confSlider) {
        auto* hdr = new QLabel(header);
        hdr->setStyleSheet("color:#5E6AD2; font-size:12px; font-weight:600;");
        cl->addWidget(hdr);

        classSpin = new QSpinBox;
        classSpin->setRange(0, 99);
        classSpin->setMinimumHeight(30);
        cl->addWidget(FormKit::fieldRow(
            QStringLiteral("\xe7\xb1\xbb\xe5\x88\xab ID"),             // 类别 ID
            classSpin));

        QSpinBox* ytSp = nullptr;
        cl->addWidget(FormKit::sliderRow(
            QStringLiteral("Y \xe9\xa1\xb6\xe9\x83\xa8 (%)"),         // Y 顶部 (%)
            0, 100, 0, yTopSlider, ytSp));

        QSpinBox* ybSp = nullptr;
        cl->addWidget(FormKit::sliderRow(
            QStringLiteral("Y \xe5\xba\x95\xe9\x83\xa8 (%)"),         // Y 底部 (%)
            0, 100, 100, yBotSlider, ybSp));

        QSpinBox* cfSp = nullptr;
        cl->addWidget(FormKit::sliderRow(
            QStringLiteral("\xe6\x9c\x80\xe5\xb0\x8f\xe7\xbd\xae\xe4\xbf\xa1\xe5\xba\xa6 (%)"),  // 最小置信度 (%)
            0, 100, 0, confSlider, cfSp));

        // Connect all to save
        connect(classSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
        connect(ytSp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
        connect(ybSp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
        connect(cfSp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this] { saveUiToCurrentProfile(); });
    };

    buildSlot(QStringLiteral("\xe6\xa7\xbd\xe4\xbd\x8d 1 (\xe6\x9c\x80\xe9\xab\x98\xe4\xbc\x98\xe5\x85\x88\xe7\xba\xa7)"),  // 槽位 1 (最高优先级)
              m_targetClass1, m_targetYTop1, m_targetYBot1, m_targetMinConf1);
    buildSlot(QStringLiteral("\xe6\xa7\xbd\xe4\xbd\x8d 2"),                                                                     // 槽位 2
              m_targetClass2, m_targetYTop2, m_targetYBot2, m_targetMinConf2);
    buildSlot(QStringLiteral("\xe6\xa7\xbd\xe4\xbd\x8d 3"),                                                                     // 槽位 3
              m_targetClass3, m_targetYTop3, m_targetYBot3, m_targetMinConf3);

    // Aim range
    m_targetAimRange = new QSpinBox;
    m_targetAimRange->setRange(1, 9999);
    m_targetAimRange->setMinimumHeight(30);
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe6\x9c\x80\xe5\xa4\xa7\xe7\x9e\x84\xe5\x87\x86\xe8\xb7\x9d\xe7\xa6\xbb (px)"),  // 最大瞄准距离 (px)
        m_targetAimRange));
    connect(m_targetAimRange, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this] { saveUiToCurrentProfile(); });

    m_rightLayout->addWidget(card);
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
        QStringLiteral("启用寻光检测 (此热键)"),
        false, m_flashlightDetect));
    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("启用玻璃过滤 (此热键)"),
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
        QStringLiteral("\xe5\x90\xb8\xe9\x99\x84\xe9\x94\x81\xe6\xad\xbb\xe7\x9e\x84\xe5\x87\x86"),
        QStringLiteral("adjustments"));
    auto* cl = card->contentLayout();

    // 移动控制器选择 (mover_kind): 顶部下拉。下面 QStackedWidget 跟着切。
    m_moverKindCombo = new QComboBox;
    m_moverKindCombo->setMinimumHeight(30);
    m_moverKindCombo->addItem(
        QStringLiteral("\xe5\xbe\xae\xe6\xbe\x9c"));        // 微澜
    m_moverKindCombo->addItem(
        QStringLiteral("\xe7\x96\xbe\xe9\xa3\x8e"));        // 疾风
    m_moverKindCombo->addItem(
        QStringLiteral("\xe5\xa4\xa9\xe6\x9e\xa2"));        // 天枢
    m_moverKindCombo->setToolTip(QStringLiteral(
        "\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x8e\xa7\xe5\x88\xb6\xe5\x99\xa8\xef\xbc\x9a\n"
        "\xe5\xbe\xae\xe6\xbe\x9c\xef\xbc\x9a" "ART \xe9\xbb\x98\xe8\xae\xa4\xe8\xb7\xaf\xe5\xbe\x84(EMA \xe5\xb9\xb3\xe6\xbb\x91)\n"
        "\xe7\x96\xbe\xe9\xa3\x8e\xef\xbc\x9a\xe4\xbd\x8d\xe7\xbd\xae\xe5\xbc\x8f PID + \xe5\xaf\xbc\xe6\x95\xb0\xe9\xa2\x84\xe6\xb5\x8b\n"
        "\xe5\xa4\xa9\xe6\x9e\xa2\xef\xbc\x9a\xe7\xbb\x8f\xe5\x85\xb8\xe5\x85\xa8\xe5\x8f\x82 PID + \xe5\x8a\xa8\xe6\x80\x81 KP + EMA/Kalman"));
    cl->addWidget(FormKit::fieldRow(
        QStringLiteral("\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x8e\xa7\xe5\x88\xb6\xe5\x99\xa8"),
        m_moverKindCombo));

    m_moverParamStack = new QStackedWidget;

    // ── Page 0: 微澜 (Smooth) ART 三参数 ──
    {
        auto* page = new QWidget;
        auto* pl = new QVBoxLayout(page);
        pl->setContentsMargins(0, 0, 0, 0);
        pl->setSpacing(6);

        QSlider* sxSl = nullptr;
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe7\x81\xb5\xe6\x95\x8f\xe5\xba\xa6 X"),  // 灵敏度 X
            0.0, 2.0, 0.6, 0.01, 2, sxSl, m_speedXSpin));

        QSlider* sySl = nullptr;
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe7\x81\xb5\xe6\x95\x8f\xe5\xba\xa6 Y"),  // 灵敏度 Y
            0.0, 2.0, 0.6, 0.01, 2, sySl, m_speedYSpin));

        QSlider* dzSl = nullptr;
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe6\xad\xbb\xe5\x8c\xba"),                // 死区
            0.0, 20.0, 2.0, 0.1, 1, dzSl, m_deadZoneSpin));

        m_moverParamStack->addWidget(page);
    }

    // ── Page 1: 疾风 (Predictive) ──
    {
        auto* page = new QWidget;
        auto* pl = new QVBoxLayout(page);
        pl->setContentsMargins(0, 0, 0, 0);
        pl->setSpacing(6);

        QSlider* s = nullptr;
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe7\x81\xb5\xe6\x95\x8f\xe5\xba\xa6 X"),  // 灵敏度 X (Kp X)
            0.0, 5.0, 0.6, 0.01, 2, s, m_predKpXSpin));
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe7\x81\xb5\xe6\x95\x8f\xe5\xba\xa6 Y"),  // 灵敏度 Y (Kp Y)
            0.0, 5.0, 0.6, 0.01, 2, s, m_predKpYSpin));
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe9\x98\xbb\xe5\xb0\xbc"),                // 阻尼 (Kd)
            0.0, 1.0, 0.10, 0.001, 3, s, m_predKdSpin));
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe9\xa2\x84\xe6\xb5\x8b\xe6\x9d\x83\xe9\x87\x8d"),  // 预测权重
            0.0, 2.0, 0.5, 0.01, 2, s, m_predPwSpin));

        m_moverParamStack->addWidget(page);
    }

    // ── Page 2: 天枢 (Classic) — 经典全参 PID ──
    {
        auto* page = new QWidget;
        auto* pl = new QVBoxLayout(page);
        pl->setContentsMargins(0, 0, 0, 0);
        pl->setSpacing(6);

        // 瞄准模式选择
        m_clsAimModeCombo = new QComboBox;
        m_clsAimModeCombo->addItem(QStringLiteral("\xe7\xae\x80\xe5\x8d\x95"));  // 简单
        m_clsAimModeCombo->addItem(QStringLiteral("\xe9\xab\x98\xe7\xba\xa7"));  // 高级
        pl->addWidget(FormKit::fieldRow(
            QStringLiteral("PID \xe6\xa8\xa1\xe5\xbc\x8f"), m_clsAimModeCombo));  // PID 模式

        m_clsAimModeStack = new QStackedWidget;

        // ── 简单模式 ──
        {
            auto* sp = new QWidget;
            auto* sl = new QVBoxLayout(sp);
            sl->setContentsMargins(0, 0, 0, 0);
            sl->setSpacing(6);
            QSlider* s = nullptr;
            sl->addWidget(FormKit::sliderRowD(
                QStringLiteral("\xe8\xb5\xb7\xe5\xa7\x8b\xe9\x80\x9f\xe5\xba\xa6"),  // 起始速度
                0.0, 5.0, 0.3, 0.01, 2, s, m_clsStartSpeed));
            sl->addWidget(FormKit::sliderRowD(
                QStringLiteral("\xe7\xbb\x88\xe6\xad\xa2\xe9\x80\x9f\xe5\xba\xa6"),  // 终止速度
                0.0, 5.0, 0.8, 0.01, 2, s, m_clsEndSpeed));
            {   QSlider* sl_ = nullptr;
                sl->addWidget(FormKit::sliderRow(
                    QStringLiteral("\xe6\xb8\x90\xe5\x8f\x98\xe6\x97\xb6\xe9\x95\xbf(ms)"),  // 渐变时长(ms)
                    0, 10000, 0, sl_, m_clsTransitionMs));
            }
            sl->addWidget(FormKit::sliderRowD(
                QStringLiteral("KI"),
                0.0, 1.0, 0.0, 0.001, 3, s, m_clsSimpleKi));
            sl->addWidget(FormKit::sliderRowD(
                QStringLiteral("KD"),
                0.0, 2.0, 0.0, 0.01, 2, s, m_clsSimpleKd));
            m_clsAimModeStack->addWidget(sp);
        }

        // ── 高级模式 ──
        {
            auto* ap = new QWidget;
            auto* al = new QVBoxLayout(ap);
            al->setContentsMargins(0, 0, 0, 0);
            al->setSpacing(4);
            QSlider* s = nullptr;

            al->addWidget(new QLabel(QStringLiteral("── X \xe8\xbd\xb4 ──")));  // ── X 轴 ──
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KP Min X"), 0.0, 5.0, 0.3, 0.01, 2, s, m_clsKpMinX));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KP Max X"), 0.0, 5.0, 0.8, 0.01, 2, s, m_clsKpMaxX));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KI X"),     0.0, 1.0, 0.0, 0.001, 3, s, m_clsKiX));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KD X"),     0.0, 2.0, 0.0, 0.01, 2, s, m_clsKdX));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("I Max X"),  0.0, 100.0, 0.0, 0.1, 1, s, m_clsImaxX));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("P Factor X"), 0.1, 5.0, 1.0, 0.01, 2, s, m_clsPfactorX));
            {   QSlider* sl_ = nullptr;
                al->addWidget(FormKit::sliderRow(
                    QStringLiteral("\xe6\x97\xb6\xe9\x97\xb4 KP X(ms)"), 0, 10000, 0, sl_, m_clsTimeX));  // 时间 KP X(ms)
            }
            m_clsTimeDynX = new ToggleSwitch;
            al->addWidget(FormKit::fieldRow(
                QStringLiteral("\xe6\x97\xb6\xe9\x97\xb4\xe5\x8a\xa8\xe6\x80\x81 X"), m_clsTimeDynX));  // 时间动态 X

            al->addWidget(new QLabel(QStringLiteral("── Y \xe8\xbd\xb4 ──")));  // ── Y 轴 ──
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KP Min Y"), 0.0, 5.0, 0.3, 0.01, 2, s, m_clsKpMinY));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KP Max Y"), 0.0, 5.0, 0.8, 0.01, 2, s, m_clsKpMaxY));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KI Y"),     0.0, 1.0, 0.0, 0.001, 3, s, m_clsKiY));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("KD Y"),     0.0, 2.0, 0.0, 0.01, 2, s, m_clsKdY));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("I Max Y"),  0.0, 100.0, 0.0, 0.1, 1, s, m_clsImaxY));
            al->addWidget(FormKit::sliderRowD(QStringLiteral("P Factor Y"), 0.1, 5.0, 1.0, 0.01, 2, s, m_clsPfactorY));
            {   QSlider* sl_ = nullptr;
                al->addWidget(FormKit::sliderRow(
                    QStringLiteral("\xe6\x97\xb6\xe9\x97\xb4 KP Y(ms)"), 0, 10000, 0, sl_, m_clsTimeY));
            }
            m_clsTimeDynY = new ToggleSwitch;
            al->addWidget(FormKit::fieldRow(
                QStringLiteral("\xe6\x97\xb6\xe9\x97\xb4\xe5\x8a\xa8\xe6\x80\x81 Y"), m_clsTimeDynY));

            m_clsAimModeStack->addWidget(ap);
        }
        pl->addWidget(m_clsAimModeStack);

        // 预测
        m_clsPredModeCombo = new QComboBox;
        m_clsPredModeCombo->addItem(QStringLiteral("\xe6\x97\xa0"));     // 无
        m_clsPredModeCombo->addItem(QStringLiteral("EMA"));
        m_clsPredModeCombo->addItem(QStringLiteral("Kalman"));
        pl->addWidget(FormKit::fieldRow(
            QStringLiteral("\xe9\xa2\x84\xe6\xb5\x8b\xe6\xa8\xa1\xe5\xbc\x8f"), m_clsPredModeCombo));  // 预测模式

        QSlider* s = nullptr;
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe9\xa2\x84\xe6\xb5\x8b\xe5\xb8\xa7\xe6\x95\xb0"),  // 预测帧数
            0.0, 10.0, 1.0, 0.1, 1, s, m_clsVelLead));
        m_clsIndependentY = new ToggleSwitch;
        pl->addWidget(FormKit::fieldRow(
            QStringLiteral("Y \xe8\xbd\xb4\xe7\x8b\xac\xe7\xab\x8b"), m_clsIndependentY));  // Y 轴独立

        // Kalman 参数容器(仅预测模式=2时显示)
        m_clsKalmanContainer = new QWidget;
        {
            auto* kl = new QVBoxLayout(m_clsKalmanContainer);
            kl->setContentsMargins(0, 0, 0, 0);
            kl->setSpacing(6);
            kl->addWidget(FormKit::sliderRowD(QStringLiteral("Q Pos"),       0.001, 100.0, 1.0, 0.01, 2, s, m_clsKalmanQPos));
            kl->addWidget(FormKit::sliderRowD(QStringLiteral("Q Vel"),       0.001, 100.0, 1.0, 0.01, 2, s, m_clsKalmanQVel));
            kl->addWidget(FormKit::sliderRowD(QStringLiteral("R Obs"),       0.001, 100.0, 1.0, 0.01, 2, s, m_clsKalmanRObs));
            kl->addWidget(FormKit::sliderRowD(
                QStringLiteral("Lookahead(ms)"), 0.0, 100.0, 2.0, 0.1, 1, s, m_clsKalmanLookahead));
        }
        pl->addWidget(m_clsKalmanContainer);

        // 死区
        m_clsDeadzoneEnabled = new ToggleSwitch;
        pl->addWidget(FormKit::fieldRow(
            QStringLiteral("\xe6\xad\xbb\xe5\x8c\xba"), m_clsDeadzoneEnabled));  // 死区
        pl->addWidget(FormKit::sliderRowD(
            QStringLiteral("\xe6\xad\xbb\xe5\x8c\xba %"),  // 死区 %
            0.0, 100.0, 0.0, 0.1, 1, s, m_clsDeadzonePercent));

        m_moverParamStack->addWidget(page);
    }

    cl->addWidget(m_moverParamStack);

    // ── Wiring: combo 联动 stack + 写回 profile ──
    connect(m_moverKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (idx < 0) idx = 0;
                if (idx > 2) idx = 2;
                m_moverParamStack->setCurrentIndex(idx);
                saveUiToCurrentProfile();
            });

    auto wireD = [this](QDoubleSpinBox* sp) {
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    };
    auto wireI = [this](QSpinBox* sp) {
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    };
    auto wireT = [this](ToggleSwitch* sw) {
        connect(sw, &ToggleSwitch::toggled,
                this, &HotkeyPage::saveUiToCurrentProfile);
    };
    auto wireC = [this](QComboBox* cb) {
        connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &HotkeyPage::saveUiToCurrentProfile);
    };
    // 微澜
    wireD(m_speedXSpin);
    wireD(m_speedYSpin);
    wireD(m_deadZoneSpin);
    // 疾风
    wireD(m_predKpXSpin);
    wireD(m_predKpYSpin);
    wireD(m_predKdSpin);
    wireD(m_predPwSpin);
    // 天枢
    connect(m_clsAimModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                m_clsAimModeStack->setCurrentIndex(std::clamp(idx, 0, 1));
                saveUiToCurrentProfile();
            });
    wireD(m_clsStartSpeed); wireD(m_clsEndSpeed);
    wireI(m_clsTransitionMs);
    wireD(m_clsSimpleKi); wireD(m_clsSimpleKd);
    wireD(m_clsKpMinX); wireD(m_clsKpMaxX);
    wireD(m_clsKiX); wireD(m_clsKdX);
    wireD(m_clsImaxX); wireD(m_clsPfactorX);
    wireI(m_clsTimeX); wireT(m_clsTimeDynX);
    wireD(m_clsKpMinY); wireD(m_clsKpMaxY);
    wireD(m_clsKiY); wireD(m_clsKdY);
    wireD(m_clsImaxY); wireD(m_clsPfactorY);
    wireI(m_clsTimeY); wireT(m_clsTimeDynY);
    connect(m_clsPredModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                m_clsKalmanContainer->setVisible(idx == 2);
                saveUiToCurrentProfile();
            });
    wireD(m_clsVelLead); wireT(m_clsIndependentY);
    wireD(m_clsKalmanQPos); wireD(m_clsKalmanQVel);
    wireD(m_clsKalmanRObs); wireD(m_clsKalmanLookahead);
    wireT(m_clsDeadzoneEnabled); wireD(m_clsDeadzonePercent);

    m_rightLayout->addWidget(card);
}


// ═══════════════════════════════════════════════════════════════════════════
// Card: 瞄准轨迹 (aim path)
// ═══════════════════════════════════════════════════════════════════════════

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

    // ── Wiring ──
    auto onModeChanged = [this](int id) {
        if (id < 0) id = 0;
        if (id > 2) id = 2;
        m_aimPathEditorStack->setCurrentIndex(id);
        saveUiToCurrentProfile();
    };
    connect(m_aimPathModeGroup, &QButtonGroup::idClicked, this, onModeChanged);

    connect(m_aimPathBezier, &BezierEditor::curveChanged,
            this, [this](float, float, float, float) {
                saveUiToCurrentProfile();
            });
    connect(m_aimPathFreehand, &FreehandCurveEditor::curveChanged,
            this, [this](const std::array<float, FreehandCurveEditor::kSampleCount>&) {
                saveUiToCurrentProfile();
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

    {
        const int mk = std::clamp(hp.mover_kind, 0, 2);
        m_moverKindCombo->setCurrentIndex(mk);
        m_moverParamStack->setCurrentIndex(mk);
    }
    m_speedXSpin->setValue(static_cast<double>(hp.speed_x));
    m_speedYSpin->setValue(static_cast<double>(hp.speed_y));
    m_deadZoneSpin->setValue(static_cast<double>(hp.dead_zone_px));
    m_predKpXSpin->setValue(static_cast<double>(hp.predictive_kp_x));
    m_predKpYSpin->setValue(static_cast<double>(hp.predictive_kp_y));
    m_predKdSpin->setValue(static_cast<double>(hp.predictive_kd));
    m_predPwSpin->setValue(static_cast<double>(hp.predictive_pred_weight));
    // 天枢
    m_clsAimModeCombo->setCurrentIndex(std::clamp(hp.classic_aim_mode, 0, 1));
    m_clsAimModeStack->setCurrentIndex(std::clamp(hp.classic_aim_mode, 0, 1));
    m_clsStartSpeed->setValue(static_cast<double>(hp.classic_simple_start_speed));
    m_clsEndSpeed->setValue(static_cast<double>(hp.classic_simple_end_speed));
    m_clsTransitionMs->setValue(hp.classic_simple_transition_ms);
    m_clsSimpleKi->setValue(static_cast<double>(hp.classic_simple_ki));
    m_clsSimpleKd->setValue(static_cast<double>(hp.classic_simple_kd));
    m_clsKpMinX->setValue(static_cast<double>(hp.classic_adv_kpmin_x));
    m_clsKpMaxX->setValue(static_cast<double>(hp.classic_adv_kpmax_x));
    m_clsKiX->setValue(static_cast<double>(hp.classic_adv_ki_x));
    m_clsKdX->setValue(static_cast<double>(hp.classic_adv_kd_x));
    m_clsImaxX->setValue(static_cast<double>(hp.classic_adv_imax_x));
    m_clsPfactorX->setValue(static_cast<double>(hp.classic_adv_pfactor_x));
    m_clsTimeX->setValue(hp.classic_adv_time_x);
    m_clsTimeDynX->setChecked(hp.classic_adv_time_dynamic_x);
    m_clsKpMinY->setValue(static_cast<double>(hp.classic_adv_kpmin_y));
    m_clsKpMaxY->setValue(static_cast<double>(hp.classic_adv_kpmax_y));
    m_clsKiY->setValue(static_cast<double>(hp.classic_adv_ki_y));
    m_clsKdY->setValue(static_cast<double>(hp.classic_adv_kd_y));
    m_clsImaxY->setValue(static_cast<double>(hp.classic_adv_imax_y));
    m_clsPfactorY->setValue(static_cast<double>(hp.classic_adv_pfactor_y));
    m_clsTimeY->setValue(hp.classic_adv_time_y);
    m_clsTimeDynY->setChecked(hp.classic_adv_time_dynamic_y);
    m_clsPredModeCombo->setCurrentIndex(std::clamp(hp.classic_prediction_mode, 0, 2));
    m_clsVelLead->setValue(static_cast<double>(hp.classic_velocity_lead_frames));
    m_clsIndependentY->setChecked(hp.classic_independent_y);
    m_clsKalmanQPos->setValue(static_cast<double>(hp.classic_kalman_q_pos));
    m_clsKalmanQVel->setValue(static_cast<double>(hp.classic_kalman_q_vel));
    m_clsKalmanRObs->setValue(static_cast<double>(hp.classic_kalman_r_obs));
    m_clsKalmanLookahead->setValue(static_cast<double>(hp.classic_kalman_lookahead));
    m_clsKalmanContainer->setVisible(hp.classic_prediction_mode == 2);
    m_clsDeadzoneEnabled->setChecked(hp.classic_deadzone_enabled);
    m_clsDeadzonePercent->setValue(static_cast<double>(hp.classic_deadzone_percent));

    // ── Deadzone ──
    m_deadzoneEnabled->setChecked(hp.deadzone_enabled);
    m_deadzonePercent->setValue(static_cast<int>(hp.deadzone_percent));

    // ── Trigger ──
    m_triggerEnabled->setChecked(hp.trigger_enabled);
    m_triggerFireDelay->setValue(hp.trigger_fire_delay);
    m_triggerFireDuration->setValue(hp.trigger_fire_duration);
    m_triggerFireInterval->setValue(hp.trigger_fire_interval);
    m_triggerYPercent->setValue(hp.trigger_y_percent);

    // ── Target selection ──
    m_targetClass1->setValue(hp.target_class_1);
    m_targetYTop1->setValue(static_cast<int>(hp.target_y_top_1 * 100.0f));
    m_targetYBot1->setValue(static_cast<int>(hp.target_y_bot_1 * 100.0f));
    m_targetMinConf1->setValue(static_cast<int>(hp.target_min_conf_1 * 100.0f));
    m_targetClass2->setValue(hp.target_class_2);
    m_targetYTop2->setValue(static_cast<int>(hp.target_y_top_2 * 100.0f));
    m_targetYBot2->setValue(static_cast<int>(hp.target_y_bot_2 * 100.0f));
    m_targetMinConf2->setValue(static_cast<int>(hp.target_min_conf_2 * 100.0f));
    m_targetClass3->setValue(hp.target_class_3);
    m_targetYTop3->setValue(static_cast<int>(hp.target_y_top_3 * 100.0f));
    m_targetYBot3->setValue(static_cast<int>(hp.target_y_bot_3 * 100.0f));
    m_targetMinConf3->setValue(static_cast<int>(hp.target_min_conf_3 * 100.0f));
    m_targetAimRange->setValue(hp.target_aim_range);

    // ── Aim trajectory ──
    {
        int mode = std::clamp(hp.aim_path_mode, 0, 2);
        if (auto* btn = m_aimPathModeGroup->button(mode))
            btn->setChecked(true);
        m_aimPathEditorStack->setCurrentIndex(mode);
        m_aimPathBezier->setCurve(
            hp.aim_path_bezier_cx1, hp.aim_path_bezier_cy1,
            hp.aim_path_bezier_cx2, hp.aim_path_bezier_cy2);
        std::array<float, FreehandCurveEditor::kSampleCount> samples{};
        samples.fill(0.0f);
        for (int i = 0; i < FreehandCurveEditor::kSampleCount; ++i) {
            if (i < static_cast<int>(hp.aim_path_custom_samples.size()))
                samples[i] = hp.aim_path_custom_samples[i];
        }
        m_aimPathFreehand->setSamples(samples);
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
    hp.mover_kind      = std::clamp(m_moverKindCombo->currentIndex(), 0, 2);
    hp.speed_x         = static_cast<float>(m_speedXSpin->value());
    hp.speed_y         = static_cast<float>(m_speedYSpin->value());
    hp.dead_zone_px    = static_cast<float>(m_deadZoneSpin->value());
    hp.predictive_kp_x        = static_cast<float>(m_predKpXSpin->value());
    hp.predictive_kp_y        = static_cast<float>(m_predKpYSpin->value());
    hp.predictive_kd          = static_cast<float>(m_predKdSpin->value());
    hp.predictive_pred_weight = static_cast<float>(m_predPwSpin->value());
    // 天枢
    hp.classic_aim_mode = std::clamp(m_clsAimModeCombo->currentIndex(), 0, 1);
    hp.classic_simple_start_speed  = static_cast<float>(m_clsStartSpeed->value());
    hp.classic_simple_end_speed    = static_cast<float>(m_clsEndSpeed->value());
    hp.classic_simple_transition_ms= m_clsTransitionMs->value();
    hp.classic_simple_ki           = static_cast<float>(m_clsSimpleKi->value());
    hp.classic_simple_kd           = static_cast<float>(m_clsSimpleKd->value());
    hp.classic_adv_kpmin_x   = static_cast<float>(m_clsKpMinX->value());
    hp.classic_adv_kpmax_x   = static_cast<float>(m_clsKpMaxX->value());
    hp.classic_adv_ki_x      = static_cast<float>(m_clsKiX->value());
    hp.classic_adv_kd_x      = static_cast<float>(m_clsKdX->value());
    hp.classic_adv_imax_x    = static_cast<float>(m_clsImaxX->value());
    hp.classic_adv_pfactor_x = static_cast<float>(m_clsPfactorX->value());
    hp.classic_adv_time_x    = m_clsTimeX->value();
    hp.classic_adv_time_dynamic_x = m_clsTimeDynX->isChecked();
    hp.classic_adv_kpmin_y   = static_cast<float>(m_clsKpMinY->value());
    hp.classic_adv_kpmax_y   = static_cast<float>(m_clsKpMaxY->value());
    hp.classic_adv_ki_y      = static_cast<float>(m_clsKiY->value());
    hp.classic_adv_kd_y      = static_cast<float>(m_clsKdY->value());
    hp.classic_adv_imax_y    = static_cast<float>(m_clsImaxY->value());
    hp.classic_adv_pfactor_y = static_cast<float>(m_clsPfactorY->value());
    hp.classic_adv_time_y    = m_clsTimeY->value();
    hp.classic_adv_time_dynamic_y = m_clsTimeDynY->isChecked();
    hp.classic_prediction_mode       = std::clamp(m_clsPredModeCombo->currentIndex(), 0, 2);
    hp.classic_velocity_lead_frames  = static_cast<float>(m_clsVelLead->value());
    hp.classic_independent_y         = m_clsIndependentY->isChecked();
    hp.classic_kalman_q_pos      = static_cast<float>(m_clsKalmanQPos->value());
    hp.classic_kalman_q_vel      = static_cast<float>(m_clsKalmanQVel->value());
    hp.classic_kalman_r_obs      = static_cast<float>(m_clsKalmanRObs->value());
    hp.classic_kalman_lookahead  = static_cast<float>(m_clsKalmanLookahead->value());
    hp.classic_deadzone_enabled  = m_clsDeadzoneEnabled->isChecked();
    hp.classic_deadzone_percent  = static_cast<float>(m_clsDeadzonePercent->value());

    // ── Deadzone ──
    hp.deadzone_enabled = m_deadzoneEnabled->isChecked();
    hp.deadzone_percent = static_cast<float>(m_deadzonePercent->value());

    // ── Trigger ──
    hp.trigger_enabled       = m_triggerEnabled->isChecked();
    hp.trigger_fire_delay    = m_triggerFireDelay->value();
    hp.trigger_fire_duration = m_triggerFireDuration->value();
    hp.trigger_fire_interval = m_triggerFireInterval->value();
    hp.trigger_y_percent     = m_triggerYPercent->value();

    // ── Target selection ──
    hp.target_class_1    = m_targetClass1->value();
    hp.target_y_top_1    = m_targetYTop1->value() / 100.0f;
    hp.target_y_bot_1    = m_targetYBot1->value() / 100.0f;
    hp.target_min_conf_1 = m_targetMinConf1->value() / 100.0f;
    hp.target_class_2    = m_targetClass2->value();
    hp.target_y_top_2    = m_targetYTop2->value() / 100.0f;
    hp.target_y_bot_2    = m_targetYBot2->value() / 100.0f;
    hp.target_min_conf_2 = m_targetMinConf2->value() / 100.0f;
    hp.target_class_3    = m_targetClass3->value();
    hp.target_y_top_3    = m_targetYTop3->value() / 100.0f;
    hp.target_y_bot_3    = m_targetYBot3->value() / 100.0f;
    hp.target_min_conf_3 = m_targetMinConf3->value() / 100.0f;
    hp.target_aim_range  = m_targetAimRange->value();

    // ── Aim trajectory ──
    {
        const int mode_id = m_aimPathModeGroup->checkedId();
        hp.aim_path_mode = (mode_id < 0) ? 0 : std::clamp(mode_id, 0, 2);
        hp.aim_path_bezier_cx1 = m_aimPathBezier->cx1();
        hp.aim_path_bezier_cy1 = m_aimPathBezier->cy1();
        hp.aim_path_bezier_cx2 = m_aimPathBezier->cx2();
        hp.aim_path_bezier_cy2 = m_aimPathBezier->cy2();
        const auto& samples = m_aimPathFreehand->samples();
        hp.aim_path_custom_samples.assign(samples.begin(), samples.end());
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
