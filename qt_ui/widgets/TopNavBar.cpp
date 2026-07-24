#include "widgets/TopNavBar.h"
#include "widgets/IconFont.h"
#include "widgets/StatusPill.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTimer>

namespace {
constexpr int kBarHeight = 60;
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
