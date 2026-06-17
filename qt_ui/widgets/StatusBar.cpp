#include "widgets/StatusBar.h"

#include <QHBoxLayout>
#include <QLabel>

StatusBar::StatusBar(QWidget* parent)
    : QWidget(parent) {
    setObjectName("statusBar");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(36);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 4, 16, 4);
    layout->setSpacing(12);

    m_statusDot = new QLabel(this);
    m_statusDot->setFixedSize(8, 8);
    m_statusDot->setStyleSheet(
        "background-color: #C7C7CC; border-radius: 4px;");

    m_statusLabel = new QLabel(QStringLiteral("已停止"), this);
    m_statusLabel->setProperty("class", "secondary");

    m_fpsLabel = new QLabel(QStringLiteral("FPS: --"), this);
    m_fpsLabel->setProperty("class", "secondary");

    m_backendLabel = new QLabel(QStringLiteral("后端: --"), this);
    m_backendLabel->setProperty("class", "secondary");

    layout->addWidget(m_statusDot);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_fpsLabel);
    layout->addWidget(m_backendLabel);
    layout->addStretch();

    m_configPathLabel = new QLabel(this);
    m_configPathLabel->setProperty("class", "secondary");
    layout->addWidget(m_configPathLabel);
}

void StatusBar::setInferenceStatus(bool running) {
    if (running) {
        m_statusDot->setStyleSheet(
            "background-color: #34C759; border-radius: 4px;");
        m_statusLabel->setText(QStringLiteral("运行中"));
    } else {
        m_statusDot->setStyleSheet(
            "background-color: #C7C7CC; border-radius: 4px;");
        m_statusLabel->setText(QStringLiteral("已停止"));
    }
}

void StatusBar::setFps(double fps) {
    m_fpsLabel->setText(QStringLiteral("FPS: %1").arg(fps, 0, 'f', 1));
}

void StatusBar::setBackend(const QString& backend) {
    m_backendLabel->setText(QStringLiteral("后端: %1").arg(backend));
}

void StatusBar::setConfigPath(const QString& path) {
    m_configPathLabel->setText(path);
}
