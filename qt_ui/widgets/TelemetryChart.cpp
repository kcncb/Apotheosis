#include "widgets/TelemetryChart.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

TelemetryChart::TelemetryChart(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(150);
}

void TelemetryChart::addDataPoint(double value) {
    m_data.append(value);
    while (m_data.size() > m_maxPoints)
        m_data.removeFirst();
    update();
}

void TelemetryChart::setAccent(const QColor& color) {
    m_accent = color;
    update();
}

void TelemetryChart::setMaxPoints(int n) {
    m_maxPoints = std::max(2, n);
}

void TelemetryChart::clear() {
    m_data.clear();
    update();
}

void TelemetryChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor grid(0, 0, 0, 12);
    const int left = 6, right = 6, top = 10, bottom = 12;
    const double plotW = width() - left - right;
    const double plotH = height() - top - bottom;
    if (plotW <= 0 || plotH <= 0) return;

    constexpr int kLines = 3;
    p.setPen(grid);
    for (int i = 0; i <= kLines; ++i) {
        const double y = top + plotH * i / kLines;
        p.drawLine(QPointF(left, y), QPointF(left + plotW, y));
    }

    if (m_data.size() < 2) return;

    double dmin = m_data.first(), dmax = m_data.first();
    for (double v : m_data) {
        dmin = std::min(dmin, v);
        dmax = std::max(dmax, v);
    }
    double span = dmax - dmin;
    if (span < 1e-6) span = 1.0;
    const double pad = span * 0.28;
    const double lo = dmin - pad;
    const double hi = dmax + pad;

    const int count = m_data.size();
    const double stepX = plotW / (m_maxPoints - 1);
    const double startX = left + (m_maxPoints - count) * stepX;

    auto pointAt = [&](int i) {
        const double x = startX + i * stepX;
        const double t = (m_data[i] - lo) / (hi - lo);
        const double y = top + (1.0 - t) * plotH;
        return QPointF(x, y);
    };

    QPainterPath area;
    area.moveTo(pointAt(0).x(), top + plotH);
    for (int i = 0; i < count; ++i)
        area.lineTo(pointAt(i));
    area.lineTo(pointAt(count - 1).x(), top + plotH);
    area.closeSubpath();

    QColor fill = m_accent;
    fill.setAlpha(28);
    p.fillPath(area, fill);

    QPainterPath line;
    line.moveTo(pointAt(0));
    for (int i = 1; i < count; ++i)
        line.lineTo(pointAt(i));
    QPen pen(m_accent, 2);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawPath(line);

    const QPointF last = pointAt(count - 1);
    p.setBrush(m_accent);
    p.setPen(QPen(QColor("#FFFFFF"), 2));
    p.drawEllipse(last, 3.5, 3.5);
}
