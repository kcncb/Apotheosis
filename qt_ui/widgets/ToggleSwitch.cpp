#include "widgets/ToggleSwitch.h"

#include <QPainter>
#include <QPropertyAnimation>

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);

    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        auto* anim = new QPropertyAnimation(this, "pos", this);
        anim->setDuration(140);
        anim->setStartValue(m_pos);
        anim->setEndValue(on ? 1.0 : 0.0);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

QSize ToggleSwitch::sizeHint() const {
    return {46, 26};
}

void ToggleSwitch::setPos(qreal pos) {
    m_pos = pos;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    const qreal w = width();
    const qreal h = height();
    const qreal r = h / 2.0;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    p.setBrush(QColor("#E0E0E5"));
    p.drawRoundedRect(QRectF(0, 0, w, h), r, r);

    if (m_pos > 0.0) {
        p.setOpacity(m_pos);
        p.setBrush(QColor("#5E6AD2"));
        p.drawRoundedRect(QRectF(0, 0, w, h), r, r);
        p.setOpacity(1.0);
    }

    const qreal margin = 3.0;
    const qreal knobD = h - 2 * margin;
    const qreal x = margin + m_pos * (w - knobD - 2 * margin);

    p.setBrush(QColor("#FFFFFF"));
    p.setPen(QPen(QColor(0, 0, 0, 28), 0.5));
    p.drawEllipse(QRectF(x, margin, knobD, knobD));
}
