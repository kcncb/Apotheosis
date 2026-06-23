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
    if (tp)
        connect(tp, &TargetPage::classFiltersChanged,
                this, &HotkeyPage::onAimClassFiltersChanged);
}

void HotkeyPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
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
    buildAimClassCard();
    buildCrosshairCard();
    buildBossAimCard();
    buildAimPathCard();
    buildSmartTriggerCard();
    buildAdvancedCard();

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
// Card 3: Aim Class Selection  ← 核心新增
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildAimClassCard()
{
    m_aimClassCard = new CardWidget(
        QStringLiteral("\xe7\x9e\x84\xe5\x87\x86\xe7\xb1\xbb\xe5\x88\xab (\xe4\xbc\x98\xe5\x85\x88\xe7\xba\xa7\xe6\x8e\x92\xe5\xba\x8f)"),
        QStringLiteral("list-numbers"));
    auto* cl = m_aimClassCard->contentLayout();

    auto* hint = new QLabel(
        QStringLiteral("\xe4\xbb\x8e\xe2\x80\x9c\xe7\x9b\xae\xe6\xa0\x87\xe7\xb1\xbb\xe5\x88\xab\xe2\x80\x9d\xe9\xa1\xb5"
                       "\xe5\x8b\xbe\xe9\x80\x89\xe2\x80\x9c\xe7\x9e\x84\xe5\x87\x86\xe2\x80\x9d\xe7\x9a\x84\xe7\xb1\xbb\xe5\x88\xab"
                       "\xe4\xbc\x9a\xe5\x87\xba\xe7\x8e\xb0\xe5\x9c\xa8\xe4\xb8\x8b\xe6\x96\xb9\xe3\x80\x82"
                       "\xe6\x8e\x92\xe5\x9c\xa8\xe5\x89\x8d\xe9\x9d\xa2\xe7\x9a\x84\xe4\xbc\x98\xe5\x85\x88\xe7\xba\xa7\xe6\x9b\xb4\xe9\xab\x98\xe3\x80\x82"));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#A1A1AA; font-size:11px;");
    cl->addWidget(hint);

    m_aimClassListLayout = new QVBoxLayout;
    m_aimClassListLayout->setSpacing(4);
    cl->addLayout(m_aimClassListLayout);

    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(6);
    m_addClassCombo = new QComboBox;
    m_addClassCombo->setMinimumWidth(120);
    addRow->addWidget(m_addClassCombo, 1);
    m_addClassBtn = new QPushButton(QStringLiteral("+ \xe6\xb7\xbb\xe5\x8a\xa0"));
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
                if (a.class_id == classId) return;
            HotkeyAimClass entry;
            entry.class_id = classId;
            entry.y_offset = 0.35f;
            ac.push_back(entry);
        }

        ConfigBridge::instance().markDirty();
        rebuildAimClassList();
        rebuildAddClassCombo();
    });

    m_rightLayout->addWidget(m_aimClassCard);
}

std::vector<int> HotkeyPage::collectAimBucketClassIds()
{
    std::vector<int> ids;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    for (const auto& cf : config.class_filters)
        if (cf.bucket == ClassBucket::Aim)
            ids.push_back(cf.class_id);
    return ids;
}

void HotkeyPage::rebuildAddClassCombo()
{
    m_addClassCombo->clear();
    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    auto aimIds = collectAimBucketClassIds();

    std::set<int> alreadyAdded;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri < static_cast<int>(config.hotkeys.size()))
            for (const auto& a : config.hotkeys[ri].aim_classes)
                alreadyAdded.insert(a.class_id);
    }

    std::lock_guard<std::recursive_mutex> lk(configMutex);
    for (int cid : aimIds) {
        if (alreadyAdded.count(cid)) continue;
        QString name;
        for (const auto& cf : config.class_filters)
            if (cf.class_id == cid) {
                name = cf.class_name.empty()
                    ? QStringLiteral("class_%1").arg(cid)
                    : QString::fromUtf8(cf.class_name.c_str());
                break;
            }
        m_addClassCombo->addItem(QStringLiteral("[%1] %2").arg(cid).arg(name), cid);
    }

    m_addClassBtn->setEnabled(m_addClassCombo->count() > 0);
}

void HotkeyPage::rebuildAimClassList()
{
    while (m_aimClassListLayout->count() > 0) {
        auto* item = m_aimClassListLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    int ri = currentRuntimeIndex();
    if (ri < 0) return;

    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (ri >= static_cast<int>(config.hotkeys.size())) return;
    auto& aimClasses = config.hotkeys[ri].aim_classes;

    auto aimIds = std::set<int>();
    for (const auto& cf : config.class_filters)
        if (cf.bucket == ClassBucket::Aim)
            aimIds.insert(cf.class_id);

    // purge stale entries
    aimClasses.erase(
        std::remove_if(aimClasses.begin(), aimClasses.end(),
            [&](const HotkeyAimClass& a) { return aimIds.find(a.class_id) == aimIds.end(); }),
        aimClasses.end());

    for (int idx = 0; idx < static_cast<int>(aimClasses.size()); ++idx) {
        auto& ac = aimClasses[idx];

        QString className;
        for (const auto& cf : config.class_filters)
            if (cf.class_id == ac.class_id) {
                className = cf.class_name.empty()
                    ? QStringLiteral("class_%1").arg(ac.class_id)
                    : QString::fromUtf8(cf.class_name.c_str());
                break;
            }

        auto* row = new QWidget;
        row->setStyleSheet("background:#FAFAFA; border:1px solid #E8E8EC; border-radius:6px;");
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(10, 6, 6, 6);
        rowLay->setSpacing(8);

        auto* priLabel = new QLabel(QStringLiteral("#%1").arg(idx + 1));
        priLabel->setFixedWidth(28);
        priLabel->setStyleSheet("color:#5E6AD2; font-size:12px; font-weight:600; border:none;");
        rowLay->addWidget(priLabel);

        auto* nameLabel = new QLabel(QStringLiteral("[%1] %2").arg(ac.class_id).arg(className));
        nameLabel->setStyleSheet("color:#3C3C44; font-size:12px; border:none;");
        rowLay->addWidget(nameLabel, 1);

        auto* yLabel = new QLabel(QStringLiteral("Y:"));
        yLabel->setFixedWidth(16);
        yLabel->setStyleSheet("color:#8E8E96; font-size:11px; border:none;");
        rowLay->addWidget(yLabel);

        auto* ySpin = new QDoubleSpinBox;
        ySpin->setRange(0.0, 1.0);
        ySpin->setSingleStep(0.05);
        ySpin->setDecimals(2);
        ySpin->setValue(static_cast<double>(ac.y_offset));
        ySpin->setFixedWidth(68);
        ySpin->setToolTip(QStringLiteral("Y \xe5\x81\x8f\xe7\xa7\xbb: 0=\xe6\xa1\x86\xe9\xa1\xb6, 0.5=\xe4\xb8\xad\xe5\xbf\x83, 1=\xe6\xa1\x86\xe5\xba\x95"));
        rowLay->addWidget(ySpin);

        const int classId = ac.class_id;
        connect(ySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, classId](double val) {
            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            std::lock_guard<std::recursive_mutex> lk2(configMutex);
            if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
            for (auto& a : config.hotkeys[ri2].aim_classes)
                if (a.class_id == classId) { a.y_offset = static_cast<float>(val); break; }
            ConfigBridge::instance().markDirty();
        });

        auto makeArrowBtn = [&](const QString& icon) {
            auto* btn = new QPushButton(icon);
            btn->setFixedSize(24, 24);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(
                "QPushButton{font-family:\"tabler-icons\"; font-size:14px; color:#8E8E96;"
                " background:transparent; border:none; padding:0;}"
                "QPushButton:hover{color:#5E6AD2;}");
            return btn;
        };

        auto* upBtn = makeArrowBtn(QString(IconFont::glyph("chevron-up")));
        auto* downBtn = makeArrowBtn(QString(IconFont::glyph("chevron-down")));
        auto* delBtn = makeArrowBtn(QString(IconFont::glyph("x")));
        delBtn->setStyleSheet(
            "QPushButton{font-family:\"tabler-icons\"; font-size:14px; color:#D25A5A;"
            " background:transparent; border:none; padding:0;}"
            "QPushButton:hover{color:#B83232;}");

        rowLay->addWidget(upBtn);
        rowLay->addWidget(downBtn);
        rowLay->addWidget(delBtn);

        connect(upBtn, &QPushButton::clicked, this, [this, classId] {
            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk2(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                auto& ac2 = config.hotkeys[ri2].aim_classes;
                for (int j = 1; j < static_cast<int>(ac2.size()); ++j)
                    if (ac2[j].class_id == classId) { std::swap(ac2[j], ac2[j-1]); break; }
            }
            ConfigBridge::instance().markDirty();
            rebuildAimClassList();
        });

        connect(downBtn, &QPushButton::clicked, this, [this, classId] {
            int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk2(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                auto& ac2 = config.hotkeys[ri2].aim_classes;
                for (int j = 0; j + 1 < static_cast<int>(ac2.size()); ++j)
                    if (ac2[j].class_id == classId) { std::swap(ac2[j], ac2[j+1]); break; }
            }
            ConfigBridge::instance().markDirty();
            rebuildAimClassList();
        });

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

        m_aimClassListLayout->addWidget(row);
    }
}

void HotkeyPage::onAimClassFiltersChanged()
{
    rebuildAimClassList();
    rebuildAddClassCombo();
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
    m_moverKindCombo->setToolTip(QStringLiteral(
        "\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x8e\xa7\xe5\x88\xb6\xe5\x99\xa8\xef\xbc\x9a\n"
        "\xe5\xbe\xae\xe6\xbe\x9c\xef\xbc\x9a" "ART \xe9\xbb\x98\xe8\xae\xa4\xe8\xb7\xaf\xe5\xbe\x84(EMA \xe5\xb9\xb3\xe6\xbb\x91)\n"
        "\xe7\x96\xbe\xe9\xa3\x8e\xef\xbc\x9a\xe4\xbd\x8d\xe7\xbd\xae\xe5\xbc\x8f PID + \xe5\xaf\xbc\xe6\x95\xb0\xe9\xa2\x84\xe6\xb5\x8b"));
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

    cl->addWidget(m_moverParamStack);

    // ── Wiring: combo 联动 stack + 写回 profile ──
    connect(m_moverKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (idx < 0) idx = 0;
                if (idx > 1) idx = 1;
                m_moverParamStack->setCurrentIndex(idx);
                saveUiToCurrentProfile();
            });

    auto wireD = [this](QDoubleSpinBox* sp) {
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
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

    m_rightLayout->addWidget(card);
}

// ═══════════════════════════════════════════════════════════════════════════
// Card: Smart Trigger
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildSmartTriggerCard()
{
    auto* card = new CardWidget(
        QStringLiteral("\xe6\x99\xba\xe8\x83\xbd\xe6\x89\xb3\xe6\x9c\xba"),
        QStringLiteral("crosshair"));
    card->setCollapsible(true);
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(
        QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8\xe6\x99\xba\xe8\x83\xbd\xe6\x89\xb3\xe6\x9c\xba"),
        false, m_smartTrigger));

    QSlider* hsSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(
        QStringLiteral("\xe5\x91\xbd\xe4\xb8\xad\xe5\xae\xb9\xe5\xb7\xae"),   // 命中容差
        0.1, 1.5, 0.60, 0.01, 2, hsSl, m_smartTriggerHitScale));
    m_smartTriggerHitScale->setToolTip(
        QStringLiteral("\xe5\x8d\xa0 bbox \xe5\x8d\x8a\xe8\xbd\xb4\xe6\xaf\x94\xe4\xbe\x8b\xef\xbc\x8c\xe8\xb6\x8a\xe5\xa4\xa7\xe8\xb6\x8a\xe5\xae\xb9\xe6\x98\x93\xe5\xbc\x80\xe7\x81\xab"));

    QSlider* agSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(
        QStringLiteral("\xe6\xbf\x80\xe8\xbf\x9b\xe5\xba\xa6"),   // 激进度
        0.0, 1.0, 0.50, 0.01, 2, agSl, m_smartTriggerAggression));
    m_smartTriggerAggression->setToolTip(
        QStringLiteral("0=\xe4\xbf\x9d\xe5\xae\x88(80ms\xe5\x8f\x8d\xe5\xba\x94)  1=\xe6\xbf\x80\xe8\xbf\x9b(0ms\xe5\x8f\x8d\xe5\xba\x94)"));

    // 按住时长 (hold_ms): 每次扣下扳机持续按住的毫秒数
    QSlider* holdSl = nullptr;
    cl->addWidget(FormKit::sliderRow(
        QStringLiteral("\xe6\x8c\x89\xe4\xbd\x8f\xe6\x97\xb6\xe9\x95\xbf"),   // 按住时长
        5, 5000, 45, holdSl, m_smartTriggerHoldMs,
        QStringLiteral(" ms")));
    m_smartTriggerHoldMs->setToolTip(
        QStringLiteral("\xe6\xaf\x8f\xe6\xac\xa1\xe6\x8c\x89\xe4\xb8\x8b\xe9\xbc\xa0\xe6\xa0\x87\xe5\xb7\xa6\xe9\x94\xae\xe7\x9a\x84\xe6\x8c\x81\xe7\xbb\xad\xe6\x97\xb6\xe9\x97\xb4 (ms)"));

    // 两次按住之间的延迟 (cooldown_ms): 一次击发后的冷却,再次扣下扳机前必须等待
    QSlider* cdSl = nullptr;
    cl->addWidget(FormKit::sliderRow(
        QStringLiteral("\xe5\x86\xb7\xe5\x8d\xb4\xe5\xbb\xb6\xe8\xbf\x9f"),   // 冷却延迟
        0, 2000, 55, cdSl, m_smartTriggerCooldownMs,
        QStringLiteral(" ms")));
    m_smartTriggerCooldownMs->setToolTip(
        QStringLiteral("\xe4\xb8\xa4\xe6\xac\xa1\xe6\x8c\x89\xe4\xbd\x8f\xe4\xb9\x8b\xe9\x97\xb4\xe7\x9a\x84\xe9\x97\xb4\xe9\x9a\x94 (ms)\xef\xbc\x8c\xe6\x8a\x97\xe9\x80\x9f\xe7\x82\xb9"));

    connect(m_smartTrigger,              &ToggleSwitch::toggled,                                     this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_smartTriggerHitScale,      QOverload<double>::of(&QDoubleSpinBox::valueChanged),       this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_smartTriggerAggression,    QOverload<double>::of(&QDoubleSpinBox::valueChanged),       this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_smartTriggerHoldMs,        QOverload<int>::of(&QSpinBox::valueChanged),                this, &HotkeyPage::saveUiToCurrentProfile);
    connect(m_smartTriggerCooldownMs,    QOverload<int>::of(&QSpinBox::valueChanged),                this, &HotkeyPage::saveUiToCurrentProfile);

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
// Card 6: Advanced
// ═══════════════════════════════════════════════════════════════════════════

void HotkeyPage::buildAdvancedCard()
{
    auto* card = new CardWidget(QStringLiteral("\xe9\xab\x98\xe7\xba\xa7"),
                                QStringLiteral("settings"));
    card->setCollapsible(true);
    auto* cl = card->contentLayout();

    QSlider* laSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(
        QStringLiteral("\xe9\x94\x81\xe5\xae\x9a\xe7\x81\xb5\xe6\xb4\xbb\xe5\xba\xa6"),
        0.0, 1.0, 0.30, 0.01, 2, laSl, m_lockAggression));
    m_lockAggression->setToolTip(
        QStringLiteral("0=\xe6\xad\xbb\xe9\x94\x81(\xe4\xb8\x8d\xe5\x88\x87\xe6\x8d\xa2)  "
                       "1=\xe7\x81\xb5\xe6\xb4\xbb(\xe5\x9b\xa2\xe6\x88\x98\xe6\x8d\xa2\xe4\xba\xba\xe5\xa4\xb4)"));

    connect(m_lockAggression, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &HotkeyPage::saveUiToCurrentProfile);

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
        const int mk = std::clamp(hp.mover_kind, 0, 1);
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

    m_lockAggression->setValue(static_cast<double>(hp.lock_aggression));

    m_smartTrigger->setChecked(hp.smart_trigger_enabled);
    m_smartTriggerHitScale->setValue(static_cast<double>(hp.smart_trigger_hit_scale));
    m_smartTriggerAggression->setValue(static_cast<double>(hp.smart_trigger_aggression));
    m_smartTriggerHoldMs->setValue(hp.smart_trigger_hold_ms);
    m_smartTriggerCooldownMs->setValue(hp.smart_trigger_cooldown_ms);

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

    rebuildAimClassList();
    rebuildAddClassCombo();
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
    hp.mover_kind      = std::clamp(m_moverKindCombo->currentIndex(), 0, 1);
    hp.speed_x         = static_cast<float>(m_speedXSpin->value());
    hp.speed_y         = static_cast<float>(m_speedYSpin->value());
    hp.dead_zone_px    = static_cast<float>(m_deadZoneSpin->value());
    hp.predictive_kp_x        = static_cast<float>(m_predKpXSpin->value());
    hp.predictive_kp_y        = static_cast<float>(m_predKpYSpin->value());
    hp.predictive_kd          = static_cast<float>(m_predKdSpin->value());
    hp.predictive_pred_weight = static_cast<float>(m_predPwSpin->value());
    hp.lock_aggression = static_cast<float>(m_lockAggression->value());
    hp.smart_trigger_enabled     = m_smartTrigger->isChecked();
    hp.smart_trigger_hit_scale   = static_cast<float>(m_smartTriggerHitScale->value());
    hp.smart_trigger_aggression  = static_cast<float>(m_smartTriggerAggression->value());
    hp.smart_trigger_hold_ms     = m_smartTriggerHoldMs->value();
    hp.smart_trigger_cooldown_ms = m_smartTriggerCooldownMs->value();

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
