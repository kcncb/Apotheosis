#include "widgets/SideNav.h"
#include "widgets/IconFont.h"

#include <QButtonGroup>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QIcon iconFromGlyph(const QString& name, int px, const QString& color) {
    if (!IconFont::available())
        return QIcon();
    const qreal dpr = 2.0;
    QPixmap pm(QSize(static_cast<int>(px * dpr), static_cast<int>(px * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f = IconFont::font(px);
    p.setFont(f);
    p.setPen(QColor(color));
    p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, QString(IconFont::glyph(name)));
    p.end();
    return QIcon(pm);
}

}  // namespace

SideNav::SideNav(QWidget* parent) : QWidget(parent) {
    setObjectName("sideNav");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(208);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(10, 14, 10, 14);
    col->setSpacing(2);

    m_title = new QLabel(this);
    m_title->setObjectName("sideNavTitle");
    col->addWidget(m_title);
    col->addSpacing(4);

    auto* itemsHost = new QWidget(this);
    m_itemsLayout = new QVBoxLayout(itemsHost);
    m_itemsLayout->setContentsMargins(0, 0, 0, 0);
    m_itemsLayout->setSpacing(2);
    col->addWidget(itemsHost);
    col->addStretch();

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
        recolorIcons();
        emit currentChanged(id);
    });
}

void SideNav::setItems(const QString& groupTitle, const QStringList& labels,
                       const QStringList& iconNames) {
    m_title->setText(groupTitle.toUpper());
    m_title->setVisible(!groupTitle.isEmpty());
    m_icons = iconNames;

    const auto buttons = m_group->buttons();
    for (auto* b : buttons) {
        m_group->removeButton(b);
        b->deleteLater();
    }

    for (int i = 0; i < labels.size(); ++i) {
        auto* btn = new QPushButton(labels[i]);
        btn->setObjectName("sideNavItem");
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        if (i < iconNames.size())
            btn->setIcon(iconFromGlyph(iconNames[i], 16, QStringLiteral("#929BAA")));
        btn->setIconSize(QSize(16, 16));
        m_group->addButton(btn, i);
        m_itemsLayout->addWidget(btn);
    }

    if (!labels.isEmpty())
        setCurrentIndex(0);
}

void SideNav::setCurrentIndex(int index) {
    if (auto* b = m_group->button(index))
        b->setChecked(true);
    recolorIcons();
}

int SideNav::currentIndex() const {
    return m_group->checkedId();
}

void SideNav::recolorIcons() {
    const auto buttons = m_group->buttons();
    for (auto* b : buttons) {
        const int id = m_group->id(b);
        if (id < 0 || id >= m_icons.size())
            continue;
        const QString color = b->isChecked() ? QStringLiteral("#5865D8") : QStringLiteral("#929BAA");
        b->setIcon(iconFromGlyph(m_icons[id], 16, color));
    }
}
