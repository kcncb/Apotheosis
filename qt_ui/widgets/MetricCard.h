#pragma once

#include <QFrame>

class QLabel;

// KPI 指标卡:label + 图标 / 大数值 + 单位 / 语义副行。
class MetricCard : public QFrame {
    Q_OBJECT

public:
    explicit MetricCard(const QString& label, const QString& iconName = QString(),
                        QWidget* parent = nullptr);

    void setValue(const QString& value);
    void setUnit(const QString& unit);
    void setSub(const QString& text, const QString& cssColor = QString());

private:
    QLabel* m_value{};
    QLabel* m_unit{};
    QLabel* m_sub{};
};
