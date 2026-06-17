#include "pages/DebugPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

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

    // ── Card 1: 截图 ──
    auto* screenshotCard = new CardWidget(QStringLiteral("截图"), QStringLiteral("camera"));

    m_screenshotKey = new QLineEdit;
    m_screenshotKey->setReadOnly(true);
    m_screenshotKey->setPlaceholderText(QStringLiteral("点击此处绑定按键"));
    screenshotCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("截图键"), m_screenshotKey));

    screenshotCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("截图延迟"), 0, 5000, 500,
                           m_screenshotDelaySlider, m_screenshotDelay,
                           QStringLiteral(" ms")));

    layout->addWidget(screenshotCard);

    // ── Card 2: 轨迹回放 (collapsible) ──
    auto* replayCard = new CardWidget(QStringLiteral("轨迹回放"), QStringLiteral("history"));
    replayCard->setCollapsible(true);

    replayCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("启用录制"), false, m_enableRecording));

    replayCard->contentLayout()->addWidget(
        FormKit::sliderRow(QStringLiteral("回放时长"), 1, 60, 10,
                           m_replayDurationSlider, m_replayDuration,
                           QStringLiteral(" 秒")));

    replayCard->contentLayout()->addWidget(
        FormKit::sliderRowD(QStringLiteral("回放速度"), 0.1, 2.0, 0.25, 0.05, 2,
                            m_replaySpeedSlider, m_replaySpeed,
                            QStringLiteral("x")));

    layout->addWidget(replayCard);

    // ── Card 3: 诊断 ──
    auto* diagCard = new CardWidget(QStringLiteral("诊断"), QStringLiteral("bug"));

    diagCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("详细日志"), false, m_verboseLog));

    diagCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示 FPS"), true, m_showFps));

    diagCard->contentLayout()->addWidget(
        FormKit::toggleRow(QStringLiteral("显示窗口"), true, m_showWindow));

    layout->addWidget(diagCard);
    layout->addStretch();
}
