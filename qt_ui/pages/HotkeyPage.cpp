#include "pages/HotkeyPage.h"
#include "widgets/BezierEditor.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <iterator>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

HotkeyPage::HotkeyPage(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* leftWidget = new QWidget;
    leftWidget->setFixedWidth(180);
    buildLeftPanel(leftWidget);

    auto* rightWidget = new QWidget;
    buildRightPanel(rightWidget);

    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setSizes({180, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root->addWidget(splitter);

    addProfileItem(QStringLiteral("主瞄"), QStringLiteral("右键"));
    addProfileItem(QStringLiteral("压枪"), QStringLiteral("侧键 1"));
    addProfileItem(QStringLiteral("狙击"), QStringLiteral("右键 + Shift"));
    m_profileList->setCurrentRow(0);
}

// ---------------------------------------------------------------------------
// Left Panel
// ---------------------------------------------------------------------------

void HotkeyPage::buildLeftPanel(QWidget* parent) {
    auto* lay = new QVBoxLayout(parent);
    lay->setContentsMargins(12, 14, 6, 12);
    lay->setSpacing(8);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(4, 0, 4, 0);
    auto* title = new QLabel(QStringLiteral("方案"));
    title->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    auto* addBtn = new QPushButton(QString(IconFont::glyph("plus")));
    addBtn->setFixedSize(24, 24);
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setStyleSheet(
        "QPushButton{font-family:\"tabler-icons\"; font-size:16px; color:#8E8E96;"
        " background:transparent; border:none; padding:0;}"
        "QPushButton:hover{color:#5E6AD2;}");
    header->addWidget(title);
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

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &HotkeyPage::onProfileSelected);
    connect(m_profileList, &QListWidget::customContextMenuRequested,
            this, &HotkeyPage::onContextMenu);
    connect(addBtn, &QPushButton::clicked, this, &HotkeyPage::onAddProfile);
}

void HotkeyPage::addProfileItem(const QString& name, const QString& keyPreview) {
    auto* item = new QListWidgetItem(m_profileList);
    item->setData(Qt::UserRole, name);
    item->setData(Qt::UserRole + 1, keyPreview);

    auto* w = new QWidget;
    w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(11, 8, 11, 8);
    v->setSpacing(3);

    auto* nameLbl = new QLabel(name);
    nameLbl->setObjectName("pname");
    auto* keyLbl = new QLabel(keyPreview);
    keyLbl->setObjectName("pkey");

    v->addWidget(nameLbl);
    v->addWidget(keyLbl);

    item->setSizeHint(w->sizeHint());
    m_profileList->setItemWidget(item, w);
    restyleProfileItems();
}

void HotkeyPage::restyleProfileItems() {
    for (int i = 0; i < m_profileList->count(); ++i) {
        auto* w = m_profileList->itemWidget(m_profileList->item(i));
        if (!w)
            continue;
        const bool sel = (i == m_profileList->currentRow());
        if (auto* n = w->findChild<QLabel*>("pname"))
            n->setStyleSheet(sel ? "color:#4A55C8; font-size:13px; font-weight:500;"
                                 : "color:#3C3C44; font-size:13px;");
        if (auto* k = w->findChild<QLabel*>("pkey"))
            k->setStyleSheet(sel ? "color:#7E88D8; font-size:11px;"
                                 : "color:#A1A1AA; font-size:11px;");
    }
}

// ---------------------------------------------------------------------------
// Right Panel
// ---------------------------------------------------------------------------

void HotkeyPage::buildRightPanel(QWidget* parent) {
    auto* outerLay = new QVBoxLayout(parent);
    outerLay->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    m_rightLayout = new QVBoxLayout(content);
    m_rightLayout->setContentsMargins(12, 8, 12, 8);
    m_rightLayout->setSpacing(12);

    buildKeyBindingCard();
    buildFovCard();
    buildAimCard();
    buildTrajectoryCard();
    buildKalmanCard();
    buildAdvancedCard();

    m_rightLayout->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);
}

// ---------------------------------------------------------------------------
// Card 1: Key Bindings
// ---------------------------------------------------------------------------

void HotkeyPage::buildKeyBindingCard() {
    auto* card = new CardWidget(QStringLiteral("按键绑定"), QStringLiteral("keyboard"));
    auto* cl = card->contentLayout();

    m_profileName = new QLineEdit;
    cl->addWidget(FormKit::fieldRow(QStringLiteral("配置名称"), m_profileName));

    m_keyBindingsLayout = new QVBoxLayout;
    m_keyBindingsLayout->setSpacing(6);
    cl->addLayout(m_keyBindingsLayout);

    auto* addKeyBtn = new QPushButton(QStringLiteral("+ 添加按键"));
    cl->addWidget(addKeyBtn);
    connect(addKeyBtn, &QPushButton::clicked, this, [this] { addKeyBindingRow(); });

    addKeyBindingRow("None");

    m_rightLayout->addWidget(card);
}

void HotkeyPage::addKeyBindingRow(const QString& key) {
    auto* row = new QWidget;
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* combo = new QComboBox;
    combo->addItems({"None", "LeftMouseButton", "RightMouseButton",
                     "X1MouseButton", "X2MouseButton"});
    combo->setCurrentText(key);
    lay->addWidget(combo, 1);

    auto* removeBtn = new QPushButton(QStringLiteral("−"));
    removeBtn->setFixedWidth(32);
    lay->addWidget(removeBtn);

    m_keyBindingsLayout->addWidget(row);

    connect(removeBtn, &QPushButton::clicked, this, [this, row] {
        if (m_keyBindingsLayout->count() <= 1) {
            return;
        }
        m_keyBindingsLayout->removeWidget(row);
        row->deleteLater();
    });
}

void HotkeyPage::removeKeyBindingRow() {
    if (m_keyBindingsLayout->count() <= 1) {
        return;
    }
    auto* item = m_keyBindingsLayout->takeAt(m_keyBindingsLayout->count() - 1);
    if (item && item->widget()) {
        item->widget()->deleteLater();
    }
    delete item;
}

void HotkeyPage::clearKeyBindings() {
    while (m_keyBindingsLayout->count() > 0) {
        auto* item = m_keyBindingsLayout->takeAt(0);
        if (item && item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

// ---------------------------------------------------------------------------
// Card 2: FOV
// ---------------------------------------------------------------------------

void HotkeyPage::buildFovCard() {
    auto* card = new CardWidget(QStringLiteral("视野 FOV"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::sliderRow(QStringLiteral("FOV X"), 10, 640, 106,
                                     m_fovXSlider, m_fovXSpin));
    cl->addWidget(FormKit::sliderRow(QStringLiteral("FOV Y"), 10, 640, 74,
                                     m_fovYSlider, m_fovYSpin));

    cl->addWidget(FormKit::toggleRow(QStringLiteral("启用动态 FOV"), false, m_dynamicFov));

    m_dynamicFovContainer = new QWidget;
    auto* dynLay = new QVBoxLayout(m_dynamicFovContainer);
    dynLay->setContentsMargins(0, 0, 0, 0);
    dynLay->setSpacing(8);

    QSlider* marginSl = nullptr;
    dynLay->addWidget(FormKit::sliderRowD(QStringLiteral("内边距系数"), 1.0, 2.0, 1.10,
                                          0.01, 2, marginSl, m_dynamicFovMargin));

    QSlider* minRadSl = nullptr;
    dynLay->addWidget(FormKit::sliderRowD(QStringLiteral("最小半径系数"), 0.05, 1.0, 0.20,
                                          0.01, 2, minRadSl, m_dynamicFovMinRadius));

    m_dynamicFovContainer->setVisible(false);
    cl->addWidget(m_dynamicFovContainer);

    connect(m_dynamicFov, &ToggleSwitch::toggled,
            m_dynamicFovContainer, &QWidget::setVisible);

    m_rightLayout->addWidget(card);
}

// ---------------------------------------------------------------------------
// Card 3: Aim Parameters
// ---------------------------------------------------------------------------

void HotkeyPage::buildAimCard() {
    auto* card = new CardWidget(QStringLiteral("瞄准参数"), QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    QSlider* sxSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("X 速度"), 0.0, 1.0, 0.6,
                                      0.001, 3, sxSl, m_speedX));

    QSlider* sySl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("Y 速度"), 0.0, 1.0, 0.6,
                                      0.001, 3, sySl, m_speedY));

    QSlider* lsSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("锁死力度"), 0.0, 1.0, 0.0,
                                      0.01, 2, lsSl, m_lockStrength));

    QSlider* lrSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("锁死范围"), 4.0, 80.0, 25.0,
                                      0.5, 1, lrSl, m_lockRadius, QStringLiteral(" px")));

    // Smart trigger subsection
    auto* triggerCard = new CardWidget(QStringLiteral("智能扳机"));
    triggerCard->setCollapsible(true);
    auto* tcl = triggerCard->contentLayout();

    tcl->addWidget(FormKit::toggleRow(QStringLiteral("启用智能扳机"), false,
                                      m_smartTriggerEnable));

    m_smartTriggerContainer = new QWidget;
    auto* stLay = new QVBoxLayout(m_smartTriggerContainer);
    stLay->setContentsMargins(0, 0, 0, 0);
    stLay->setSpacing(8);

    QSlider* hrSl = nullptr;
    stLay->addWidget(FormKit::sliderRowD(QStringLiteral("命中半径系数"), 0.1, 1.0, 0.5,
                                         0.01, 2, hrSl, m_smartTriggerHitRadius));

    QSlider* varSl = nullptr;
    stLay->addWidget(FormKit::sliderRowD(QStringLiteral("方差阈值"), 1.0, 20.0, 5.0,
                                         0.5, 1, varSl, m_smartTriggerVariance,
                                         QStringLiteral(" px")));

    QSlider* winSl = nullptr;
    stLay->addWidget(FormKit::sliderRow(QStringLiteral("窗口帧数"), 1, 30, 5,
                                        winSl, m_smartTriggerWindow));

    QSlider* durSl = nullptr;
    stLay->addWidget(FormKit::sliderRow(QStringLiteral("开火时长"), 10, 200, 50,
                                        durSl, m_smartTriggerDuration,
                                        QStringLiteral(" ms")));

    m_smartTriggerContainer->setVisible(false);
    tcl->addWidget(m_smartTriggerContainer);

    connect(m_smartTriggerEnable, &ToggleSwitch::toggled,
            m_smartTriggerContainer, &QWidget::setVisible);

    cl->addWidget(triggerCard);
    m_rightLayout->addWidget(card);
}

// ---------------------------------------------------------------------------
// Card 4: Trajectory Curve
// ---------------------------------------------------------------------------

static const struct {
    const char* name;
    float cx1, cy1, cx2, cy2;
} kBezierPresets[] = {
    {"直线",          0.00f,  0.00f, 1.00f,  0.00f},
    {"右弧",          0.30f,  0.50f, 0.70f, -0.30f},
    {"左弧",          0.30f, -0.50f, 0.70f,  0.30f},
    {"S形",               0.25f,  0.60f, 0.75f, -0.60f},
    {"Z形",               0.25f, -0.60f, 0.75f,  0.60f},
    {"先慢后快", 0.80f, 0.00f, 1.00f,  0.00f},
    {"先快后慢", 0.00f, 0.00f, 0.20f,  0.00f},
    {"高弧右抛", 0.15f, 0.80f, 0.85f, -0.40f},
    {"低弧左拐", 0.15f,-0.40f, 0.85f,  0.80f},
    {"轻微右偏", 0.30f, 0.15f, 0.70f, -0.05f},
    {"轻微左偏", 0.30f,-0.15f, 0.70f,  0.05f},
    {"过冲",          0.20f,  0.00f, 0.60f,  0.00f},
};

void HotkeyPage::buildTrajectoryCard() {
    auto* card = new CardWidget(QStringLiteral("轨迹曲线"), QStringLiteral("vector-spline"));
    auto* cl = card->contentLayout();

    m_trajectoryMode = new QComboBox;
    m_trajectoryMode->addItems({
        QStringLiteral("直线 (Direct)"),
        QStringLiteral("贝塞尔 (Bezier)")
    });
    cl->addWidget(FormKit::fieldRow(QStringLiteral("轨迹模式"), m_trajectoryMode));

    // Bezier container
    m_bezierContainer = new QWidget;
    auto* bzLay = new QVBoxLayout(m_bezierContainer);
    bzLay->setContentsMargins(0, 4, 0, 0);
    bzLay->setSpacing(8);

    // Preset buttons in a grid
    auto* presetGrid = new QWidget;
    auto* gridLay = new QGridLayout(presetGrid);
    gridLay->setContentsMargins(0, 0, 0, 0);
    gridLay->setHorizontalSpacing(4);
    gridLay->setVerticalSpacing(4);

    constexpr int kPresetsPerRow = 4;
    const int presetCount = static_cast<int>(std::size(kBezierPresets));

    for (int i = 0; i < presetCount; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(kBezierPresets[i].name));
        btn->setFixedHeight(28);
        gridLay->addWidget(btn, i / kPresetsPerRow, i % kPresetsPerRow);
        connect(btn, &QPushButton::clicked, this, [this, i] { onBezierPreset(i); });
    }

    bzLay->addWidget(presetGrid);

    m_bezierEditor = new BezierEditor;
    bzLay->addWidget(m_bezierEditor, 0, Qt::AlignHCenter);

    QSlider* ffSl = nullptr;
    bzLay->addWidget(FormKit::sliderRowD(QStringLiteral("跟随系数"), 0.0, 1.0, 0.5,
                                         0.01, 2, ffSl, m_followFactor));

    QSlider* raSl = nullptr;
    bzLay->addWidget(FormKit::sliderRowD(QStringLiteral("重锚阈值"), 10.0, 200.0, 50.0,
                                         1.0, 0, raSl, m_reanchorThreshold,
                                         QStringLiteral(" px")));

    m_bezierContainer->setVisible(false);
    cl->addWidget(m_bezierContainer);

    connect(m_trajectoryMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HotkeyPage::onTrajectoryModeChanged);

    m_rightLayout->addWidget(card);
}

void HotkeyPage::onTrajectoryModeChanged(int index) {
    m_bezierContainer->setVisible(index == 1);
}

void HotkeyPage::onBezierPreset(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= static_cast<int>(std::size(kBezierPresets))) {
        return;
    }
    const auto& p = kBezierPresets[presetIndex];
    m_bezierEditor->setCurve(p.cx1, p.cy1, p.cx2, p.cy2);
}

void HotkeyPage::setBezierWidgetsVisible(bool visible) {
    m_bezierContainer->setVisible(visible);
}

// ---------------------------------------------------------------------------
// Card 5: Kalman Filter
// ---------------------------------------------------------------------------

void HotkeyPage::buildKalmanCard() {
    auto* card = new CardWidget(QStringLiteral("Kalman 滤波"), QStringLiteral("adjustments"));
    card->setCollapsible(true);
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(QStringLiteral("启用 Kalman"), false, m_kalmanEnable));

    QSlider* pnSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("位置过程噪声"), 1.0, 200.0, 40.0,
                                      1.0, 1, pnSl, m_kalmanPosNoise));

    QSlider* vnSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("速度过程噪声"), 100.0, 10000.0, 1800.0,
                                      50.0, 0, vnSl, m_kalmanVelNoise));

    QSlider* mnSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("测量噪声"), 1.0, 200.0, 35.0,
                                      1.0, 1, mnSl, m_kalmanMeasNoise));

    QSlider* vdSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("速度衰减"), 0.0, 1.0, 0.08,
                                      0.01, 2, vdSl, m_kalmanVelDecay));

    QSlider* mvSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("最大速度"), 1000.0, 50000.0, 20000.0,
                                      500.0, 0, mvSl, m_kalmanMaxVel));

    QSlider* wuSl = nullptr;
    cl->addWidget(FormKit::sliderRow(QStringLiteral("预热帧数"), 0, 10, 2,
                                     wuSl, m_kalmanWarmup));

    cl->addWidget(FormKit::toggleRow(QStringLiteral("补偿检测延迟"), false,
                                     m_kalmanCompensateDelay));

    m_rightLayout->addWidget(card);
}

// ---------------------------------------------------------------------------
// Card 6: Advanced
// ---------------------------------------------------------------------------

void HotkeyPage::buildAdvancedCard() {
    auto* card = new CardWidget(QStringLiteral("高级"), QStringLiteral("settings"));
    card->setCollapsible(true);
    auto* cl = card->contentLayout();

    cl->addWidget(FormKit::toggleRow(QStringLiteral("准星找色"), false, m_crosshairDetect));

    // Lock switch
    auto* lockSwitchLabel = new QLabel(QStringLiteral("锁定切换"));
    lockSwitchLabel->setProperty("class", "heading");
    cl->addWidget(lockSwitchLabel);

    QSlider* lsmSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("切换边距"), 0.0, 200.0, 30.0,
                                      1.0, 1, lsmSl, m_lockSwitchMargin));

    QSlider* lsfSl = nullptr;
    cl->addWidget(FormKit::sliderRow(QStringLiteral("最小帧数"), 1, 30, 3,
                                     lsfSl, m_lockSwitchMinFrames));

    QSlider* lhSl = nullptr;
    cl->addWidget(FormKit::sliderRow(QStringLiteral("保持最小帧"), 1, 60, 5,
                                     lhSl, m_lockHoldMinFrames));

    // Y offset decay
    auto* yOffLabel = new QLabel(QStringLiteral("Y 偏移衰减"));
    yOffLabel->setProperty("class", "heading");
    cl->addWidget(yOffLabel);

    QSlider* ydrSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("衰减速率"), 0.0, 1.0, 0.1,
                                      0.01, 2, ydrSl, m_yOffsetDecayRate));

    QSlider* yddSl = nullptr;
    cl->addWidget(FormKit::sliderRow(QStringLiteral("延迟帧数"), 0, 60, 10,
                                     yddSl, m_yOffsetDecayDelay));

    // Threat priority
    auto* threatLabel = new QLabel(QStringLiteral("威胁优先级"));
    threatLabel->setProperty("class", "heading");
    cl->addWidget(threatLabel);

    QSlider* tpSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("优先级权重"), 0.0, 1.0, 0.5,
                                      0.01, 2, tpSl, m_threatPriorityWeight));

    QSlider* tdSl = nullptr;
    cl->addWidget(FormKit::sliderRowD(QStringLiteral("距离权重"), 0.0, 1.0, 0.5,
                                      0.01, 2, tdSl, m_threatDistanceWeight));

    m_rightLayout->addWidget(card);
}

// ---------------------------------------------------------------------------
// Profile Management
// ---------------------------------------------------------------------------

void HotkeyPage::onProfileSelected(int row) {
    restyleProfileItems();
    if (row < 0) {
        return;
    }
    loadProfileToUi(row);
}

void HotkeyPage::onAddProfile() {
    QStringList templates = {
        QStringLiteral("近距离"),
        QStringLiteral("中距离"),
        QStringLiteral("远距离"),
        QStringLiteral("空白"),
    };

    bool ok = false;
    auto choice = QInputDialog::getItem(
        this,
        QStringLiteral("新建配置"),
        QStringLiteral("选择模板:"),
        templates, 0, false, &ok);

    if (!ok) {
        return;
    }

    QString name;
    float speedX = 0.6f, speedY = 0.6f, lockStr = 0.0f;
    int fovX = 106, fovY = 74;

    if (choice == templates[0]) {
        name = QStringLiteral("近距离");
        speedX = 0.8f; speedY = 0.8f; lockStr = 0.6f;
        fovX = 160; fovY = 120;
    } else if (choice == templates[1]) {
        name = QStringLiteral("中距离");
        speedX = 0.5f; speedY = 0.5f; lockStr = 0.3f;
        fovX = 106; fovY = 74;
    } else if (choice == templates[2]) {
        name = QStringLiteral("远距离");
        speedX = 0.3f; speedY = 0.3f; lockStr = 0.1f;
        fovX = 80; fovY = 60;
    } else {
        name = QStringLiteral("新配置");
    }

    addProfileItem(name, QStringLiteral("None"));
    m_profileList->setCurrentRow(m_profileList->count() - 1);

    // Apply template values to UI
    m_profileName->setText(name);
    m_speedX->setValue(speedX);
    m_speedY->setValue(speedY);
    m_lockStrength->setValue(lockStr);
    m_fovXSpin->setValue(fovX);
    m_fovYSpin->setValue(fovY);
}

void HotkeyPage::onDeleteProfile() {
    int row = m_profileList->currentRow();
    if (row < 0) {
        return;
    }

    auto* item = m_profileList->item(row);
    auto answer = QMessageBox::question(
        this,
        QStringLiteral("删除配置"),
        QStringLiteral("确定删除“%1”？").arg(item->text()));

    if (answer != QMessageBox::Yes) {
        return;
    }

    delete m_profileList->takeItem(row);
}

void HotkeyPage::onCopyProfile() {
    int row = m_profileList->currentRow();
    if (row < 0) {
        return;
    }

    auto* src = m_profileList->item(row);
    const QString newName = src->data(Qt::UserRole).toString() + QStringLiteral(" (副本)");
    addProfileItem(newName, src->data(Qt::UserRole + 1).toString());
    m_profileList->setCurrentRow(m_profileList->count() - 1);
}

void HotkeyPage::onContextMenu(const QPoint& pos) {
    auto* item = m_profileList->itemAt(pos);
    if (!item) {
        return;
    }

    QMenu menu;
    auto* renameAct = menu.addAction(QStringLiteral("重命名"));
    auto* copyAct   = menu.addAction(QStringLiteral("复制"));
    auto* deleteAct = menu.addAction(QStringLiteral("删除"));

    auto* chosen = menu.exec(m_profileList->viewport()->mapToGlobal(pos));
    if (chosen == renameAct) {
        onRenameProfile();
    } else if (chosen == copyAct) {
        onCopyProfile();
    } else if (chosen == deleteAct) {
        onDeleteProfile();
    }
}

void HotkeyPage::onRenameProfile() {
    int row = m_profileList->currentRow();
    if (row < 0) {
        return;
    }

    auto* item = m_profileList->item(row);
    bool ok = false;
    auto name = QInputDialog::getText(
        this,
        QStringLiteral("重命名"),
        QStringLiteral("新名称:"),
        QLineEdit::Normal,
        item->text(),
        &ok);

    if (ok && !name.isEmpty()) {
        item->setData(Qt::UserRole, name);
        if (auto* w = m_profileList->itemWidget(item))
            if (auto* n = w->findChild<QLabel*>("pname"))
                n->setText(name);
        m_profileName->setText(name);
    }
}

void HotkeyPage::updateProfileListItem(int row) {
    if (row < 0 || row >= m_profileList->count()) {
        return;
    }
    auto* item = m_profileList->item(row);
    item->setData(Qt::UserRole, m_profileName->text());
    if (auto* w = m_profileList->itemWidget(item))
        if (auto* n = w->findChild<QLabel*>("pname"))
            n->setText(m_profileName->text());
}

void HotkeyPage::loadProfileToUi(int index) {
    if (!m_profileName || index < 0 || index >= m_profileList->count()) {
        return;
    }
    auto* item = m_profileList->item(index);
    m_profileName->setText(item->data(Qt::UserRole).toString());
}

void HotkeyPage::saveUiToProfile(int index) {
    if (index < 0 || index >= m_profileList->count()) {
        return;
    }
    auto* item = m_profileList->item(index);
    item->setData(Qt::UserRole, m_profileName->text());
    if (auto* w = m_profileList->itemWidget(item))
        if (auto* n = w->findChild<QLabel*>("pname"))
            n->setText(m_profileName->text());
}
