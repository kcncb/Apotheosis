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

    metricGrid->addWidget(makeMetricCell(QStringLiteral("推理 FPS"), m_fpsValue),        0, 0);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("采集延迟"), m_captureLatency),   0, 1);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("推理延迟"), m_inferenceLatency), 1, 0);
    metricGrid->addWidget(makeMetricCell(QStringLiteral("总延迟"),   m_totalLatency),     1, 1);
    metricGrid->setColumnStretch(0, 1);
    metricGrid->setColumnStretch(1, 1);

    perfCard->contentLayout()->addLayout(metricGrid);
    layout->addWidget(perfCard);

    // ── Card 2: 性能图表 ──
    auto* graphCard = new CardWidget(QStringLiteral("性能图表"), QStringLiteral("chart-line"));
    m_graph = new FpsGraphWidget;
    graphCard->contentLayout()->addWidget(m_graph);
    layout->addWidget(graphCard);

    // ── Card 3: 系统资源 (collapsible) ──
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
