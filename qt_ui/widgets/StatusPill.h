#pragma once

#include <QWidget>

class QLabel;

// 小型状态药丸:圆点 + 文本 + 语义底色。TopNav 与概览 Hero 复用。
class StatusPill : public QWidget {
    Q_OBJECT

public:
    enum Tone { Neutral, Success, Warning, Danger };

    explicit StatusPill(QWidget* parent = nullptr);

    void setStatus(const QString& text, Tone tone = Neutral);

private:
    QLabel* m_dot{};
    QLabel* m_text{};
};
