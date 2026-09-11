#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class MetricCard;
class TelemetryChart;

// 概览仪表盘首屏:状态英雄区 + KPI 指标卡 + 实时遥测曲线 + 接收诊断。
// 纯 Qt,数据通过 setter 喂入,会话操作通过信号外发 —— 既可被预览程序用假数据
// 驱动,也可被真实 MainWindow 接到 runtime。
class OverviewPage : public QWidget {
    Q_OBJECT

public:
    explicit OverviewPage(QWidget* parent = nullptr);

    void setFps(double fps);
    void setSourceFps(double fps);
    // 负数 = 尚无数据(显示 "--")。数值来自端到端探针, 见 runtime/latency_probe.h。
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);   // 端到端下界: 采集产出 -> 位移写出
    void setDetectionCount(int boxes, int locked);
    // 采集链路分段: 设备帧龄 / 采集→取帧 / 推理 / 发布→消费 / 全链路, 单位 ms。
    // deviceAgeUs < 0 = 该驱动不提供设备时间戳。
    void setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                    double publishToAimMs, double endToEndMs);
    void setSessionState(bool running, const QString& model,
                         const QString& backend, const QString& uptime);

signals:
    void startStopRequested();
    void previewRequested();

private:
    QLabel* m_heroChip{};
    QLabel* m_heroTitle{};
    QLabel* m_heroSub{};
    QPushButton* m_startBtn{};

    MetricCard* m_mFps{};
    MetricCard* m_mInfer{};
    MetricCard* m_mTotal{};
    MetricCard* m_mTargets{};

    TelemetryChart* m_chart{};
    QLabel* m_chartValue{};

    QLabel* m_diagDeviceAge{};
    QLabel* m_diagCapToDetect{};
    QLabel* m_diagInfer{};
    QLabel* m_diagPublishToAim{};
    QLabel* m_diagEndToEnd{};

    bool m_running = false;
};
