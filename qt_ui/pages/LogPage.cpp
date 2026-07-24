#include "pages/LogPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "app_log.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>

LogPage::LogPage(QWidget* parent)
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

    // ── Card 1: 应用日志 ──
    auto* logCard = new CardWidget(QStringLiteral("应用日志"), QStringLiteral("terminal-2"));

    m_levelCombo = new QComboBox;
    m_levelCombo->addItems({
        QStringLiteral("全部"),
        QStringLiteral("信息"),
        QStringLiteral("警告"),
        QStringLiteral("错误"),
    });
    logCard->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("级别"), m_levelCombo));

    m_logText = new QTextEdit;
    m_logText->setReadOnly(true);
    m_logText->setMinimumHeight(360);
    QFont mono(QStringLiteral("Consolas, Courier New, monospace"));
    mono.setStyleHint(QFont::Monospace);
    m_logText->setFont(mono);
    logCard->contentLayout()->addWidget(m_logText, 1);

    auto* btnRow = new QHBoxLayout;
    m_clearBtn = new QPushButton(QStringLiteral("清空日志"));
    m_copyBtn = new QPushButton(QStringLiteral("复制到剪贴板"));
    m_exportBtn = new QPushButton(QStringLiteral("导出..."));
    btnRow->addWidget(m_clearBtn);
    btnRow->addWidget(m_copyBtn);
    btnRow->addWidget(m_exportBtn);
    btnRow->addStretch();
    logCard->contentLayout()->addLayout(btnRow);

    layout->addWidget(logCard);
    layout->addStretch();

    connect(m_clearBtn, &QPushButton::clicked, this, &LogPage::clearLog);
    connect(m_copyBtn, &QPushButton::clicked, this, &LogPage::copyToClipboard);
    connect(m_exportBtn, &QPushButton::clicked, this, &LogPage::exportLog);
    connect(m_levelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &LogPage::onLevelChanged);
}

void LogPage::appendLog(const QString& line) {
    m_allLines.push_back(line);
    if (!acceptsLine(line))
        return;
    m_logText->append(line.toHtmlEscaped());
    auto* bar = m_logText->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void LogPage::clearLog() {
    AppLog::Clear();
    m_allLines.clear();
    m_logText->clear();
}

void LogPage::copyToClipboard() {
    QApplication::clipboard()->setText(m_logText->toPlainText());
}

void LogPage::exportLog() {
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出日志"), QString(), QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(m_logText->toPlainText().toUtf8());
    }
}

void LogPage::onLevelChanged(int) {
    rebuildView();
}

bool LogPage::acceptsLine(const QString& line) const {
    const int selected = m_levelCombo->currentIndex();
    if (selected <= 0)
        return true;
    const QString lower = line.toLower();
    const bool isError = lower.contains(QStringLiteral("[错误]"))
        || lower.contains(QStringLiteral("error"))
        || lower.contains(QStringLiteral("failed"))
        || lower.contains(QStringLiteral("失败"));
    const bool isWarning = !isError && (
        lower.contains(QStringLiteral("warning"))
        || lower.contains(QStringLiteral("warn"))
        || lower.contains(QStringLiteral("警告"))
        || lower.contains(QStringLiteral("fallback"))
        || lower.contains(QStringLiteral("unavailable")));
    if (selected == 1) return !isError && !isWarning;
    if (selected == 2) return isWarning;
    return isError;
}

void LogPage::rebuildView() {
    QStringList visible;
    visible.reserve(m_allLines.size());
    for (const auto& line : m_allLines) {
        if (acceptsLine(line))
            visible.push_back(line);
    }
    m_logText->setPlainText(visible.join(QLatin1Char('\n')));
    auto* bar = m_logText->verticalScrollBar();
    bar->setValue(bar->maximum());
}
