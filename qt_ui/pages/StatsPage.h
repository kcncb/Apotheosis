#pragma once

#include <QPainter>
#include <QVector>
#include <QWidget>

class QLabel;

class FpsGraphWidget : public QWidget {
    Q_OBJECT

public:
    explicit FpsGraphWidget(QWidget* parent = nullptr);
    void addDataPoint(double value);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> m_data;
    static constexpr int kMaxPoints = 120;
};

class StatsPage : public QWidget {
    Q_OBJECT

public:
    explicit StatsPage(QWidget* parent = nullptr);

    void setFps(double fps);
    void setCaptureLatency(double ms);
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);
    void setGpuMemory(const QString& text);
    void setCpuCores(const QString& text);

private:
    QLabel* m_fpsValue{};
    QLabel* m_captureLatency{};
    QLabel* m_inferenceLatency{};
    QLabel* m_totalLatency{};
    QLabel* m_gpuMemory{};
    QLabel* m_cpuCores{};
    FpsGraphWidget* m_graph{};
};
