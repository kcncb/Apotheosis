#include "widgets/CardWidget.h"
#include "widgets/IconFont.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QVBoxLayout>

namespace {
constexpr char kAccent[] = "#5865D8";
}

CardWidget::CardWidget(const QString& title, QWidget* parent)
    : QWidget(parent) {
    init(title, QString());
}

CardWidget::CardWidget(const QString& title, const QString& iconName, QWidget* parent)
    : QWidget(parent) {
    init(title, iconName);
}

void CardWidget::init(const QString& title, const QString& iconName) {
    setObjectName("card");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    m_headerWidget = new QWidget(this);
    m_headerWidget->setObjectName("cardHeader");
    m_headerLayout = new QHBoxLayout(m_headerWidget);
    m_headerLayout->setContentsMargins(16, 14, 16, 10);
    m_headerLayout->setSpacing(8);

    m_iconLabel = new QLabel(m_headerWidget);
    m_iconLabel->hide();
    m_headerLayout->addWidget(m_iconLabel);

    m_titleLabel = new QLabel(title, m_headerWidget);
    m_titleLabel->setObjectName("cardTitle");
    QFont titleFont = m_titleLabel->font();
    titleFont.setWeight(QFont::Medium);
    m_titleLabel->setFont(titleFont);
    m_headerLayout->addWidget(m_titleLabel);

    m_headerLayout->addStretch();

    m_chevron = new QLabel(QStringLiteral("▾"), m_headerWidget);
    m_chevron->setObjectName("cardChevron");
    m_chevron->hide();
    m_headerLayout->addWidget(m_chevron);

    m_contentWidget = new QWidget(this);
    m_contentLayout = new QVBoxLayout(m_contentWidget);
    m_contentLayout->setContentsMargins(16, 2, 16, 14);
    m_contentLayout->setSpacing(9);

    outerLayout->addWidget(m_headerWidget);
    outerLayout->addWidget(m_contentWidget);

    if (!iconName.isEmpty())
        setIcon(iconName);
}

void CardWidget::setIcon(const QString& iconName) {
    if (!IconFont::available()) {
        m_iconLabel->hide();
        return;
    }
    m_iconLabel->setText(QString(IconFont::glyph(iconName)));
    m_iconLabel->setFixedWidth(20);
    m_iconLabel->setStyleSheet(
        QString("font-family:\"tabler-icons\"; font-size:16px; color:%1;").arg(kAccent));
    m_iconLabel->show();
}

QVBoxLayout* CardWidget::contentLayout() const {
    return m_contentLayout;
}

void CardWidget::setCollapsible(bool collapsible) {
    m_collapsible = collapsible;
    m_chevron->setVisible(collapsible);

    if (collapsible) {
        m_chevron->setText(m_collapsed ? QStringLiteral("▸") : QStringLiteral("▾"));
        m_headerWidget->setCursor(Qt::PointingHandCursor);
        m_headerWidget->installEventFilter(this);
    } else {
        m_headerWidget->setCursor(Qt::ArrowCursor);
        m_headerWidget->removeEventFilter(this);
    }
}

bool CardWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_headerWidget && m_collapsible
        && event->type() == QEvent::MouseButtonPress) {
        toggleCollapsed();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void CardWidget::toggleCollapsed() {
    if (m_contentAnimation) {
        m_contentAnimation->stop();
        m_contentAnimation->deleteLater();
        m_contentAnimation = nullptr;
    }

    m_collapsed = !m_collapsed;
    m_chevron->setText(m_collapsed ? QStringLiteral("▸") : QStringLiteral("▾"));

    const int expandedHeight = qMax(m_contentWidget->sizeHint().height(), m_contentWidget->height());
    if (!m_collapsed) {
        m_contentWidget->setMaximumHeight(0);
        m_contentWidget->show();
    }

    auto* animation = new QPropertyAnimation(m_contentWidget, "maximumHeight", this);
    m_contentAnimation = animation;
    animation->setDuration(170);
    animation->setStartValue(m_collapsed ? expandedHeight : 0);
    animation->setEndValue(m_collapsed ? 0 : expandedHeight);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, animation] {
        if (m_contentAnimation != animation)
            return;
        m_contentAnimation = nullptr;
        if (m_collapsed)
            m_contentWidget->hide();
        else
            m_contentWidget->setMaximumHeight(QWIDGETSIZE_MAX);
        animation->deleteLater();
    });
    animation->start();
}
