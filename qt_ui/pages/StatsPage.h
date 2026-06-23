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
    void setSourceFps(double fps);           // 产帧侧真实 fps(wire+NVDEC)
    void setCaptureLatency(double ms);
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);
    void setGpuMemory(const QString& text);
    void setCpuCores(const QString& text);
    // 接收侧诊断(每秒滚动)。eth_capture 有数据,其他后端 0。
    void setReceiverDiagnostics(int senderSpanFps,
                                int wireLostFps,
                                int partialLostFps,
                                int pcapKernelDroppedFps,
                                int pcapIfDroppedFps);

private:
    QLabel* m_fpsValue{};
    QLabel* m_sourceFpsValue{};
    QLabel* m_captureLatency{};
    QLabel* m_inferenceLatency{};
    QLabel* m_totalLatency{};
    QLabel* m_gpuMemory{};
    QLabel* m_cpuCores{};
    QLabel* m_rxSenderSpan{};
    QLabel* m_rxWireLost{};
    QLabel* m_rxPartialLost{};
    QLabel* m_rxPcapKernelDrop{};
    QLabel* m_rxPcapIfDrop{};
    FpsGraphWidget* m_graph{};
};
