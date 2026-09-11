#include "pages/StatsPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "runtime/aim_telemetry.h"

#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

// ── FpsGraphWidget ──

FpsGraphWidget::FpsGraphWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(200);
    setFixedHeight(200);
}

void FpsGraphWidget::addDataPoint(double value) {
    m_data.append(value);
    if (m_data.size() > kMaxPoints) {
        m_data.removeFirst();
    }
    update();
}

void FpsGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor kAccent(QStringLiteral("#5E6AD2"));
    const QColor kGrid(0, 0, 0, 12);
    const QColor kAxisText(QStringLiteral("#A1A1AA"));

    const int w = width();
    const int h = height();
    const int margin = 40;
    const int graphW = w - margin * 2;
    const int graphH = h - margin * 2;

    // Transparent background — the card behind us provides the white surface.

    double maxVal = 1.0;
    for (auto v : m_data) {
        if (v > maxVal) maxVal = v;
    }
    maxVal = std::ceil(maxVal / 10.0) * 10.0;
    if (maxVal < 10.0) maxVal = 10.0;

    constexpr int kGridLines = 5;
    for (int i = 0; i <= kGridLines; ++i) {
        int y = margin + graphH - (i * graphH / kGridLines);

        p.setPen(kGrid);
        p.drawLine(margin, y, margin + graphW, y);

        p.setPen(kAxisText);
        double label = maxVal * i / kGridLines;
        p.drawText(0, y - 8, margin - 4, 16, Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(label, 'f', 0));
    }

    if (m_data.size() < 2) return;

    int count = m_data.size();
    double stepX = static_cast<double>(graphW) / (kMaxPoints - 1);
    int startX = margin + static_cast<int>((kMaxPoints - count) * stepX);

    auto pointAt = [&](int i) {
        double x = startX + i * stepX;
        double y = margin + graphH - (m_data[i] / maxVal) * graphH;
        return QPointF(x, y);
    };

    // Filled area under the curve (accent, faint).
    QPainterPath area;
    area.moveTo(pointAt(0).x(), margin + graphH);
    for (int i = 0; i < count; ++i) {
        area.lineTo(pointAt(i));
    }
    area.lineTo(pointAt(count - 1).x(), margin + graphH);
    area.closeSubpath();

    QColor fill = kAccent;
    fill.setAlpha(28);
    p.fillPath(area, fill);

    // Line on top (accent).
    QPen linePen(kAccent, 2);
    linePen.setJoinStyle(Qt::RoundJoin);
    p.setPen(linePen);
    QPointF prev;
    for (int i = 0; i < count; ++i) {
        QPointF pt = pointAt(i);
        if (i > 0) {
            p.drawLine(prev, pt);
        }
        prev = pt;
    }

    if (!m_data.isEmpty()) {
        p.setPen(kAccent);
        QFont valueFont = p.font();
        valueFont.setBold(true);
        p.setFont(valueFont);
        p.drawText(margin + graphW - 80, margin - 22, 80, 18,
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(m_data.last(), 'f', 1));
    }
}

// ── StatsPage ──

StatsPage::StatsPage(QWidget* parent)
    : QWidget(parent) {
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    // ── Card 1: 实时性能 (metric grid) ──
    auto* perfCard = new CardWidget(QString::fromUtf8(u8"实时性能"), QStringLiteral("gauge"));

    auto* metricGrid = new QGridLayout;
    metricGrid->setHorizontalSpacing(12);
    metricGrid->setVerticalSpacing(16);
    metricGrid->setContentsMargins(0, 4, 0, 4);

    auto makeMetricCell = [](const QString& caption, QLabel*& valueOut) {
        auto* cell = new QWidget;
        auto* v = new QVBoxLayout(cell);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        auto* captionLabel = new QLabel(caption);
        captionLabel->setProperty("class", "secondary");

        auto* value = new QLabel(QStringLiteral("--"));
        value->setObjectName(QStringLiteral("metricValue"));

        v->addWidget(captionLabel);
        v->addWidget(value);

        valueOut = value;
        return cell;
    };

    // "采集 FPS" = 消费循环每秒迭代数(captureFps);"产帧 FPS" = receive 线程
    // 每秒真正解码+入队的帧数(captureSourceFps,wire+NVDEC 的真实速度)。两个
    // 数字分开看能立刻判断瓶颈在采集线程还是产帧侧。
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"采集 FPS"), m_fpsValue),         0, 0);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"产帧 FPS"), m_sourceFpsValue),   0, 1);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"采集延迟"), m_captureLatency),   1, 0);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"推理延迟"), m_inferenceLatency), 1, 1);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"总延迟"),   m_totalLatency),     2, 0);
    metricGrid->setColumnStretch(0, 1);
    metricGrid->setColumnStretch(1, 1);

    perfCard->contentLayout()->addLayout(metricGrid);
    layout->addWidget(perfCard);

    // ── Card 2: 性能图表 ──
    auto* graphCard = new CardWidget(QString::fromUtf8(u8"性能图表"), QStringLiteral("chart-line"));
    m_graph = new FpsGraphWidget;
    graphCard->contentLayout()->addWidget(m_graph);
    layout->addWidget(graphCard);

    // ── Card 3: 采集诊断 (collapsible) ──
    // 采集卡路径的延迟分段(原先这里是 eth_capture 的网络接收诊断 —— 网络后端已
    // 删除, 五项恒为 0, 只会把排查延迟的人带偏)。
    //
    // 设备帧龄与回调后的软件耗时分开显示, 不据此单独判断卡芯片快慢。
    auto* rxCard = new CardWidget(QString::fromUtf8(u8"采集诊断 (采集卡)"), QStringLiteral("activity"));
    rxCard->setCollapsible(true);

    auto addRxRow = [rxCard](const QString& caption, QLabel*& outLabel) {
        outLabel = new QLabel(QStringLiteral("--"));
        outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rxCard->contentLayout()->addWidget(FormKit::fieldRow(caption, outLabel));
    };
    addRxRow(QString::fromUtf8(u8"设备帧龄 (驱动/MF)"), m_diagDeviceAge);
    addRxRow(QString::fromUtf8(u8"接收→取帧"),          m_diagCapToDetect);
    addRxRow(QString::fromUtf8(u8"推理 (含前后处理)"),  m_diagInfer);
    addRxRow(QString::fromUtf8(u8"发布→消费"),          m_diagPublishToAim);
    addRxRow(QString::fromUtf8(u8"全链路 (下界)"),      m_diagEndToEnd);

    layout->addWidget(rxCard);

    auto* mouseCard = new CardWidget(QString::fromUtf8(u8"鼠标发送队列"), QStringLiteral("activity"));
    mouseCard->setCollapsible(true);
    auto addMouseRow = [mouseCard](const QString& caption, QLabel*& output) {
        output = new QLabel(QStringLiteral("--"));
        output->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mouseCard->contentLayout()->addWidget(FormKit::fieldRow(caption, output));
    };
    addMouseRow(QString::fromUtf8(u8"发送延迟"), m_mouseQueueLatency);
    addMouseRow(QString::fromUtf8(u8"队列积压"), m_mouseQueueBacklog);
    addMouseRow(QString::fromUtf8(u8"发送失败"), m_mouseSendFailures);
    layout->addWidget(mouseCard);

    auto* mouseTimer = new QTimer(this);
    mouseTimer->setInterval(250);
    connect(mouseTimer, &QTimer::timeout, this, [this] {
        m_mouseQueueLatency->setText(
            QString::number(g_mouse_queue_latency_ms.load(), 'f', 2) + QStringLiteral(" ms"));
        m_mouseQueueBacklog->setText(QString::number(g_mouse_queue_backlog.load()));
        m_mouseSendFailures->setText(QString::number(g_mouse_send_failures.load()));
    });
    mouseTimer->start();

    // ── Card 4: 系统资源 (collapsible) ──
    auto* sysCard = new CardWidget(QString::fromUtf8(u8"系统资源"), QStringLiteral("cpu"));
    sysCard->setCollapsible(true);

    m_gpuMemory = new QLabel(QStringLiteral("--"));
    m_gpuMemory->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"GPU 显存预留"), m_gpuMemory));

    m_cpuCores = new QLabel(QStringLiteral("--"));
    m_cpuCores->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"CPU 核心预留"), m_cpuCores));

    layout->addWidget(sysCard);
    layout->addStretch();
}

void StatsPage::setFps(double fps) {
    m_fpsValue->setText(QString::number(fps, 'f', 1));
    m_graph->addDataPoint(fps);
}

void StatsPage::setSourceFps(double fps) {
    if (!m_sourceFpsValue) return;
    if (fps > 0.5)
        m_sourceFpsValue->setText(QString::number(fps, 'f', 1));
    else
        m_sourceFpsValue->setText(QStringLiteral("--"));
}

// 延迟数值统一格式化: 负数 = 尚无数据(探针还没结算过一帧), 显示 "--"。
// 不能用 0 当"没有数据": 0 ms 是一个合法测量值, 混在一起会让人以为链路变快了。
static QString fmtLatencyMs(double ms) {
    if (ms < 0.0) return QStringLiteral("--");
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
}

void StatsPage::setCaptureLatency(double ms) {
    if (m_captureLatency) m_captureLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setInferenceLatency(double ms) {
    if (m_inferenceLatency) m_inferenceLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setTotalLatency(double ms) {
    if (m_totalLatency) m_totalLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setGpuMemory(const QString& text) {
    m_gpuMemory->setText(text);
}

void StatsPage::setCpuCores(const QString& text) {
    m_cpuCores->setText(text);
}

void StatsPage::setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                           double publishToAimMs, double endToEndMs) {
    auto setMs = [](QLabel* lbl, double ms) {
        if (lbl) lbl->setText(fmtLatencyMs(ms));
    };
    // 设备帧龄用微秒精度: 对接侧正常时它经常只有零点几毫秒, 取整到 1 位小数会看
    // 不出差别, 而"有没有排队"正是靠这个小数区分的。
    if (m_diagDeviceAge) {
        m_diagDeviceAge->setText(deviceAgeUs < 0
            ? QStringLiteral("--")
            : QStringLiteral("%1 ms").arg(deviceAgeUs / 1000.0, 0, 'f', 2));
    }
    setMs(m_diagCapToDetect, capToDetectMs);
    setMs(m_diagInfer, inferMs);
    setMs(m_diagPublishToAim, publishToAimMs);
    setMs(m_diagEndToEnd, endToEndMs);
}
