#pragma once

#include <QWidget>
#include <QPainter>
#include <QPainterPath>

class TriggerVisualWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TriggerVisualWidget(QWidget* parent = nullptr)
        : QWidget(parent), m_percent(100)
    {
        setFixedHeight(90);
        setAttribute(Qt::WA_Hover, true);
    }

    void setPercent(int pct)
    {
        int clamped = std::clamp(pct, 10, 300);
        if (m_percent != clamped)
        {
            m_percent = clamped;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int w = width();
        const int h = height();

        // 背景卡槽
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x18, 0x18, 0x1B, 40)); // 浅暗灰背景
        p.drawRoundedRect(rect(), 8, 8);

        // 人体骨骼/受击模型参考线 (固定比例胶囊体)
        const int cx = w / 2;
        const int cy = h / 2;
        const int bodyW = 28;
        const int bodyH = 64;
        const QRect bodyRect(cx - bodyW / 2, cy - bodyH / 2, bodyW, bodyH);

        // 绘制人形身体底框 (浅灰线框)
        p.setPen(QPen(QColor(0xA1, 0xA1, 0xAA, 120), 1.5, Qt::DashLine));
        p.setBrush(QColor(0xFF, 0xFF, 0xFF, 15));
        p.drawRoundedRect(bodyRect, 8, 8);

        // 头部圆
        const int headR = 7;
        const QPoint headCenter(cx, bodyRect.top() + 10);
        p.setPen(QPen(QColor(0xA1, 0xA1, 0xAA, 160), 1.5));
        p.setBrush(QColor(0xFF, 0xFF, 0xFF, 25));
        p.drawEllipse(headCenter, headR, headR);

        // 根据 m_percent 动态绘制当前的实际触发容许受击区
        // 100% 代表正好贴合身体宽度与高度
        const float scale = m_percent / 100.0f;
        const int zoneW = std::clamp(static_cast<int>(bodyW * scale), 6, w - 16);
        const int zoneH = std::clamp(static_cast<int>(bodyH * scale), 6, h - 10);
        const QRect zoneRect(cx - zoneW / 2, cy - zoneH / 2, zoneW, zoneH);

        // 动态判定区高亮色彩 (紫色/科技蓝)
        QColor zoneColor(0x7C, 0x3A, 0xED, 45);   // 填充浅紫
        QColor borderColor(0xA7, 0x8B, 0xFA, 220); // 边框亮紫
        if (m_percent > 120)
        {
            // 预开火范围变橙红色警告
            zoneColor = QColor(0xEA, 0x58, 0x0C, 40);
            borderColor = QColor(0xFB, 0x92, 0x3C, 220);
        }

        p.setPen(QPen(borderColor, 1.5));
        p.setBrush(zoneColor);
        p.drawRoundedRect(zoneRect, 4, 4);

        // 准星中心十字 ⌖
        p.setPen(QPen(QColor(0xEF, 0x44, 0x44, 230), 2.0)); // 红色发光瞄准准星
        const int crossSize = 5;
        p.drawLine(cx - crossSize, cy, cx + crossSize, cy);
        p.drawLine(cx, cy - crossSize, cx, cy + crossSize);

        // 文本提示
        p.setPen(QColor(0x71, 0x71, 0x7A));
        QFont font = p.font();
        font.setPointSize(9);
        p.setFont(font);

        QString desc;
        if (m_percent <= 50) desc = QStringLiteral("高精严苛 (头部锁点)");
        else if (m_percent <= 100) desc = QStringLiteral("精准受击 (核心身躯)");
        else desc = QStringLiteral("宽容抢发 (预开火/提前截击)");

        p.drawText(QRect(12, h - 22, w - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, desc);
        p.drawText(QRect(12, h - 22, w - 24, 18), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1% 容差").arg(m_percent));
    }

private:
    int m_percent;
};
