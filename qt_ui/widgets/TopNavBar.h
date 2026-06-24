#pragma once

#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class StatusPill;

// 应用外壳顶部栏:品牌 + 主导航(下划线指示)+ 全局操作(状态 / 保存 / 头像)。
class TopNavBar : public QWidget {
    Q_OBJECT

public:
    explicit TopNavBar(QWidget* parent = nullptr);

    void setPrimaryItems(const QStringList& labels);
    void setCurrentPrimary(int index);
    int currentPrimary() const;
    void setSessionStatus(bool running, const QString& text);

signals:
    void primaryChanged(int index);
    void saveClicked();

private:
    QButtonGroup* m_group{};
    QHBoxLayout* m_navRow{};
    StatusPill* m_status{};
};
