#include "widgets/StatusPill.h"

#include <QHBoxLayout>
#include <QLabel>

StatusPill::StatusPill(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(7);

    m_dot = new QLabel(this);
    m_dot->setFixedSize(7, 7);

    m_text = new QLabel(this);
    m_text->setStyleSheet("background:transparent;");

    row->addWidget(m_dot);
    row->addWidget(m_text);

    setStatus(QString(), Neutral);
}

void StatusPill::setStatus(const QString& text, Tone tone) {
    // No filled pill background — just a colored dot + colored text, for a clean,
    // subtle status indicator that sits flush in the top bar.
    QString dot, fg;
    switch (tone) {
        case Success: dot = "#22C55E"; fg = "#16A34A"; break;
        case Warning: dot = "#F5A623"; fg = "#B45309"; break;
        case Danger:  dot = "#E5484D"; fg = "#C0362C"; break;
        case Neutral:
        default:      dot = "#B0B0B8"; fg = "#6B6B73"; break;
    }

    setStyleSheet(QStringLiteral("background:transparent;"));
    m_dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(dot));
    m_text->setStyleSheet(QStringLiteral("background:transparent; color:%1; font-size:12px;").arg(fg));
    m_text->setText(text);
}
