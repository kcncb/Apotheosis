#include "widgets/ToggleSwitch.h"

#include <QPainter>
#include <QPropertyAnimation>
#include <QSizePolicy>

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    // fieldRow 会把普通输入控件横向拉伸；开关必须保持自身尺寸，否则轨道会
    // 被拉成一整条灰色长条，看起来像参数区背后的阴影。
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());

    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        if (m_animation) {
            m_animation->stop();
            m_animation->deleteLater();
        }
        m_animation = new QPropertyAnimation(this, "pos", this);
        m_animation->setDuration(150);
        m_animation->setStartValue(m_pos);
        m_animation->setEndValue(on ? 1.0 : 0.0);
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QPropertyAnimation::finished, this, [this] {
            m_animation->deleteLater();
            m_animation = nullptr;
        });
        m_animation->start();
    });
}

QSize ToggleSwitch::sizeHint() const {
    return {46, 26};
}

void ToggleSwitch::checkStateSet() {
    QAbstractButton::checkStateSet();
    // 配置批量加载会 blockSignals()。此时 toggled 信号（包括本控件用于
    // 更新动画位置的内部连接）不会发出，必须直接同步视觉位置，否则
    // checked=false 仍会画成开启状态。
    if (signalsBlocked()) {
        if (m_animation) {
            m_animation->stop();
            m_animation->deleteLater();
            m_animation = nullptr;
        }
        m_pos = isChecked() ? 1.0 : 0.0;
        update();
    }
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

    const QColor offTrack = isEnabled() ? QColor("#D9DEE8") : QColor("#EBEDF2");
    const QColor onTrack = isEnabled() ? QColor("#5865D8") : QColor("#BFC5EA");
    p.setBrush(offTrack);
    p.drawRoundedRect(QRectF(0, 0, w, h), r, r);

    if (m_pos > 0.0) {
        p.setOpacity(m_pos);
        p.setBrush(onTrack);
        p.drawRoundedRect(QRectF(0, 0, w, h), r, r);
        p.setOpacity(1.0);
    }

    const qreal margin = 3.0;
    const qreal knobD = h - 2 * margin;
    const qreal x = margin + m_pos * (w - knobD - 2 * margin);

    p.setBrush(QColor("#FFFFFF"));
    p.setPen(QPen(QColor(0, 0, 0, 28), 0.5));
    p.drawEllipse(QRectF(x, margin, knobD, knobD));

    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(88, 101, 216, 120), 2));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), r - 1, r - 1);
    }
}
