#include "pages/OverviewPage.h"
#include "widgets/IconFont.h"
#include "widgets/MetricCard.h"
#include "widgets/TelemetryChart.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QFrame* makeCard() {
    auto* f = new QFrame;
    f->setObjectName("card");
    f->setAttribute(Qt::WA_StyledBackground, true);
    return f;
}

QLabel* makeChip(const QString& icon, const QString& bg, const QString& color) {
    auto* c = new QLabel;
    c->setObjectName("iconChip");
    c->setFixedSize(30, 30);
    c->setAlignment(Qt::AlignCenter);
    QString css = QStringLiteral("background:%1; border-radius:8px; color:%2;").arg(bg, color);
    c->setStyleSheet(css);
    if (IconFont::available()) {
        c->setFont(IconFont::font(17));
        c->setText(QString(IconFont::glyph(icon)));
    }
    return c;
}

QWidget* makeDiagRow(const QString& caption, QLabel*& valueOut) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto* cap = new QLabel(caption);
    cap->setProperty("class", "secondary");

    valueOut = new QLabel(QStringLiteral("0"));
    valueOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    valueOut->setStyleSheet(QStringLiteral("color:#16A34A; background:transparent;"));

    h->addWidget(cap);
    h->addStretch();
    h->addWidget(valueOut);
    return row;
}

}  // namespace

OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget;
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(16, 16, 16, 16);
    col->setSpacing(13);
    scroll->setWidget(content);

    // ── Hero ──
    auto* hero = makeCard();
    auto* heroRow = new QHBoxLayout(hero);
    heroRow->setContentsMargins(17, 15, 17, 15);
    heroRow->setSpacing(14);

    m_heroChip = makeChip(QStringLiteral("gauge"), QStringLiteral("#E7F6EC"), QStringLiteral("#16A34A"));
    heroRow->addWidget(m_heroChip);

    auto* heroText = new QVBoxLayout;
    heroText->setContentsMargins(0, 0, 0, 0);
    heroText->setSpacing(2);
    m_heroTitle = new QLabel(QStringLiteral("推理运行中"));
    m_heroTitle->setObjectName("heroTitle");
    m_heroSub = new QLabel(QStringLiteral("--"));
    m_heroSub->setProperty("class", "tertiary");
    heroText->addWidget(m_heroTitle);
    heroText->addWidget(m_heroSub);
    heroRow->addLayout(heroText);
    heroRow->addStretch();

    auto* previewBtn = new QPushButton(QStringLiteral("预览"));
    previewBtn->setCursor(Qt::PointingHandCursor);
    connect(previewBtn, &QPushButton::clicked, this, &OverviewPage::previewRequested);
    heroRow->addWidget(previewBtn);

    m_startBtn = new QPushButton(QStringLiteral("停止推理"));
    m_startBtn->setProperty("class", "danger");
    m_startBtn->setCursor(Qt::PointingHandCursor);
    connect(m_startBtn, &QPushButton::clicked, this, &OverviewPage::startStopRequested);
    heroRow->addWidget(m_startBtn);

    col->addWidget(hero);

    // ── KPI row ──
    auto* kpiRow = new QHBoxLayout;
    kpiRow->setContentsMargins(0, 0, 0, 0);
    kpiRow->setSpacing(12);

    m_mFps = new MetricCard(QStringLiteral("采集 FPS"), QStringLiteral("gauge"));
    m_mInfer = new MetricCard(QStringLiteral("推理延迟"), QStringLiteral("cpu"));
    m_mInfer->setUnit(QStringLiteral("ms"));
    m_mTotal = new MetricCard(QStringLiteral("端到端延迟"), QStringLiteral("history"));
    m_mTotal->setUnit(QStringLiteral("ms"));
    m_mTargets = new MetricCard(QStringLiteral("目标数"), QStringLiteral("target"));

    kpiRow->addWidget(m_mFps);
    kpiRow->addWidget(m_mInfer);
    kpiRow->addWidget(m_mTotal);
    kpiRow->addWidget(m_mTargets);
    col->addLayout(kpiRow);

    // ── Chart + diagnostics ──
    auto* lowerRow = new QHBoxLayout;
    lowerRow->setContentsMargins(0, 0, 0, 0);
    lowerRow->setSpacing(13);

    auto* chartCard = makeCard();
    auto* chartCol = new QVBoxLayout(chartCard);
    chartCol->setContentsMargins(17, 15, 17, 15);
    chartCol->setSpacing(6);

    auto* chartHead = new QHBoxLayout;
    auto* chartTitleCol = new QVBoxLayout;
    chartTitleCol->setSpacing(1);
    auto* chartTitle = new QLabel(QStringLiteral("采集 FPS"));
    chartTitle->setProperty("class", "heading");
    auto* chartSub = new QLabel(QStringLiteral("最近 60 秒 · 每 250ms 采样"));
    chartSub->setProperty("class", "tertiary");
    chartTitleCol->addWidget(chartTitle);
    chartTitleCol->addWidget(chartSub);
    chartHead->addLayout(chartTitleCol);
    chartHead->addStretch();
    m_chartValue = new QLabel(QStringLiteral("--"));
    m_chartValue->setObjectName("metricValue");
    chartHead->addWidget(m_chartValue, 0, Qt::AlignBottom);
    chartCol->addLayout(chartHead);

    m_chart = new TelemetryChart;
    chartCol->addWidget(m_chart);
    lowerRow->addWidget(chartCard, 17);

    auto* diagCard = makeCard();
    auto* diagCol = new QVBoxLayout(diagCard);
    diagCol->setContentsMargins(16, 15, 16, 15);
    diagCol->setSpacing(2);
    auto* diagTitle = new QLabel(QStringLiteral("接收诊断"));
    diagTitle->setProperty("class", "heading");
    auto* diagSub = new QLabel(QStringLiteral("丢帧来源定位"));
    diagSub->setProperty("class", "tertiary");
    diagCol->addWidget(diagTitle);
    diagCol->addWidget(diagSub);
    diagCol->addSpacing(4);
    diagCol->addWidget(makeDiagRow(QStringLiteral("发送端间隔"), m_rxSender));
    diagCol->addWidget(makeDiagRow(QStringLiteral("线路丢包"), m_rxWire));
    diagCol->addWidget(makeDiagRow(QStringLiteral("分片丢失"), m_rxPartial));
    diagCol->addWidget(makeDiagRow(QStringLiteral("内核丢弃"), m_rxKernel));
    diagCol->addWidget(makeDiagRow(QStringLiteral("网卡丢弃"), m_rxIf));
    diagCol->addStretch();
    lowerRow->addWidget(diagCard, 10);

    col->addLayout(lowerRow);
    col->addStretch();
}

void OverviewPage::setFps(double fps) {
    m_mFps->setValue(QString::number(fps, 'f', 0));
    m_chartValue->setText(QString::number(fps, 'f', 0));
    m_chart->addDataPoint(fps);
}

void OverviewPage::setSourceFps(double fps) {
    m_mFps->setSub(QStringLiteral("源 %1 帧").arg(fps, 0, 'f', 0), QStringLiteral("#16A34A"));
}

void OverviewPage::setInferenceLatency(double ms) {
    m_mInfer->setValue(QString::number(ms, 'f', 1));
    m_mInfer->setSub(QStringLiteral("推理引擎"));
}

void OverviewPage::setTotalLatency(double ms) {
    m_mTotal->setValue(QString::number(ms, 'f', 1));
    m_mTotal->setSub(QStringLiteral("采集 → 落点"), QStringLiteral("#16A34A"));
}

void OverviewPage::setDetectionCount(int boxes, int locked) {
    m_mTargets->setValue(QString::number(locked >= 0 ? locked : boxes));
    m_mTargets->setSub(QStringLiteral("当前帧 · 检测 %1").arg(boxes));
}

void OverviewPage::setReceiverDiagnostics(int senderSpan, int wireLost, int partialLost,
                                          int kernelDropped, int ifDropped) {
    auto apply = [](QLabel* lbl, int v) {
        lbl->setText(QString::number(v));
        if (v > 0)
            lbl->setStyleSheet(QStringLiteral("color:#B45309; background:#FBF0DD; padding:1px 8px; border-radius:6px;"));
        else
            lbl->setStyleSheet(QStringLiteral("color:#16A34A; background:transparent;"));
    };
    apply(m_rxSender, senderSpan);
    apply(m_rxWire, wireLost);
    apply(m_rxPartial, partialLost);
    apply(m_rxKernel, kernelDropped);
    apply(m_rxIf, ifDropped);
}

void OverviewPage::setSessionState(bool running, const QString& model,
                                   const QString& backend, const QString& uptime) {
    m_running = running;
    if (running) {
        m_heroTitle->setText(QStringLiteral("推理运行中"));
        QString sub = QStringLiteral("%1 · %2").arg(model, backend);
        if (!uptime.isEmpty())
            sub += QStringLiteral(" · 已运行 %1").arg(uptime);
        m_heroSub->setText(sub);
        m_heroChip->setStyleSheet(QStringLiteral("background:#E7F6EC; border-radius:8px; color:#16A34A;"));
        m_startBtn->setText(QStringLiteral("停止推理"));
        m_startBtn->setProperty("class", "danger");
    } else {
        m_heroTitle->setText(QStringLiteral("推理已停止"));
        m_heroSub->setText(QStringLiteral("%1 · %2").arg(model, backend));
        m_heroChip->setStyleSheet(QStringLiteral("background:#F1F1F4; border-radius:8px; color:#9A9AA2;"));
        m_startBtn->setText(QStringLiteral("启动推理"));
        m_startBtn->setProperty("class", "primary");
    }
    m_startBtn->style()->unpolish(m_startBtn);
    m_startBtn->style()->polish(m_startBtn);
}
