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
    hero->setProperty("role", "hero");
    auto* heroRow = new QHBoxLayout(hero);
    heroRow->setContentsMargins(16, 15, 16, 15);
    heroRow->setSpacing(14);

    m_heroChip = makeChip(QStringLiteral("gauge"), QStringLiteral("#E7F6EC"), QStringLiteral("#16A34A"));
    heroRow->addWidget(m_heroChip);

    auto* heroText = new QVBoxLayout;
    heroText->setContentsMargins(0, 0, 0, 0);
    heroText->setSpacing(2);
    m_heroTitle = new QLabel(QString::fromUtf8(u8"推理运行中"));
    m_heroTitle->setObjectName("heroTitle");
    m_heroSub = new QLabel(QStringLiteral("--"));
    m_heroSub->setProperty("class", "tertiary");
    heroText->addWidget(m_heroTitle);
    heroText->addWidget(m_heroSub);
    heroRow->addLayout(heroText);
    heroRow->addStretch();

    auto* previewBtn = new QPushButton(QString::fromUtf8(u8"预览"));
    previewBtn->setCursor(Qt::PointingHandCursor);
    connect(previewBtn, &QPushButton::clicked, this, &OverviewPage::previewRequested);
    heroRow->addWidget(previewBtn);

    m_startBtn = new QPushButton(QString::fromUtf8(u8"停止推理"));
    m_startBtn->setProperty("class", "danger");
    m_startBtn->setCursor(Qt::PointingHandCursor);
    connect(m_startBtn, &QPushButton::clicked, this, &OverviewPage::startStopRequested);
    heroRow->addWidget(m_startBtn);

    col->addWidget(hero);

    // ── KPI row ──
    auto* kpiRow = new QHBoxLayout;
    kpiRow->setContentsMargins(0, 0, 0, 0);
    kpiRow->setSpacing(12);

    m_mFps = new MetricCard(QString::fromUtf8(u8"采集 FPS"), QStringLiteral("gauge"));
    m_mInfer = new MetricCard(QString::fromUtf8(u8"推理延迟"), QStringLiteral("cpu"));
    m_mInfer->setUnit(QStringLiteral("ms"));
    m_mTotal = new MetricCard(QString::fromUtf8(u8"端到端延迟"), QStringLiteral("history"));
    m_mTotal->setUnit(QStringLiteral("ms"));
    m_mTargets = new MetricCard(QString::fromUtf8(u8"目标数"), QStringLiteral("target"));

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
    chartCol->setContentsMargins(16, 15, 16, 15);
    chartCol->setSpacing(6);

    auto* chartHead = new QHBoxLayout;
    auto* chartTitleCol = new QVBoxLayout;
    chartTitleCol->setSpacing(1);
    auto* chartTitle = new QLabel(QString::fromUtf8(u8"采集 FPS"));
    chartTitle->setProperty("class", "heading");
    auto* chartSub = new QLabel(QString::fromUtf8(u8"实时趋势 · 80 个采样点"));
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
    // 采集链路分段。这里原来是 eth_capture 的网络接收诊断(发包速率/丢包/内核丢),
    // 而网络后端已经删除, 五项恒为 0 —— 改成采集卡路径上真正测得到的几段。
    auto* diagTitle = new QLabel(QString::fromUtf8(u8"采集链路"));
    diagTitle->setProperty("class", "heading");
    auto* diagSub = new QLabel(QString::fromUtf8(u8"分段耗时 (ms) · 设备帧龄取决于驱动时间戳"));
    diagSub->setProperty("class", "tertiary");
    diagCol->addWidget(diagTitle);
    diagCol->addWidget(diagSub);
    diagCol->addSpacing(4);
    diagCol->addWidget(makeDiagRow(QString::fromUtf8(u8"设备帧龄"), m_diagDeviceAge));
    diagCol->addWidget(makeDiagRow(QString::fromUtf8(u8"接收→取帧"), m_diagCapToDetect));
    diagCol->addWidget(makeDiagRow(QString::fromUtf8(u8"推理"), m_diagInfer));
    diagCol->addWidget(makeDiagRow(QString::fromUtf8(u8"发布→消费"), m_diagPublishToAim));
    diagCol->addWidget(makeDiagRow(QString::fromUtf8(u8"全链路 (下界)"), m_diagEndToEnd));
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
    m_mFps->setSub(QString::fromUtf8(u8"源 %1 帧").arg(fps, 0, 'f', 0), QStringLiteral("#16A34A"));
}

// 延迟显示: 负数 = 探针还没出数(尚未跑满一帧), 显示 "--"。0 是合法测量值,
// 不能拿来当"没有数据", 否则"链路真的很快"和"根本没测"看起来一样。
static QString ovFmtMs(double ms, int precision = 1) {
    if (ms < 0.0) return QStringLiteral("--");
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', precision);
}

void OverviewPage::setInferenceLatency(double ms) {
    m_mInfer->setValue(ms < 0.0 ? QStringLiteral("--") : QString::number(ms, 'f', 1));
    m_mInfer->setSub(QString::fromUtf8(u8"推理引擎"));
}

void OverviewPage::setTotalLatency(double ms) {
    m_mTotal->setValue(ms < 0.0 ? QStringLiteral("--") : QString::number(ms, 'f', 1));
    m_mTotal->setSub(QString::fromUtf8(u8"采集 → 落点"), QStringLiteral("#16A34A"));
}

void OverviewPage::setDetectionCount(int boxes, int locked) {
    m_mTargets->setValue(QString::number(locked >= 0 ? locked : boxes));
    m_mTargets->setSub(QString::fromUtf8(u8"当前帧 · 检测 %1").arg(boxes));
}

void OverviewPage::setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                              double publishToAimMs, double endToEndMs) {
    // 设备帧龄用两位数: 对接侧正常时它常常只有零点几毫秒, 一位小数看不出"有没有
    // 排队", 而这一点正是判断"卡本身慢还是对接方式慢"的关键。
    if (m_diagDeviceAge) {
        m_diagDeviceAge->setText(deviceAgeUs < 0
            ? QStringLiteral("--")
            : QStringLiteral("%1 ms").arg(deviceAgeUs / 1000.0, 0, 'f', 2));
    }
    auto setMs = [](QLabel* lbl, double ms) {
        if (lbl) lbl->setText(ovFmtMs(ms));
    };
    setMs(m_diagCapToDetect, capToDetectMs);
    setMs(m_diagInfer, inferMs);
    setMs(m_diagPublishToAim, publishToAimMs);
    setMs(m_diagEndToEnd, endToEndMs);
}

void OverviewPage::setSessionState(bool running, const QString& model,
                                   const QString& backend, const QString& uptime) {
    const bool styleChanged = !m_startBtn->property("sessionRunning").isValid()
        || m_running != running;
    m_running = running;
    if (running) {
        m_heroTitle->setText(QString::fromUtf8(u8"推理运行中"));
        QString sub = QString::fromUtf8(u8"%1 · %2").arg(model, backend);
        if (!uptime.isEmpty())
            sub += QString::fromUtf8(u8" · 已运行 %1").arg(uptime);
        m_heroSub->setText(sub);
        if (styleChanged) {
            m_heroChip->setStyleSheet(QStringLiteral("background:#E7F6EC; border-radius:8px; color:#168A50;"));
            m_startBtn->setText(QString::fromUtf8(u8"停止推理"));
            m_startBtn->setProperty("class", "danger");
        }
    } else {
        m_heroTitle->setText(QString::fromUtf8(u8"推理已停止"));
        m_heroSub->setText(QString::fromUtf8(u8"%1 · %2").arg(model, backend));
        if (styleChanged) {
            m_heroChip->setStyleSheet(QStringLiteral("background:#EEF1F5; border-radius:8px; color:#929BAA;"));
            m_startBtn->setText(QString::fromUtf8(u8"启动推理"));
            m_startBtn->setProperty("class", "primary");
        }
    }
    if (styleChanged) {
        m_startBtn->setProperty("sessionRunning", running);
        m_startBtn->style()->unpolish(m_startBtn);
        m_startBtn->style()->polish(m_startBtn);
    }
}
