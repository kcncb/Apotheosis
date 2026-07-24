#include "MainWindow.h"

#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPropertyAnimation>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <mutex>

#include "Apotheosis.h"
#include "app_log.h"
#include "capture/capture.h"
#include "config/config.h"
#include "config/config_bridge.h"
#include "config/ConfigManager.h"
#include "detector/i_detector.h"
#include "runtime/inference_session.h"
#include "widgets/StatusBar.h"
#include "widgets/TopNavBar.h"
#include "widgets/SideNav.h"
#include "pages/OverviewPage.h"
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
#include "pages/EventPage.h"
#include "capture/auto_capture.h"

namespace {

struct GroupDef {
    QString name;
    QStringList subs;
    QStringList icons;
};

// 单一导航数据源:一级分组(顶栏)+ 二级子页(侧边栏)+ 子页图标。
const QVector<GroupDef>& navGroups() {
    static const QVector<GroupDef> kGroups = {
        {QString::fromUtf8(u8"概览"), {}, {}},
        {QString::fromUtf8(u8"会话"),
         {QString::fromUtf8(u8"推理启动"), QString::fromUtf8(u8"模型工具")},
         {QStringLiteral("player-play"), QStringLiteral("settings")}},
        {QString::fromUtf8(u8"配置"),
         {QString::fromUtf8(u8"画面采集"), QString::fromUtf8(u8"目标"), QString::fromUtf8(u8"硬件"),
          QString::fromUtf8(u8"AI 模型"), QString::fromUtf8(u8"深度模型")},
         {QStringLiteral("device-desktop"), QStringLiteral("target"), QStringLiteral("plug"),
          QStringLiteral("cpu"), QStringLiteral("stack-2")}},
        {QString::fromUtf8(u8"控制"),
         {QString::fromUtf8(u8"瞄准热键"), QString::fromUtf8(u8"准星找色"), QString::fromUtf8(u8"寻光"),
           QString::fromUtf8(u8"玻璃过滤"), QString::fromUtf8(u8"宏脚本"), QString::fromUtf8(u8"事件编排")},
         {QStringLiteral("keyboard"), QStringLiteral("color-swatch"), QStringLiteral("world"),
           QStringLiteral("layers-intersect"), QStringLiteral("terminal-2"), QStringLiteral("history")}},
        {QString::fromUtf8(u8"监控"),
         {QString::fromUtf8(u8"性能统计"), QString::fromUtf8(u8"日志"), QString::fromUtf8(u8"自动采集"),
          QString::fromUtf8(u8"调试")},
         {QStringLiteral("gauge"), QStringLiteral("terminal-2"), QStringLiteral("camera"),
          QStringLiteral("bug")}},
    };
    return kGroups;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Apotheosis");
    resize(1080, 720);
    setMinimumSize(940, 600);

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_topNav = new TopNavBar(central);
    m_sideNav = new SideNav(central);
    m_pageStack = new QStackedWidget(central);
    m_statusBar = new StatusBar(central);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_sideNav);

    auto* pageShell = new QWidget(central);
    pageShell->setObjectName("pageShell");
    pageShell->setAttribute(Qt::WA_StyledBackground, true);
    auto* pageShellLayout = new QVBoxLayout(pageShell);
    pageShellLayout->setContentsMargins(0, 0, 0, 0);
    pageShellLayout->setSpacing(0);

    auto* contextHeader = new QWidget(pageShell);
    contextHeader->setObjectName("contextHeader");
    contextHeader->setAttribute(Qt::WA_StyledBackground, true);
    contextHeader->setFixedHeight(62);
    auto* contextLayout = new QVBoxLayout(contextHeader);
    contextLayout->setContentsMargins(20, 10, 20, 9);
    contextLayout->setSpacing(1);
    m_contextTitle = new QLabel(contextHeader);
    m_contextTitle->setObjectName("contextTitle");
    m_contextPath = new QLabel(contextHeader);
    m_contextPath->setObjectName("contextPath");
    contextLayout->addWidget(m_contextTitle);
    contextLayout->addWidget(m_contextPath);

    pageShellLayout->addWidget(contextHeader);
    pageShellLayout->addWidget(m_pageStack, 1);
    body->addWidget(pageShell, 1);

    layout->addWidget(m_topNav);
    layout->addLayout(body, 1);
    layout->addWidget(m_statusBar);

    setupPages();

    m_statusBar->setInferenceStatus(false);
    m_statusBar->setBackend("TRT");

    connect(m_topNav, &TopNavBar::primaryChanged, this, &MainWindow::onPrimaryChanged);
    connect(m_sideNav, &SideNav::currentChanged, this, &MainWindow::onSecondaryChanged);
    connect(m_topNav, &TopNavBar::saveClicked, this, &MainWindow::onSaveRequested);

    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &MainWindow::onSaveRequested);

    m_topNav->setCurrentPrimary(0);
    onPrimaryChanged(0);

    // Monitor pages (概览 / 性能统计 / 日志 / 调试) are static widgets — without
    // this poll loop their setters never get called.
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(200);
    connect(m_monitorTimer, &QTimer::timeout, this, &MainWindow::pollMonitorTelemetry);
    m_monitorTimer->start();
    pollMonitorTelemetry();
}

void MainWindow::setupPages() {
    int pageIndex = 0;
    QStringList primaryNames;

    for (const auto& g : navGroups()) {
        primaryNames << g.name;

        PageRange range;
        range.first = pageIndex;
        range.name = g.name;
        range.subs = g.subs;
        range.icons = g.icons;

        if (g.subs.isEmpty()) {
            // 无二级导航的一级分组:整组只有一个落地页。
            m_overviewPage = new OverviewPage();
            connect(m_overviewPage, &OverviewPage::startStopRequested,
                    this, &MainWindow::onHeroToggleInference);
            connect(m_overviewPage, &OverviewPage::previewRequested,
                    this, [] { ConfigManager::instance().setShowWindow(true); });
            m_pageStack->addWidget(m_overviewPage);
            range.count = 1;
            ++pageIndex;
        } else {
            range.count = g.subs.size();
            for (const auto& name : g.subs) {
                QWidget* page = createPage(name);
                m_pageStack->addWidget(page);
                ++pageIndex;
            }
        }

        m_tabPages.append(range);
    }

    m_topNav->setPrimaryItems(primaryNames);

    if (m_hotkeyPage && m_targetPage)
        m_hotkeyPage->setTargetPage(m_targetPage);
}

void MainWindow::selectPage(int primary, int secondary) {
    if (primary < 0 || primary >= m_tabPages.size())
        return;
    m_topNav->setCurrentPrimary(primary);
    onPrimaryChanged(primary);
    if (secondary > 0 && secondary < m_tabPages[primary].count) {
        m_sideNav->setCurrentIndex(secondary);
        onSecondaryChanged(secondary);
    }
}

void MainWindow::onPrimaryChanged(int index) {
    if (index < 0 || index >= m_tabPages.size())
        return;

    const PageRange& r = m_tabPages[index];
    if (r.subs.isEmpty()) {
        m_sideNav->hide();
    } else {
        m_sideNav->setItems(r.name, r.subs, r.icons);
        m_sideNav->show();
    }
    updateContextHeader(index, 0);
    switchPage(r.first);
}

void MainWindow::onSecondaryChanged(int index) {
    const int primaryIndex = m_topNav->currentPrimary();
    if (primaryIndex < 0 || primaryIndex >= m_tabPages.size())
        return;
    if (index < 0 || index >= m_tabPages[primaryIndex].count)
        return;

    updateContextHeader(primaryIndex, index);
    switchPage(m_tabPages[primaryIndex].first + index);
}

void MainWindow::switchPage(int index) {
    if (index < 0 || index >= m_pageStack->count())
        return;
    if (m_pageStack->currentIndex() == index)
        return;

    if (m_pageAnimation) {
        m_pageAnimation->stop();
        m_pageAnimation->deleteLater();
        m_pageAnimation = nullptr;
    }

    if (auto* previous = m_pageStack->currentWidget())
        previous->setGraphicsEffect(nullptr);

    m_pageStack->setCurrentIndex(index);
    QWidget* page = m_pageStack->currentWidget();
    const bool reduceMotion = qEnvironmentVariableIntValue("APOTHEOSIS_REDUCE_MOTION") != 0;
    if (!page || !isVisible() || reduceMotion)
        return;

    // 仅在切页的短时间内启用透明度特效，结束后立即移除，避免持续合成开销。
    auto* effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(0.72);
    page->setGraphicsEffect(effect);

    auto* animation = new QPropertyAnimation(effect, "opacity", this);
    m_pageAnimation = animation;
    animation->setDuration(155);
    animation->setStartValue(0.72);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, page, animation] {
        if (m_pageAnimation != animation)
            return;
        m_pageAnimation = nullptr;
        page->setGraphicsEffect(nullptr);
        animation->deleteLater();
    });
    animation->start();
}

void MainWindow::updateContextHeader(int primary, int secondary) {
    if (primary < 0 || primary >= m_tabPages.size())
        return;

    const PageRange& range = m_tabPages[primary];
    const QString pageName = range.subs.isEmpty() ? range.name : range.subs.value(secondary, range.name);
    m_contextTitle->setText(pageName);

    static const QStringList descriptions = {
        QString::fromUtf8(u8"运行状态与关键性能一览"),
        QString::fromUtf8(u8"推理会话与模型运行管理"),
        QString::fromUtf8(u8"采集、模型与硬件参数"),
        QString::fromUtf8(u8"瞄准逻辑与自动化控制"),
        QString::fromUtf8(u8"运行数据、日志与诊断"),
    };
    const QString description = descriptions.value(primary);
    if (range.subs.isEmpty()) {
        m_contextPath->setText(description);
    } else {
        m_contextPath->setText(QString::fromUtf8(u8"%1  /  %2 · %3")
                                   .arg(range.name, pageName, description));
    }
}

void MainWindow::onSaveRequested() {
    ConfigBridge::instance().syncToRuntime();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.saveConfig();
    }
    m_topNav->showSaveFeedback();
}

void MainWindow::onHeroToggleInference() {
    if (!g_inference_session)
        return;

    if (g_inference_session->running()) {
        g_inference_session->stop();
    } else {
        ConfigBridge::instance().syncToRuntime();
        std::string backend;
        std::string modelPath;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            backend = config.backend;
            modelPath = "models/" + config.ai_model;
        }
        g_inference_session->start(backend, modelPath);
    }
    // Display refreshes on the next pollMonitorTelemetry() tick.
}

QWidget* MainWindow::createPage(const QString& name) {
    if (name == QString::fromUtf8(u8"推理启动"))   return new SessionPage();
    if (name == QString::fromUtf8(u8"模型工具"))   return new ModelToolsPage();
    if (name == QString::fromUtf8(u8"画面采集"))   return new CapturePage();
    if (name == QString::fromUtf8(u8"目标"))       { m_targetPage = new TargetPage(); return m_targetPage; }
    if (name == QString::fromUtf8(u8"硬件"))       return new HardwarePage();
    if (name == QString::fromUtf8(u8"AI 模型"))    return new AiModelPage();
    if (name == QString::fromUtf8(u8"深度模型"))   return new DepthPage();
    if (name == QString::fromUtf8(u8"瞄准热键"))   { m_hotkeyPage = new HotkeyPage(); return m_hotkeyPage; }
    if (name == QString::fromUtf8(u8"准星找色"))   return new CrosshairPage();
    if (name == QString::fromUtf8(u8"寻光"))       return new FlashlightPage();
    if (name == QString::fromUtf8(u8"玻璃过滤"))   return new GlassFilterPage();
    if (name == QString::fromUtf8(u8"宏脚本"))     return new MacroPage();
    if (name == QString::fromUtf8(u8"事件编排"))   return new EventPage();
    if (name == QString::fromUtf8(u8"性能统计"))   { m_statsPage = new StatsPage(); return m_statsPage; }
    if (name == QString::fromUtf8(u8"日志"))       { m_logPage   = new LogPage();   return m_logPage;   }
    if (name == QString::fromUtf8(u8"自动采集"))   { m_autoCapPage = new AutoCapturePage(); return m_autoCapPage; }
    if (name == QString::fromUtf8(u8"调试"))       { m_debugPage = new DebugPage(); return m_debugPage; }
    return new QWidget();
}

void MainWindow::pollMonitorTelemetry() {
    // ── Shared telemetry (computed once, fed to both 概览 and 性能统计) ──────
    const double fps = static_cast<double>(captureFps.load());
    const double sourceFps = static_cast<double>(captureSourceFps.load());
    const int rxSender = captureSenderSpanFps.load();
    const int rxWire = captureWireLostFps.load();
    const int rxPartial = capturePartialLostFps.load();
    const int rxKernel = capturePcapKernelDroppedFps.load();
    const int rxIf = capturePcapIfDroppedFps.load();

    double pre_ms = 0.0, infer_ms = 0.0, copy_ms = 0.0, post_ms = 0.0, nms_ms = 0.0;
    if (g_inference_session && g_inference_session->detector()) {
        auto* d = g_inference_session->detector();
        pre_ms   = d->lastPreprocessTime().count();
        infer_ms = d->lastInferenceTime().count();
        copy_ms  = d->lastCopyTime().count();
        post_ms  = d->lastPostprocessTime().count();
        nms_ms   = d->lastNmsTime().count();
    }
    const double total_ms = pre_ms + infer_ms + copy_ms + post_ms + nms_ms;
    const double cap_ms = (fps > 0.5) ? (1000.0 / fps) : 0.0;
    const bool running = g_inference_session && g_inference_session->running();

    // Session uptime: stamp on the false→true edge.
    if (running && !m_sessionRunning)
        m_sessionStart = std::chrono::steady_clock::now();
    m_sessionRunning = running;

    int gpuMb = 0, cpuCores = 0;
    QString model, backendRaw;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        gpuMb = config.gpuMemoryReserveMB;
        cpuCores = config.cpuCoreReserveCount;
        model = QString::fromStdString(config.ai_model);
        backendRaw = QString::fromStdString(config.backend);
    }
    const QString backendDisp = (backendRaw == QStringLiteral("DML"))
        ? QStringLiteral("DirectML")
        : QStringLiteral("TensorRT (CUDA)");

    m_statusBar->setInferenceStatus(running);
    m_statusBar->setFps(fps);
    m_topNav->setSessionStatus(running, running ? QString::fromUtf8(u8"运行中") : QString::fromUtf8(u8"已停止"));

    // ── 概览 dashboard ──────────────────────────────────────────────────
    if (m_overviewPage) {
        m_overviewPage->setFps(fps);
        m_overviewPage->setSourceFps(sourceFps);
        m_overviewPage->setReceiverDiagnostics(rxSender, rxWire, rxPartial, rxKernel, rxIf);
        m_overviewPage->setInferenceLatency(infer_ms);
        m_overviewPage->setTotalLatency(total_ms);
        m_overviewPage->setDetectionCount(static_cast<int>(detectionBuffer.boxes.size()), -1);

        QString uptime;
        if (running) {
            const long s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_sessionStart).count());
            uptime = QStringLiteral("%1:%2:%3")
                .arg(s / 3600, 2, 10, QChar('0'))
                .arg((s % 3600) / 60, 2, 10, QChar('0'))
                .arg(s % 60, 2, 10, QChar('0'));
        }
        m_overviewPage->setSessionState(
            running,
            model.isEmpty() ? QString::fromUtf8(u8"(未选择模型)") : model,
            backendDisp, uptime);
    }

    // ── 性能统计 ────────────────────────────────────────────────────────
    if (m_statsPage) {
        m_statsPage->setFps(fps);
        m_statsPage->setSourceFps(sourceFps);
        m_statsPage->setReceiverDiagnostics(rxSender, rxWire, rxPartial, rxKernel, rxIf);
        m_statsPage->setCaptureLatency(cap_ms);
        m_statsPage->setInferenceLatency(infer_ms);
        m_statsPage->setTotalLatency(total_ms);
        m_statsPage->setGpuMemory(QStringLiteral("%1 MB").arg(gpuMb));
        m_statsPage->setCpuCores(QString::number(cpuCores));
    }

    // ── Log: drain new tail lines into the textbox ──────────────────────
    if (m_logPage) {
        const auto snap = AppLog::Snapshot();
        const int total = static_cast<int>(snap.size());
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
                QString::fromUtf8(u8"X = %1 px,  Y = %2 px  (有效半径)")
                    .arg(rx, 0, 'f', 1).arg(ry, 0, 'f', 1));
        } else {
            m_debugPage->setFovReadout(
                QString::fromUtf8(u8"未启用 / 无锁定目标。在「瞄准热键」面板的「动态 FOV」子段中开启。"));
        }
    }
}
