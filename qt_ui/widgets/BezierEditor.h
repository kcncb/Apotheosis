#pragma once

#include <QWidget>

class BezierEditor : public QWidget {
    Q_OBJECT
    Q_PROPERTY(float cx1 READ cx1 WRITE setCx1)
    Q_PROPERTY(float cy1 READ cy1 WRITE setCy1)
    Q_PROPERTY(float cx2 READ cx2 WRITE setCx2)
    Q_PROPERTY(float cy2 READ cy2 WRITE setCy2)

public:
    explicit BezierEditor(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    float cx1() const { return m_cx1; }
    float cy1() const { return m_cy1; }
    float cx2() const { return m_cx2; }
    float cy2() const { return m_cy2; }

    void setCx1(float v);
    void setCy1(float v);
    void setCx2(float v);
    void setCy2(float v);

    void setCurve(float cx1, float cy1, float cx2, float cy2);

signals:
    void curveChanged(float cx1, float cy1, float cx2, float cy2);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    static constexpr int kPadding = 20;
    static constexpr float kHitRadius = 12.0f;
    static constexpr float kPointRadius = 5.0f;

    QPointF normToScreen(float nx, float ny) const;
    QPair<float, float> screenToNorm(const QPointF& screen) const;
    QRectF canvasRect() const;

    float m_cx1 = 0.30f;
    float m_cy1 = 0.25f;
    float m_cx2 = 0.70f;
    float m_cy2 = -0.15f;
    int m_dragging = -1; // -1=none, 1=P1, 2=P2
};
