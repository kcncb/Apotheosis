#include "widgets/BezierEditor.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

BezierEditor::BezierEditor(QWidget* parent)
    : QWidget(parent) {
    setFixedSize(sizeHint());
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
}

QSize BezierEditor::sizeHint() const {
    return {320, 200};
}

void BezierEditor::setCx1(float v) { m_cx1 = v; update(); }
void BezierEditor::setCy1(float v) { m_cy1 = v; update(); }
void BezierEditor::setCx2(float v) { m_cx2 = v; update(); }
void BezierEditor::setCy2(float v) { m_cy2 = v; update(); }

void BezierEditor::setCurve(float cx1, float cy1, float cx2, float cy2) {
    m_cx1 = cx1;
    m_cy1 = cy1;
    m_cx2 = cx2;
    m_cy2 = cy2;
    update();
    emit curveChanged(m_cx1, m_cy1, m_cx2, m_cy2);
}

QRectF BezierEditor::canvasRect() const {
    return QRectF(kPadding, kPadding,
                  width() - 2 * kPadding,
                  height() - 2 * kPadding);
}

QPointF BezierEditor::normToScreen(float nx, float ny) const {
    auto r = canvasRect();
    float sx = r.left() + nx * r.width();
    float sy = r.top() + (0.5f - ny * 0.5f) * r.height();
    return {static_cast<double>(sx), static_cast<double>(sy)};
}

QPair<float, float> BezierEditor::screenToNorm(const QPointF& screen) const {
    auto r = canvasRect();
    float nx = static_cast<float>((screen.x() - r.left()) / r.width());
    float ny = (0.5f - static_cast<float>((screen.y() - r.top()) / r.height())) * 2.0f;
    nx = std::clamp(nx, 0.0f, 1.0f);
    ny = std::clamp(ny, -1.0f, 1.0f);
    return {nx, ny};
}

void BezierEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    auto r = canvasRect();

    // Canvas background
    p.setPen(QPen(QColor("#E5E5E7"), 1));
    p.setBrush(QColor("#FAFAFA"));
    p.drawRoundedRect(r, 4, 4);

    // Horizontal center line (Y=0)
    auto centerY = normToScreen(0, 0).y();
    p.setPen(QPen(QColor("#E5E5E7"), 1, Qt::DashLine));
    p.drawLine(QPointF(r.left(), centerY), QPointF(r.right(), centerY));

    auto p0 = normToScreen(0, 0);
    auto p1 = normToScreen(m_cx1, m_cy1);
    auto p2 = normToScreen(m_cx2, m_cy2);
    auto p3 = normToScreen(1, 0);

    // Control handle lines (dashed)
    QPen handlePen(QColor("#4A7FE5"), 1, Qt::DashLine);
    p.setPen(handlePen);
    p.setBrush(Qt::NoBrush);
    p.drawLine(p0, p1);
    p.drawLine(p3, p2);

    // Bezier curve
    QPainterPath path;
    path.moveTo(p0);
    path.cubicTo(p1, p2, p3);
    p.setPen(QPen(QColor("#4A7FE5"), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Endpoints (small, dark)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#86868B"));
    p.drawEllipse(p0, 3, 3);
    p.drawEllipse(p3, 3, 3);

    // Control points (larger, accent)
    p.setBrush(QColor("#4A7FE5"));
    p.drawEllipse(p1, kPointRadius, kPointRadius);
    p.drawEllipse(p2, kPointRadius, kPointRadius);
}

void BezierEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }

    auto pos = event->position();
    auto p1 = normToScreen(m_cx1, m_cy1);
    auto p2 = normToScreen(m_cx2, m_cy2);

    auto dist = [&](QPointF a) {
        auto d = pos - a;
        return std::sqrt(d.x() * d.x() + d.y() * d.y());
    };

    float d1 = static_cast<float>(dist(p1));
    float d2 = static_cast<float>(dist(p2));

    if (d1 <= kHitRadius && d1 <= d2) {
        m_dragging = 1;
    } else if (d2 <= kHitRadius) {
        m_dragging = 2;
    }
}

void BezierEditor::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging < 0) {
        return;
    }

    auto [nx, ny] = screenToNorm(event->position());

    if (m_dragging == 1) {
        m_cx1 = nx;
        m_cy1 = ny;
    } else {
        m_cx2 = nx;
        m_cy2 = ny;
    }

    update();
    emit curveChanged(m_cx1, m_cy1, m_cx2, m_cy2);
}

void BezierEditor::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = -1;
}
