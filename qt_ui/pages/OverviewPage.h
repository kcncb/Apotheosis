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
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);
    void setDetectionCount(int boxes, int locked);
    void setReceiverDiagnostics(int senderSpan, int wireLost, int partialLost,
                                int kernelDropped, int ifDropped);
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

    QLabel* m_rxSender{};
    QLabel* m_rxWire{};
    QLabel* m_rxPartial{};
    QLabel* m_rxKernel{};
    QLabel* m_rxIf{};

    bool m_running = false;
};
