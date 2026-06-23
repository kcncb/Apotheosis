#pragma once

#include <QVector>
#include <QWidget>

#include <array>

// Mouse-drawn aim-trajectory curve editor.
//
// Canvas is parameterized as X∈[0,1] (path progress: start→end) and
// Y∈[-1,1] (perpendicular deviation from the straight line, in units of
// path length). The user holds LMB and drags a single stroke from the
// left edge to the right edge; on release the polyline is RESAMPLED to
// `kSampleCount` uniform-X samples and emitted via `curveChanged`.
//
// The widget enforces:
//   - X is monotone non-decreasing (the resampler ignores backtracks)
//   - Y is clamped to [-1, 1]
//   - First sample = 0, last sample = 0 (endpoints pinned to the axis)
//
// A "reset" button (right-click anywhere) flattens the curve back to all
// zeros (= straight line) so the user can start over without typing.
class FreehandCurveEditor : public QWidget {
    Q_OBJECT

public:
    static constexpr int kSampleCount = 32;

    explicit FreehandCurveEditor(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Y values at X = 0, 1/(N-1), 2/(N-1), ..., 1.  Always size kSampleCount.
    const std::array<float, kSampleCount>& samples() const { return m_samples; }

    // Replace samples from the outside (e.g., loading from config). Values
    // are clamped to [-1, 1] and the endpoints are forced to zero.
    void setSamples(const std::array<float, kSampleCount>& s);

    // Reset to straight line (all zeros).
    void clearCurve();

signals:
    void curveChanged(const std::array<float, kSampleCount>& samples);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    static constexpr int kPadding = 20;

    QRectF canvasRect() const;
    QPointF normToScreen(float nx, float ny) const;
    QPointF screenToNormPoint(const QPointF& screen) const;

    void resampleStrokeToSamples();

    // Persisted output.
    std::array<float, kSampleCount> m_samples{};

    // In-progress freehand stroke (raw screen-space points captured while
    // LMB is held). Resampled to m_samples on release.
    QVector<QPointF> m_stroke;
    bool m_drawing = false;
};
