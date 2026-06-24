#pragma once

#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QVBoxLayout;
class QLabel;

// 二级导航侧边栏:展示当前主分组下的子页面列表(图标 + 名称),选中项高亮。
// 顶部主导航负责一级,本侧边栏负责二级。
class SideNav : public QWidget {
    Q_OBJECT

public:
    explicit SideNav(QWidget* parent = nullptr);

    void setItems(const QString& groupTitle, const QStringList& labels,
                  const QStringList& iconNames);
    void setCurrentIndex(int index);
    int currentIndex() const;

signals:
    void currentChanged(int index);

private:
    void recolorIcons();

    QLabel* m_title{};
    QVBoxLayout* m_itemsLayout{};
    QButtonGroup* m_group{};
    QStringList m_icons;
};
