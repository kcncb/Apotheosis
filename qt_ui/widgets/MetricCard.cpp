#include "widgets/MetricCard.h"
#include "widgets/IconFont.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

MetricCard::MetricCard(const QString& label, const QString& iconName, QWidget* parent)
    : QFrame(parent) {
    setObjectName("metricCard");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(15, 13, 15, 13);
    col->setSpacing(3);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(6);

    auto* labelLbl = new QLabel(label, this);
    labelLbl->setProperty("class", "secondary");
    topRow->addWidget(labelLbl);
    topRow->addStretch();

    if (!iconName.isEmpty() && IconFont::available()) {
        auto* icon = IconFont::label(iconName, 16, QStringLiteral("#B8C0CC"));
        topRow->addWidget(icon);
    }
    col->addLayout(topRow);

    auto* valueRow = new QHBoxLayout;
    valueRow->setContentsMargins(0, 4, 0, 0);
    valueRow->setSpacing(3);
    valueRow->setAlignment(Qt::AlignLeft | Qt::AlignBottom);

    m_value = new QLabel(QStringLiteral("--"), this);
    m_value->setObjectName("metricValue");
    valueRow->addWidget(m_value, 0, Qt::AlignBottom);

    m_unit = new QLabel(this);
    m_unit->setProperty("class", "tertiary");
    m_unit->setContentsMargins(0, 0, 0, 3);
    valueRow->addWidget(m_unit, 0, Qt::AlignBottom);
    valueRow->addStretch();
    col->addLayout(valueRow);

    m_sub = new QLabel(this);
    m_sub->setProperty("class", "tertiary");
    col->addWidget(m_sub);
}

void MetricCard::setValue(const QString& value) {
    m_value->setText(value);
}

void MetricCard::setUnit(const QString& unit) {
    m_unit->setText(unit);
}

void MetricCard::setSub(const QString& text, const QString& cssColor) {
    m_sub->setText(text);
    if (!cssColor.isEmpty())
        m_sub->setStyleSheet(QStringLiteral("color:%1; background:transparent; font-size:12px;").arg(cssColor));
    else
        m_sub->setStyleSheet(QString());
}
