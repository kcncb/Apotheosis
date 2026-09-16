#include "widgets/TopNavBar.h"
#include "widgets/IconFont.h"
#include "widgets/StatusPill.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

namespace {
constexpr int kBarHeight = 60;
constexpr int kProfileComboWidth = 132;
}

TopNavBar::TopNavBar(QWidget* parent) : QWidget(parent) {
    setObjectName("topNav");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kBarHeight);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(18, 0, 18, 0);
    row->setSpacing(0);

    // ── Brand ──
    auto* mark = new QLabel(this);
    mark->setObjectName("brandMark");
    mark->setFixedSize(29, 29);
    mark->setAlignment(Qt::AlignCenter);
    if (IconFont::available()) {
        mark->setFont(IconFont::font(17));
        mark->setText(QString(IconFont::glyph("crosshair")));
    }
    row->addWidget(mark);

    auto* word = new QLabel(QStringLiteral("Apotheosis"), this);
    word->setObjectName("brandWord");
    row->addSpacing(9);
    row->addWidget(word);

    auto* divider = new QFrame(this);
    divider->setObjectName("navDivider");
    divider->setFixedSize(1, 20);
    row->addSpacing(14);
    row->addWidget(divider);
    row->addSpacing(8);

    // ── Primary nav ──
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    m_navRow = new QHBoxLayout;
    m_navRow->setContentsMargins(0, 0, 0, 0);
    m_navRow->setSpacing(0);
    row->addLayout(m_navRow);

    connect(m_group, &QButtonGroup::idClicked, this, &TopNavBar::primaryChanged);

    row->addStretch();

    // ── Global actions ──
    // 全局配置方案: 下拉 = 一键切换, 右侧 ⋯ 菜单 = 保存 / 另存为 / 重命名 / 删除。
    // 放在最左边的全局操作位, 任何页面都能直接换方案。
    {
        auto* profileIcon = new QLabel(this);
        profileIcon->setObjectName("profileIcon");
        profileIcon->setFixedSize(18, 18);
        profileIcon->setAlignment(Qt::AlignCenter);
        if (IconFont::available()) {
            profileIcon->setFont(IconFont::font(13));
            profileIcon->setText(QString(IconFont::glyph("layers-intersect")));
        }
        profileIcon->setToolTip(QString::fromUtf8(u8"全局配置方案：整套设置(采集/AI/硬件/瞄准)的快照"));
        row->addWidget(profileIcon);
        row->addSpacing(6);

        m_profileCombo = new QComboBox(this);
        m_profileCombo->setObjectName("profileCombo");
        m_profileCombo->setFixedWidth(kProfileComboWidth);
        m_profileCombo->setCursor(Qt::PointingHandCursor);
        m_profileTip = QString::fromUtf8(
            u8"选择即切换：把整套配置换成该方案（切换前会自动保存当前方案的改动）。");
        m_profileCombo->setToolTip(m_profileTip);
        connect(m_profileCombo, QOverload<int>::of(&QComboBox::activated),
                this, [this](int index) {
            if (index < 0) return;
            emit profileSwitchRequested(m_profileCombo->itemText(index));
        });
        row->addWidget(m_profileCombo);

        m_profileMenuBtn = new QToolButton(this);
        m_profileMenuBtn->setObjectName("profileMenuBtn");
        m_profileMenuBtn->setCursor(Qt::PointingHandCursor);
        m_profileMenuBtn->setPopupMode(QToolButton::InstantPopup);
        m_profileMenuBtn->setToolTip(QString::fromUtf8(u8"配置方案管理"));
        if (IconFont::available())
            m_profileMenuBtn->setFont(IconFont::font(15));
        m_profileMenuBtn->setText(IconFont::available()
                                      ? QString(IconFont::glyph("dots-vertical"))
                                      : QStringLiteral("..."));

        auto* menu = new QMenu(m_profileMenuBtn);
        menu->addAction(QString::fromUtf8(u8"保存当前方案"), this,
                        [this] { emit profileSaveRequested(); });
        menu->addAction(QString::fromUtf8(u8"另存为..."), this,
                        [this] { emit profileSaveAsRequested(); });
        menu->addAction(QString::fromUtf8(u8"重命名..."), this,
                        [this] { emit profileRenameRequested(); });
        menu->addAction(QString::fromUtf8(u8"删除当前方案..."), this,
                        [this] { emit profileDeleteRequested(); });
        menu->addSeparator();
        menu->addAction(QString::fromUtf8(u8"刷新方案列表"), this,
                        [this] { emit profileRefreshRequested(); });
        menu->addAction(QString::fromUtf8(u8"打开方案目录"), this,
                        [this] { emit profileOpenDirRequested(); });
        m_profileMenuBtn->setMenu(menu);
        row->addWidget(m_profileMenuBtn);
    }

    row->addSpacing(10);

    m_status = new StatusPill(this);
    m_status->setStatus(QString::fromUtf8(u8"已停止"), StatusPill::Neutral);
    row->addWidget(m_status);
    row->addSpacing(10);

    m_saveButton = new QPushButton(QString::fromUtf8(u8"保存设置"), this);
    m_saveButton->setObjectName("saveBtn");
    m_saveButton->setProperty("class", "primary");
    m_saveButton->setCursor(Qt::PointingHandCursor);
    m_saveButton->setToolTip(QString::fromUtf8(u8"保存当前设置（Ctrl+S）"));
    connect(m_saveButton, &QPushButton::clicked, this, &TopNavBar::saveClicked);
    row->addWidget(m_saveButton);

    m_saveFeedbackTimer = new QTimer(this);
    m_saveFeedbackTimer->setSingleShot(true);
    connect(m_saveFeedbackTimer, &QTimer::timeout, this, [this] {
        m_saveButton->setText(QString::fromUtf8(u8"保存设置"));
        m_saveButton->setProperty("saved", false);
        m_saveButton->style()->unpolish(m_saveButton);
        m_saveButton->style()->polish(m_saveButton);
    });

    // 方案名的临时反馈 (例如「已保存」) —— 到时把高亮撤掉。
    // 注意: 下拉是不可编辑的, setCurrentText() 对不在列表里的文本不会生效,
    // 所以反馈走「高亮 + 提示」而不是改文本, 下拉始终显示真实的当前方案名。
    m_profileFeedbackTimer = new QTimer(this);
    m_profileFeedbackTimer->setSingleShot(true);
    connect(m_profileFeedbackTimer, &QTimer::timeout, this, [this] {
        m_profileCombo->setProperty("feedback", false);
        m_profileCombo->style()->unpolish(m_profileCombo);
        m_profileCombo->style()->polish(m_profileCombo);
        m_profileCombo->setToolTip(m_profileTip);
    });
}

void TopNavBar::setPrimaryItems(const QStringList& labels) {
    const auto buttons = m_group->buttons();
    for (auto* b : buttons) {
        m_group->removeButton(b);
        b->deleteLater();
    }

    for (int i = 0; i < labels.size(); ++i) {
        auto* btn = new QPushButton(labels[i], this);
        btn->setObjectName("primaryNavItem");
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(kBarHeight);
        m_group->addButton(btn, i);
        m_navRow->addWidget(btn);
    }

    if (!labels.isEmpty())
        setCurrentPrimary(0);
}

void TopNavBar::setCurrentPrimary(int index) {
    if (auto* b = m_group->button(index))
        b->setChecked(true);
}

int TopNavBar::currentPrimary() const {
    return m_group->checkedId();
}

void TopNavBar::setSessionStatus(bool running, const QString& text) {
    m_status->setStatus(text, running ? StatusPill::Success : StatusPill::Neutral);
}

void TopNavBar::showSaveFeedback() {
    m_saveButton->setText(QString::fromUtf8(u8"已保存"));
    m_saveButton->setProperty("saved", true);
    m_saveButton->style()->unpolish(m_saveButton);
    m_saveButton->style()->polish(m_saveButton);

    m_saveFeedbackTimer->start(1200);
}

// ── 全局配置方案 ───────────────────────────────────────────────────────────

void TopNavBar::setProfiles(const QStringList& names, const QString& active) {
    if (m_profileFeedbackTimer->isActive())
        m_profileFeedbackTimer->stop();
    m_profileCombo->setProperty("feedback", false);
    m_profileCombo->style()->unpolish(m_profileCombo);
    m_profileCombo->style()->polish(m_profileCombo);

    QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();
    m_profileCombo->addItems(names);
    m_profileCombo->setProperty("activeName", active);
    const int index = names.indexOf(active);
    // 活动方案不在列表里 (文件被外部删掉等) 就留空, 由 MainWindow 负责提示。
    m_profileCombo->setCurrentIndex(index);
    m_profileCombo->setEnabled(!names.isEmpty());
}

QString TopNavBar::currentProfile() const {
    return m_profileCombo->property("activeName").toString();
}

void TopNavBar::setProfileControlsEnabled(bool enabled) {
    m_profileCombo->setEnabled(enabled);
    m_profileMenuBtn->setEnabled(enabled);
}

void TopNavBar::showProfileFeedback(const QString& text) {
    m_profileCombo->setProperty("feedback", true);
    m_profileCombo->style()->unpolish(m_profileCombo);
    m_profileCombo->style()->polish(m_profileCombo);
    if (!text.isEmpty())
        m_profileCombo->setToolTip(text);
    m_profileFeedbackTimer->start(1400);
}
