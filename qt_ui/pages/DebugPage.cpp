#include "pages/DebugPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

// ────────────────────────────────────────────────────────────────────────────
// Construction
// ────────────────────────────────────────────────────────────────────────────

DebugPage::DebugPage(QWidget* parent)
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

    buildScreenshotCard(layout);
    buildReplayCard(layout);
    buildDiagCard(layout);
    buildDynamicFovCard(layout);

    layout->addStretch();

    // Load initial values from config and connect reload signal.
    auto& cfg = ConfigManager::instance();
    connect(&cfg, &ConfigManager::configLoaded, this, &DebugPage::onLoadConfig);
    onLoadConfig();
}

// ────────────────────────────────────────────────────────────────────────────
// Card 1 : Screenshot
// ────────────────────────────────────────────────────────────────────────────

void DebugPage::buildScreenshotCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(QStringLiteral("截图"), QStringLiteral("camera"));

    // ── Screenshot key binding ──
    m_screenshotKey = new QLineEdit;
    m_screenshotKey->setReadOnly(true);
    m_screenshotKey->setPlaceholderText(QStringLiteral("点击此处绑定按键"));
    m_screenshotKey->setToolTip(tr(
        "按下任一绑定按键就把当前画面+检测框保存到 screenshots/ 目录。\n"
        "用 +/- 增删，可以同时绑定多个键。"));
    card->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("截图键"), m_screenshotKey));

    // ── Screenshot delay ──
    card->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("截图延迟"), 0, 5000, 100,
                           m_screenshotDelaySlider, m_screenshotDelay,
                           QStringLiteral(" ms")));
    m_screenshotDelay->setToolTip(tr(
        "按下截图键后等待多少毫秒再实际抓帧，用来避开自己手部抖动等瞬间。"));
    m_screenshotDelay->setSingleStep(50);

    connect(m_screenshotDelay, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setScreenshotDelay(v);
            });

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 2 : Replay (collapsible)
// ────────────────────────────────────────────────────────────────────────────

void DebugPage::buildReplayCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(QStringLiteral("瞄准轨迹回放"), QStringLiteral("history"));
    card->setCollapsible(true);

    // ── Enable recording ──
    card->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用环形录制"), false, m_enableRecording));
    m_enableRecording->setToolTip(tr(
        "开启后持续把每帧的检测框、锁定 ID、锚点写入环形缓冲；\n"
        "用于事后慢放复盘'刚才那一枪为什么过了头'。关闭时无任何分配。"));

    connect(m_enableRecording, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setReplayRecordEnabled(v);
            });

    // ── Replay duration (seconds) ──
    card->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("保留秒数"), 1, 60, 10,
                           m_replayDurationSlider, m_replayDuration,
                           QStringLiteral(" 秒")));
    m_replayDuration->setToolTip(tr(
        "环形缓冲保留多少秒的近况。值大占内存稍多。"));

    connect(m_replayDuration, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) {
                ConfigManager::instance().setReplaySeconds(v);
            });

    // ── Playback speed ──
    card->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("回放速度"), 0.05, 2.0, 0.25, 0.05, 2,
                            m_replaySpeedSlider, m_replaySpeed,
                            QStringLiteral("x")));
    m_replaySpeed->setToolTip(tr(
        "慢放速度。0.25x = 1/4 速，适合细看一两枪；1x = 原速；>1x 跳过看大势。"));

    connect(m_replaySpeed, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) {
                ConfigManager::instance().setReplayPlaybackSpeed(static_cast<float>(v));
            });

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 3 : Diagnostics
// ────────────────────────────────────────────────────────────────────────────

void DebugPage::buildDiagCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(QStringLiteral("诊断"), QStringLiteral("bug"));

    // ── Verbose logging ──
    card->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("控制台输出详细日志"), false, m_verboseLog));
    m_verboseLog->setToolTip(tr(
        "开启后会向控制台打印每帧检测/锁定/瞄准计算的细节，用于排查问题；\n"
        "性能会受影响，正常使用建议关闭。"));

    connect(m_verboseLog, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setVerbose(v);
            });

    // ── Show FPS ──
    card->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示 FPS"), false, m_showFps));

    connect(m_showFps, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setShowFps(v);
            });

    // ── Show window ──
    card->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示窗口"), false, m_showWindow));

    connect(m_showWindow, &ToggleSwitch::toggled,
            this, [](bool v) {
                ConfigManager::instance().setShowWindow(v);
            });

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// Card 4 : Dynamic FOV (read-only telemetry)
// ────────────────────────────────────────────────────────────────────────────

void DebugPage::buildDynamicFovCard(QVBoxLayout* layout) {
    auto* card = new CardWidget(QStringLiteral("动态 FOV (实时)"), QStringLiteral("target"));
    card->setCollapsible(true);

    m_fovReadout = new QLabel(
        QStringLiteral("未启用 / 无锁定目标。在「瞄准热键」面板的「动态 FOV」子段中开启。"));
    m_fovReadout->setWordWrap(true);
    m_fovReadout->setToolTip(tr(
        "瞄准时按目标距准星距离与 bbox 大小动态收缩的有效 FOV 直径（像素）。\n"
        "此处仅做只读显示，开关和参数在「瞄准热键」页面的动态 FOV 子段。"));
    card->contentLayout()->addWidget(m_fovReadout);

    layout->addWidget(card);
}

// ────────────────────────────────────────────────────────────────────────────
// Config load
// ────────────────────────────────────────────────────────────────────────────

void DebugPage::setFovReadout(const QString& text) {
    if (m_fovReadout) m_fovReadout->setText(text);
}

void DebugPage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    // Screenshot
    m_screenshotDelay->setValue(cfg.screenshotDelay());

    // Replay
    m_enableRecording->setChecked(cfg.replayRecordEnabled());
    m_replayDuration->setValue(cfg.replaySeconds());
    m_replaySpeed->setValue(static_cast<double>(cfg.replayPlaybackSpeed()));

    // Diagnostics
    m_verboseLog->setChecked(cfg.verbose());
    m_showFps->setChecked(cfg.showFps());
    m_showWindow->setChecked(cfg.showWindow());
}
