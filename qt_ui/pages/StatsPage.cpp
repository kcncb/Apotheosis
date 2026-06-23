#include "pages/StatsPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollArea>
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
    auto* perfCard = new CardWidget(QStringLiteral("实时性能"), QStringLiteral("gauge"));

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
    metricGrid->addWidget(makeMetricCell(QStringLiteral("采集 FPS"), m_fpsValue),         0, 0);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("产帧 FPS"), m_sourceFpsValue),   0, 1);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("采集延迟"), m_captureLatency),   1, 0);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("推理延迟"), m_inferenceLatency), 1, 1);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("总延迟"),   m_totalLatency),     2, 0);
    metricGrid->setColumnStretch(0, 1);
    metricGrid->setColumnStretch(1, 1);

    perfCard->contentLayout()->addLayout(metricGrid);
    layout->addWidget(perfCard);

    // ── Card 2: 性能图表 ──
    auto* graphCard = new CardWidget(QStringLiteral("性能图表"), QStringLiteral("chart-line"));
    m_graph = new FpsGraphWidget;
    graphCard->contentLayout()->addWidget(m_graph);
    layout->addWidget(graphCard);

    // ── Card 3: 接收诊断 (collapsible) ──
    // 把"产帧 FPS 为什么上不去"拆成五项互不重叠的桶,直接定位损失发生在哪一层:
    //   发包速率 — sender 真实发出的帧数(理论 = sender FPS,接收端通过 frameId
    //              跨度反推。低于 sender 设定 = sender 自己没发够)。
    //   网络丢帧 — sender 发了但一个 fragment 都没到的帧(senderSpan - started)。
    //              基本就是 wire/pcap kernel ring 满前 drop 整组包,以及网线/
    //              交换机抖动。
    //   重组失败 — 收到 fragment 但凑不齐整帧(started - decoded)。某个分片晚
    //              到/丢了,无法拼。
    //   pcap内核丢 — pcap_stats 的 ps_drop:内核 ring buffer 满后 drop。如果这
    //              个非 0,说明缓冲不够或 receive thread 抢不到 CPU。
    //   NIC驱动丢 — pcap_stats 的 ps_ifdrop:NIC/驱动层 drop,通常意味着 RX 描
    //              述符不足/中断处理慢,要去网卡设置里调 RSS / 增大 Rx buffer。
    auto* rxCard = new CardWidget(QStringLiteral("接收诊断 (eth_capture, 每秒)"), QStringLiteral("activity"));
    rxCard->setCollapsible(true);

    auto addRxRow = [rxCard](const QString& caption, QLabel*& outLabel) {
        outLabel = new QLabel(QStringLiteral("--"));
        outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rxCard->contentLayout()->addWidget(FormKit::fieldRow(caption, outLabel));
    };
    addRxRow(QStringLiteral("发包速率"),    m_rxSenderSpan);
    addRxRow(QStringLiteral("网络丢帧"),    m_rxWireLost);
    addRxRow(QStringLiteral("重组失败"),    m_rxPartialLost);
    addRxRow(QStringLiteral("pcap 内核丢"), m_rxPcapKernelDrop);
    addRxRow(QStringLiteral("NIC 驱动丢"),  m_rxPcapIfDrop);

    layout->addWidget(rxCard);

    // ── Card 4: 系统资源 (collapsible) ──
    auto* sysCard = new CardWidget(QStringLiteral("系统资源"), QStringLiteral("cpu"));
    sysCard->setCollapsible(true);

    m_gpuMemory = new QLabel(QStringLiteral("--"));
    m_gpuMemory->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("GPU 显存预留"), m_gpuMemory));

    m_cpuCores = new QLabel(QStringLiteral("--"));
    m_cpuCores->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("CPU 核心预留"), m_cpuCores));

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

void StatsPage::setCaptureLatency(double ms) {
    m_captureLatency->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
}

void StatsPage::setInferenceLatency(double ms) {
    m_inferenceLatency->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
}

void StatsPage::setTotalLatency(double ms) {
    m_totalLatency->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));
}

void StatsPage::setGpuMemory(const QString& text) {
    m_gpuMemory->setText(text);
}

void StatsPage::setCpuCores(const QString& text) {
    m_cpuCores->setText(text);
}

void StatsPage::setReceiverDiagnostics(int senderSpanFps,
                                       int wireLostFps,
                                       int partialLostFps,
                                       int pcapKernelDroppedFps,
                                       int pcapIfDroppedFps) {
    auto fmt = [](int v) {
        return (v > 0) ? QString::number(v) : QStringLiteral("0");
    };
    if (m_rxSenderSpan)     m_rxSenderSpan->setText(fmt(senderSpanFps));
    if (m_rxWireLost)       m_rxWireLost->setText(fmt(wireLostFps));
    if (m_rxPartialLost)    m_rxPartialLost->setText(fmt(partialLostFps));
    if (m_rxPcapKernelDrop) m_rxPcapKernelDrop->setText(fmt(pcapKernelDroppedFps));
    if (m_rxPcapIfDrop)     m_rxPcapIfDrop->setText(fmt(pcapIfDroppedFps));
}
