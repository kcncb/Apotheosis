#include "widgets/FreehandCurveEditor.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

FreehandCurveEditor::FreehandCurveEditor(QWidget* parent)
    : QWidget(parent) {
    setFixedSize(sizeHint());
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    m_samples.fill(0.0f);
}

QSize FreehandCurveEditor::sizeHint() const {
    return {320, 200};
}

QRectF FreehandCurveEditor::canvasRect() const {
    return QRectF(kPadding, kPadding,
                  width()  - 2 * kPadding,
                  height() - 2 * kPadding);
}

QPointF FreehandCurveEditor::normToScreen(float nx, float ny) const {
    const QRectF r = canvasRect();
    const double sx = r.left() + nx * r.width();
    const double sy = r.top()  + (0.5f - ny * 0.5f) * r.height();
    return {sx, sy};
}

QPointF FreehandCurveEditor::screenToNormPoint(const QPointF& screen) const {
    const QRectF r = canvasRect();
    const float nx = std::clamp(static_cast<float>((screen.x() - r.left()) / r.width()),
                                0.0f, 1.0f);
    const float ny = std::clamp(
        (0.5f - static_cast<float>((screen.y() - r.top()) / r.height())) * 2.0f,
        -1.0f, 1.0f);
    return QPointF(nx, ny);
}

void FreehandCurveEditor::setSamples(const std::array<float, kSampleCount>& s) {
    m_samples = s;
    for (auto& v : m_samples) v = std::clamp(v, -1.0f, 1.0f);
    // Endpoints pinned to axis — the path always starts and ends at the
    // straight line, deviation is only allowed in between.
    m_samples.front() = 0.0f;
    m_samples.back()  = 0.0f;
    update();
}

void FreehandCurveEditor::clearCurve() {
    m_samples.fill(0.0f);
    update();
    emit curveChanged(m_samples);
}

void FreehandCurveEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        clearCurve();
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    m_stroke.clear();
    m_stroke.append(screenToNormPoint(event->position()));
    m_drawing = true;
    update();
}

void FreehandCurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!m_drawing) return;
    const QPointF n = screenToNormPoint(event->position());
    // Coalesce points that landed in the same X bucket to keep the stroke
    // vector reasonable on slow drags.
    if (m_stroke.isEmpty() || std::abs(n.x() - m_stroke.last().x()) > 1e-4)
        m_stroke.append(n);
    else
        m_stroke.last() = n;
    update();
}

void FreehandCurveEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_drawing || event->button() != Qt::LeftButton) return;
    m_drawing = false;

    if (m_stroke.size() >= 2)
    {
        resampleStrokeToSamples();
        emit curveChanged(m_samples);
    }

    m_stroke.clear();
    update();
}

void FreehandCurveEditor::resampleStrokeToSamples() {
    // Build a monotone-X version of the stroke. When the user drags
    // backwards, we keep the LAST visited Y at each X bucket — this is
    // the most intuitive behaviour for "I drew a wobble, take the
    // freshest pass".
    QVector<QPointF> mono;
    mono.reserve(m_stroke.size());
    float maxX = -1.0f;
    for (const auto& p : m_stroke) {
        if (p.x() > maxX) {
            mono.append(p);
            maxX = static_cast<float>(p.x());
        } else {
            // Same-or-backwards X — overwrite the last entry's Y so the
            // newest pass wins, but only if X is reasonably close to the
            // previous max (within 5% of the canvas).
            if (!mono.isEmpty() && (maxX - p.x()) < 0.05)
                mono.last().setY(p.y());
        }
    }
    if (mono.size() < 2) return;

    // Pin endpoints to (0,0) and (1,0): the curve is a deviation OFF the
    // axis, so the integration must start and end on the axis to actually
    // reach the target.
    if (mono.first().x() > 0.001) mono.prepend(QPointF(0.0, 0.0));
    if (mono.last().x()  < 0.999) mono.append(QPointF(1.0, 0.0));
    // (Force endpoints regardless of where the stroke started/ended.)
    mono.first() = QPointF(0.0, 0.0);
    mono.last()  = QPointF(1.0, 0.0);

    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };

    for (int i = 0; i < kSampleCount; ++i) {
        const double x = static_cast<double>(i) / (kSampleCount - 1);
        // Find the segment containing x.
        int j = 0;
        while (j + 1 < mono.size() && mono[j + 1].x() < x) ++j;
        if (j + 1 >= mono.size()) {
            m_samples[i] = 0.0f;
            continue;
        }
        const double x0 = mono[j].x(),     y0 = mono[j].y();
        const double x1 = mono[j + 1].x(), y1 = mono[j + 1].y();
        const double t  = (x1 > x0 + 1e-6) ? (x - x0) / (x1 - x0) : 0.0;
        m_samples[i] = static_cast<float>(std::clamp(lerp(y0, y1, t), -1.0, 1.0));
    }
    m_samples.front() = 0.0f;
    m_samples.back()  = 0.0f;
}

void FreehandCurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = canvasRect();

    // Canvas background.
    p.setPen(QPen(QColor("#E5E5E7"), 1));
    p.setBrush(QColor("#FAFAFA"));
    p.drawRoundedRect(r, 4, 4);

    // Horizontal center line (Y=0).
    const double centerY = normToScreen(0, 0).y();
    p.setPen(QPen(QColor("#E5E5E7"), 1, Qt::DashLine));
    p.drawLine(QPointF(r.left(), centerY), QPointF(r.right(), centerY));

    // Stored sample polyline.
    QPainterPath path;
    // 内部保留 32768 点，显示时按画布像素宽度降采样，避免每帧
    // 提交数万个无法在屏幕上区分的绘图顶点。
    const int displayCount = std::clamp(static_cast<int>(r.width()) * 2, 64, 2048);
    for (int di = 0; di < displayCount; ++di) {
        const float x = static_cast<float>(di) / (displayCount - 1);
        const int i = std::clamp(static_cast<int>(std::lround(x * (kSampleCount - 1))),
                                 0, kSampleCount - 1);
        const QPointF pt = normToScreen(x, m_samples[i]);
        if (di == 0) path.moveTo(pt);
        else        path.lineTo(pt);
    }
    p.setPen(QPen(QColor("#4A7FE5"), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Sample dots.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#4A7FE5"));
    constexpr int visibleDots = 128;
    for (int di = 0; di < visibleDots; ++di) {
        const float x = static_cast<float>(di) / (visibleDots - 1);
        const int i = std::clamp(static_cast<int>(std::lround(x * (kSampleCount - 1))),
                                 0, kSampleCount - 1);
        p.drawEllipse(normToScreen(x, m_samples[i]), 1.5, 1.5);
    }

    // In-progress stroke (lighter color over the top).
    if (m_drawing && m_stroke.size() >= 2) {
        QPainterPath sp;
        sp.moveTo(normToScreen(static_cast<float>(m_stroke[0].x()),
                               static_cast<float>(m_stroke[0].y())));
        for (int i = 1; i < m_stroke.size(); ++i)
            sp.lineTo(normToScreen(static_cast<float>(m_stroke[i].x()),
                                   static_cast<float>(m_stroke[i].y())));
        p.setPen(QPen(QColor(255, 90, 90, 180), 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(sp);
    }

    // Hint text.
    p.setPen(QColor("#A1A1AA"));
    p.drawText(r.adjusted(4, 4, -4, -4),
               Qt::AlignTop | Qt::AlignLeft,
               QStringLiteral("\xe6\x8c\x89\xe4\xbd\x8f\xe5\xb7\xa6\xe9\x94\xae"
                              "\xe4\xbb\x8e\xe5\xb7\xa6\xe5\x88\xb0\xe5\x8f\xb3"
                              "\xe7\x94\xbb\xe6\x9b\xb2\xe7\xba\xbf,"
                              "\xe5\x8f\xb3\xe9\x94\xae\xe6\xb8\x85\xe7\xa9\xba"));
}
