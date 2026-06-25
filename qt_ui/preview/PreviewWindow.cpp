#include "preview/PreviewWindow.h"

#include "pages/OverviewPage.h"
#include "pages/LinksPage.h"
#include "widgets/IconFont.h"
#include "widgets/SideNav.h"
#include "widgets/TopNavBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

struct GroupDef {
    QString name;
    QStringList subs;
    QStringList icons;
};

const QVector<GroupDef>& groups() {
    static const QVector<GroupDef> kGroups = {
        {QStringLiteral("概览"), {}, {}},
        {QStringLiteral("会话"),
         {QStringLiteral("推理启动"), QStringLiteral("模型工具")},
         {QStringLiteral("player-play"), QStringLiteral("settings")}},
        {QStringLiteral("配置"),
         {QStringLiteral("画面采集"), QStringLiteral("目标"), QStringLiteral("硬件"),
          QStringLiteral("AI 模型"), QStringLiteral("深度模型")},
         {QStringLiteral("device-desktop"), QStringLiteral("target"), QStringLiteral("plug"),
          QStringLiteral("cpu"), QStringLiteral("stack-2")}},
        {QStringLiteral("控制"),
         {QStringLiteral("瞄准热键"), QStringLiteral("准星找色"), QStringLiteral("寻光"),
          QStringLiteral("玻璃过滤"), QStringLiteral("宏脚本")},
         {QStringLiteral("keyboard"), QStringLiteral("color-swatch"), QStringLiteral("world"),
          QStringLiteral("layers-intersect"), QStringLiteral("terminal-2")}},
        {QStringLiteral("监控"),
         {QStringLiteral("性能统计"), QStringLiteral("日志"), QStringLiteral("自动采集"),
          QStringLiteral("调试")},
         {QStringLiteral("gauge"), QStringLiteral("terminal-2"), QStringLiteral("camera"),
          QStringLiteral("bug")}},
        // 「找母狗」:无二级导航,落地页是一排快速跳转按钮(LinksPage)。
        {QStringLiteral("找母狗"), {}, {}},
    };
    return kGroups;
}

}  // namespace

PreviewWindow::PreviewWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Apotheosis — UI 预览"));
    resize(1080, 720);
    setMinimumSize(940, 600);

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    m_top = new TopNavBar(central);

    m_side = new SideNav(central);
    m_stack = new QStackedWidget(central);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_side);
    body->addWidget(m_stack, 1);

    col->addWidget(m_top);
    col->addLayout(body, 1);

    buildPages();

    QStringList primaryLabels;
    for (const auto& g : groups())
        primaryLabels << g.name;
    m_top->setPrimaryItems(primaryLabels);

    connect(m_top, &TopNavBar::primaryChanged, this, &PreviewWindow::onPrimaryChanged);
    connect(m_side, &SideNav::currentChanged, this, &PreviewWindow::onSecondaryChanged);
    connect(m_top, &TopNavBar::saveClicked, this, [] { /* 预览中保存为空操作 */ });
    connect(m_overview, &OverviewPage::startStopRequested, this, &PreviewWindow::toggleRunning);

    m_top->setCurrentPrimary(0);
    onPrimaryChanged(0);

    // Pre-seed the chart so it looks populated the moment the window opens.
    for (int i = 0; i < 64; ++i)
        tickMock();

    m_timer = new QTimer(this);
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &PreviewWindow::tickMock);
    m_timer->start();
}

void PreviewWindow::buildPages() {
    int index = 0;
    for (const auto& g : groups()) {
        Range r;
        r.first = index;
        r.name = g.name;
        r.subs = g.subs;
        r.icons = g.icons;
        if (g.subs.isEmpty()) {
            if (g.name == QStringLiteral("找母狗")) {
                m_stack->addWidget(new LinksPage);
            } else {
                m_overview = new OverviewPage;
                m_stack->addWidget(m_overview);
            }
            ++index;
        } else {
            for (int s = 0; s < g.subs.size(); ++s) {
                m_stack->addWidget(makePlaceholder(g.subs[s], g.icons.value(s)));
                ++index;
            }
        }
        m_ranges.append(r);
    }
}

QWidget* PreviewWindow::makePlaceholder(const QString& title, const QString& iconName) {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(16, 16, 16, 16);
    col->addStretch();

    auto* box = new QVBoxLayout;
    box->setAlignment(Qt::AlignCenter);
    box->setSpacing(10);

    if (IconFont::available() && !iconName.isEmpty()) {
        auto* icon = new QLabel;
        icon->setAlignment(Qt::AlignCenter);
        icon->setFont(IconFont::font(34));
        icon->setText(QString(IconFont::glyph(iconName)));
        icon->setStyleSheet(QStringLiteral("color:#C7C7CF;"));
        box->addWidget(icon);
    }

    auto* name = new QLabel(title);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(QStringLiteral("font-size:16px; font-weight:500; color:#52525B;"));
    box->addWidget(name);

    auto* hint = new QLabel(QStringLiteral("「%1」的真实内容将在完整版接入 —— 当前为新外壳预览").arg(title));
    hint->setAlignment(Qt::AlignCenter);
    hint->setProperty("class", "tertiary");
    box->addWidget(hint);

    col->addLayout(box);
    col->addStretch();
    return page;
}

void PreviewWindow::selectPrimary(int index) {
    m_top->setCurrentPrimary(index);
    onPrimaryChanged(index);
}

void PreviewWindow::onPrimaryChanged(int index) {
    if (index < 0 || index >= m_ranges.size())
        return;
    const Range& r = m_ranges[index];
    if (r.subs.isEmpty()) {
        m_side->hide();
    } else {
        m_side->setItems(r.name, r.subs, r.icons);
        m_side->show();
    }
    m_stack->setCurrentIndex(r.first);
}

void PreviewWindow::onSecondaryChanged(int index) {
    const int primary = m_top->currentPrimary();
    if (primary < 0 || primary >= m_ranges.size())
        return;
    const Range& r = m_ranges[primary];
    if (index < 0 || index >= r.subs.size())
        return;
    m_stack->setCurrentIndex(r.first + index);
}

void PreviewWindow::toggleRunning() {
    m_running = !m_running;
}

void PreviewWindow::tickMock() {
    ++m_tick;
    const double t = m_tick;

    if (m_running) {
        const double fps = 235.0 + 6.0 * std::sin(t * 0.30) + 3.0 * std::sin(t * 0.11);
        const double infer = 3.9 + 0.45 * std::sin(t * 0.22);
        const double total = 6.4 + 0.55 * std::sin(t * 0.17);
        const int boxes = 18 + static_cast<int>(std::round(5.0 * std::sin(t * 0.25)));
        const long secs = 754 + m_tick / 4;

        m_overview->setFps(fps);
        m_overview->setSourceFps(240.0);
        m_overview->setInferenceLatency(infer);
        m_overview->setTotalLatency(total);
        m_overview->setDetectionCount(boxes, boxes > 6 ? boxes - 6 : 0);
        m_overview->setReceiverDiagnostics(0, 0, 2, 0, 0);

        const QString uptime = QStringLiteral("%1:%2:%3")
            .arg(secs / 3600, 2, 10, QChar('0'))
            .arg((secs % 3600) / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0'));
        m_overview->setSessionState(true, QStringLiteral("yolov8n.engine"),
                                    QStringLiteral("TensorRT (CUDA)"), uptime);
    } else {
        m_overview->setSessionState(false, QStringLiteral("yolov8n.engine"),
                                    QStringLiteral("TensorRT (CUDA)"), QString());
    }

    m_top->setSessionStatus(m_running, m_running ? QStringLiteral("运行中") : QStringLiteral("已停止"));
}
