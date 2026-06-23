#include "MainWindow.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "Apotheosis.h"
#include "app_log.h"
#include "capture/capture.h"
#include "config/config.h"
#include "config/config_bridge.h"
#include "config/ConfigManager.h"
#include "detector/i_detector.h"
#include "runtime/inference_session.h"
#include "widgets/StatusBar.h"
#include "pages/SessionPage.h"
#include "pages/ModelToolsPage.h"
#include "pages/CapturePage.h"
#include "pages/TargetPage.h"
#include "pages/HardwarePage.h"
#include "pages/AiModelPage.h"
#include "pages/DepthPage.h"
#include "pages/HotkeyPage.h"
#include "pages/CrosshairPage.h"
#include "pages/FlashlightPage.h"
#include "pages/GlassFilterPage.h"
#include "pages/MacroPage.h"
#include "pages/StatsPage.h"
#include "pages/LogPage.h"
#include "pages/DebugPage.h"
#include "pages/AutoCapturePage.h"
#include "capture/auto_capture.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Apotheosis");
    resize(960, 640);
    setMinimumSize(720, 480);

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_primaryTabs = new QTabBar(central);
    m_primaryTabs->setObjectName("primaryTabs");
    m_primaryTabs->setDocumentMode(true);
    m_primaryTabs->setExpanding(false);
    m_primaryTabs->setDrawBase(false);
    m_primaryTabs->setTabsClosable(false);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 4, 0);
    topRow->setSpacing(0);
    topRow->addWidget(m_primaryTabs, 1);

    auto* saveBtn = new QPushButton(QStringLiteral("\xf0\x9f\x92\xbe \xe4\xbf\x9d\xe5\xad\x98\xe9\x85\x8d\xe7\xbd\xae"), central);
    saveBtn->setObjectName("saveConfigBtn");
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setFixedHeight(28);
    saveBtn->setStyleSheet(
        "QPushButton { background:#2563EB; color:white; border:none; border-radius:4px; padding:0 12px; font-size:12px; }"
        "QPushButton:hover { background:#1D4ED8; }"
        "QPushButton:pressed { background:#1E40AF; }");
    connect(saveBtn, &QPushButton::clicked, this, [saveBtn] {
        ConfigBridge::instance().syncToRuntime();
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            config.saveConfig();
        }
        saveBtn->setText(QStringLiteral("\xe2\x9c\x93 \xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"));
        QTimer::singleShot(1500, saveBtn, [saveBtn] {
            saveBtn->setText(QStringLiteral("\xf0\x9f\x92\xbe \xe4\xbf\x9d\xe5\xad\x98\xe9\x85\x8d\xe7\xbd\xae"));
        });
    });
    topRow->addWidget(saveBtn);

    m_secondaryTabs = new QTabBar(central);
    m_secondaryTabs->setObjectName("secondaryTabs");
    m_secondaryTabs->setDocumentMode(true);
    m_secondaryTabs->setExpanding(false);
    m_secondaryTabs->setDrawBase(false);

    m_pageStack = new QStackedWidget(central);
    m_statusBar = new StatusBar(central);

    layout->addLayout(topRow);
    layout->addWidget(m_secondaryTabs);
    layout->addWidget(m_pageStack, 1);
    layout->addWidget(m_statusBar);

    setupPages();

    m_statusBar->setInferenceStatus(false);
    m_statusBar->setBackend("TRT");

    connect(m_primaryTabs, &QTabBar::currentChanged,
            this, &MainWindow::onPrimaryTabChanged);
    connect(m_secondaryTabs, &QTabBar::currentChanged,
            this, &MainWindow::onSecondaryTabChanged);

    m_primaryTabs->setCurrentIndex(0);
    onPrimaryTabChanged(0);

    // Monitor pages (性能统计 / 日志 / 调试) are static widgets — without
    // this poll loop their setters never get called, which is why the
    // monitor tab looked frozen at startup defaults.
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(200);
    connect(m_monitorTimer, &QTimer::timeout, this, &MainWindow::pollMonitorTelemetry);
    m_monitorTimer->start();
    pollMonitorTelemetry();
}

void MainWindow::setupPages() {
    const QVector<QPair<QString, QStringList>> tabStructure = {
        {QStringLiteral("会话"), {QStringLiteral("推理启动"), QStringLiteral("模型工具")}},
        {QStringLiteral("配置"), {QStringLiteral("画面采集"), QStringLiteral("目标"), QStringLiteral("硬件"), QStringLiteral("AI 模型"), QStringLiteral("深度模型")}},
        {QStringLiteral("控制"), {QStringLiteral("瞄准热键"), QStringLiteral("准星找色"), QStringLiteral("寻光"), QStringLiteral("玻璃过滤"), QStringLiteral("宏脚本")}},
        {QStringLiteral("监控"), {QStringLiteral("性能统计"), QStringLiteral("日志"), QStringLiteral("自动采集"), QStringLiteral("调试")}},
    };

    int pageIndex = 0;

    for (const auto& [primary, secondaries] : tabStructure) {
        m_primaryTabs->addTab(primary);

        PageRange range;
        range.first = pageIndex;
        range.count = secondaries.size();

        for (const auto& name : secondaries) {
            QWidget* page = createPage(name);
            m_pageStack->addWidget(page);
            ++pageIndex;
        }

        m_tabPages.append(range);
    }

    if (m_hotkeyPage && m_targetPage)
        m_hotkeyPage->setTargetPage(m_targetPage);
}

void MainWindow::selectPage(int primary, int secondary) {
    m_primaryTabs->setCurrentIndex(primary);
    m_secondaryTabs->setCurrentIndex(secondary);
    onSecondaryTabChanged(secondary);
}

void MainWindow::onPrimaryTabChanged(int index) {
    if (index < 0 || index >= m_tabPages.size()) {
        return;
    }

    const QVector<QStringList> secondaryLabels = {
        {QStringLiteral("推理启动"), QStringLiteral("模型工具")},
        {QStringLiteral("画面采集"), QStringLiteral("目标"), QStringLiteral("硬件"), QStringLiteral("AI 模型"), QStringLiteral("深度模型")},
        {QStringLiteral("瞄准热键"), QStringLiteral("准星找色"), QStringLiteral("寻光"), QStringLiteral("玻璃过滤"), QStringLiteral("宏脚本")},
        {QStringLiteral("性能统计"), QStringLiteral("日志"), QStringLiteral("自动采集"), QStringLiteral("调试")},
    };

    QSignalBlocker blocker(m_secondaryTabs);
    while (m_secondaryTabs->count() > 0) {
        m_secondaryTabs->removeTab(0);
    }

    for (const auto& label : secondaryLabels[index]) {
        m_secondaryTabs->addTab(label);
    }

    blocker.unblock();
    m_secondaryTabs->setCurrentIndex(0);
    onSecondaryTabChanged(0);
}

void MainWindow::onSecondaryTabChanged(int index) {
    int primaryIndex = m_primaryTabs->currentIndex();
    if (primaryIndex < 0 || primaryIndex >= m_tabPages.size()) {
        return;
    }
    if (index < 0 || index >= m_tabPages[primaryIndex].count) {
        return;
    }

    m_pageStack->setCurrentIndex(m_tabPages[primaryIndex].first + index);
}

QWidget* MainWindow::createPage(const QString& name) {
    if (name == QStringLiteral("推理启动"))   return new SessionPage();
    if (name == QStringLiteral("模型工具"))   return new ModelToolsPage();
    if (name == QStringLiteral("画面采集"))   return new CapturePage();
    if (name == QStringLiteral("目标"))       { m_targetPage = new TargetPage(); return m_targetPage; }
    if (name == QStringLiteral("硬件"))       return new HardwarePage();
    if (name == QStringLiteral("AI 模型"))    return new AiModelPage();
    if (name == QStringLiteral("深度模型"))   return new DepthPage();
    if (name == QStringLiteral("瞄准热键"))   { m_hotkeyPage = new HotkeyPage(); return m_hotkeyPage; }
    if (name == QStringLiteral("准星找色"))   return new CrosshairPage();
    if (name == QStringLiteral("寻光"))       return new FlashlightPage();
    if (name == QStringLiteral("玻璃过滤"))   return new GlassFilterPage();
    if (name == QStringLiteral("宏脚本"))     return new MacroPage();
    if (name == QStringLiteral("性能统计"))   { m_statsPage = new StatsPage(); return m_statsPage; }
    if (name == QStringLiteral("日志"))       { m_logPage   = new LogPage();   return m_logPage;   }
    if (name == QStringLiteral("自动采集"))   { m_autoCapPage = new AutoCapturePage(); return m_autoCapPage; }
    if (name == QStringLiteral("调试"))       { m_debugPage = new DebugPage(); return m_debugPage; }
    return new QWidget();
}

void MainWindow::pollMonitorTelemetry() {
    // ── Stats ────────────────────────────────────────────────────────────
    if (m_statsPage) {
        const double fps = static_cast<double>(captureFps.load());
        m_statsPage->setFps(fps);
        // 产帧 FPS:eth_capture 等支持的后端通过 GetSourceFpsEstimate 把"receive
        // 线程每秒解码+入队的真实帧数"塞到这里。captureFps(消费侧) < 产帧 FPS
        // 意味着 capture 循环跟不上;两者持平意味着已经吃满产帧速率,瓶颈在产帧。
        m_statsPage->setSourceFps(static_cast<double>(captureSourceFps.load()));
        // 接收诊断:五个互不重叠的桶定位"产帧 FPS 上不去"的源头。
        m_statsPage->setReceiverDiagnostics(
            captureSenderSpanFps.load(),
            captureWireLostFps.load(),
            capturePartialLostFps.load(),
            capturePcapKernelDroppedFps.load(),
            capturePcapIfDroppedFps.load());

        double infer_ms = 0.0;
        double pre_ms   = 0.0;
        double post_ms  = 0.0;
        double copy_ms  = 0.0;
        double nms_ms   = 0.0;
        if (g_inference_session && g_inference_session->detector()) {
            auto* d = g_inference_session->detector();
            pre_ms   = d->lastPreprocessTime().count();
            infer_ms = d->lastInferenceTime().count();
            copy_ms  = d->lastCopyTime().count();
            post_ms  = d->lastPostprocessTime().count();
            nms_ms   = d->lastNmsTime().count();
        }
        // Capture-side latency proxy: 1000 / max(captureFps, 1).
        const double cap_ms = (fps > 0.5) ? (1000.0 / fps) : 0.0;
        m_statsPage->setCaptureLatency(cap_ms);
        m_statsPage->setInferenceLatency(infer_ms);
        m_statsPage->setTotalLatency(pre_ms + infer_ms + copy_ms + post_ms + nms_ms);

        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            m_statsPage->setGpuMemory(QStringLiteral("%1 MB").arg(config.gpuMemoryReserveMB));
            m_statsPage->setCpuCores(QString::number(config.cpuCoreReserveCount));
        }
    }

    // ── Log: drain new tail lines into the textbox ──────────────────────
    if (m_logPage) {
        const auto snap = AppLog::Snapshot();
        const int total = static_cast<int>(snap.size());
        // Snapshot can shrink after a manual Clear — reset cursor if so.
        if (m_logCursor > total) m_logCursor = 0;
        for (int i = m_logCursor; i < total; ++i)
            m_logPage->appendLog(QString::fromUtf8(snap[static_cast<size_t>(i)].c_str()));
        m_logCursor = total;
    }

    // ── Auto capture status ─────────────────────────────────────────────
    if (m_autoCapPage) {
        m_autoCapPage->setForceHeld(AutoCapture::g_force_held.load());
        m_autoCapPage->setSavedCounts(AutoCapture::g_saved_session.load(),
                                      AutoCapture::g_saved_total.load());
    }

    // ── Debug: dynamic FOV readout ──────────────────────────────────────
    if (m_debugPage) {
        const float rx = g_dynamic_fov_radius_x_px.load();
        const float ry = g_dynamic_fov_radius_y_px.load();
        if (rx > 0.0f && ry > 0.0f) {
            m_debugPage->setFovReadout(
                QStringLiteral("X = %1 px,  Y = %2 px  (有效半径)")
                    .arg(rx, 0, 'f', 1).arg(ry, 0, 'f', 1));
        } else {
            m_debugPage->setFovReadout(
                QStringLiteral("未启用 / 无锁定目标。在「瞄准热键」面板的「动态 FOV」子段中开启。"));
        }
    }
}
