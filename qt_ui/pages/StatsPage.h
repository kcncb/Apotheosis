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
    // 三个延迟都来自端到端探针(runtime/latency_probe.h), 不再由本页自行推算。
    // 负数 = 尚无数据, 显示 "--"; 不要拿 0 当"没有数据" —— 0 是合法的测量值。
    void setCaptureLatency(double ms);     // 采集产出 -> 检测器取帧
    void setInferenceLatency(double ms);   // 取帧 -> 结果发布
    void setTotalLatency(double ms);       // 采集产出 -> 控制环消费 (T0->T3)
    void setGpuMemory(const QString& text);
    void setCpuCores(const QString& text);
    // 采集链路分段(单位 ms)。原先这里是 eth_capture 的网络接收诊断, 而网络后端
    // 早已删除, 五项恒为 0 —— 换成采集卡路径真正测得到的几段。
    //   deviceAgeUs     设备侧帧龄(<0 = 该驱动不提供 sample 时间戳)
    //   capToDetectMs   采集产出 -> 检测器取帧
    //   inferMs         取帧 -> 结果发布(含预处理 / D2H / NMS)
    //   publishToAimMs  结果发布 -> 控制环消费
    //   endToEndMs      采集产出 -> 位移写出 (下界)
    void setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                    double publishToAimMs, double endToEndMs);

private:
    QLabel* m_fpsValue{};
    QLabel* m_sourceFpsValue{};
    QLabel* m_captureLatency{};
    QLabel* m_inferenceLatency{};
    QLabel* m_totalLatency{};
    QLabel* m_gpuMemory{};
    QLabel* m_cpuCores{};
    QLabel* m_diagDeviceAge{};
    QLabel* m_diagCapToDetect{};
    QLabel* m_diagInfer{};
    QLabel* m_diagPublishToAim{};
    QLabel* m_diagEndToEnd{};
    QLabel* m_mouseQueueLatency{};
    QLabel* m_mouseQueueBacklog{};
    QLabel* m_mouseSendFailures{};
    FpsGraphWidget* m_graph{};
};
