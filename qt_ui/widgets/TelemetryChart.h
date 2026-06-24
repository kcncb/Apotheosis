#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

// 共享实时遥测折线:面积 + 线 + 网格,带动态 min/max 视窗(让小幅波动也可见)。
// 由原 StatsPage::FpsGraphWidget 泛化而来,概览与统计页共用。
class TelemetryChart : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryChart(QWidget* parent = nullptr);

    void addDataPoint(double value);
    void setAccent(const QColor& color);
    void setMaxPoints(int n);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> m_data;
    QColor m_accent{QStringLiteral("#5E6AD2")};
    int m_maxPoints = 80;
};
