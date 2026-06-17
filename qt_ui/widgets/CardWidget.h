#pragma once

#include <QWidget>

class QLabel;
class QVBoxLayout;
class QHBoxLayout;

class CardWidget : public QWidget {
    Q_OBJECT

public:
    explicit CardWidget(const QString& title, QWidget* parent = nullptr);
    CardWidget(const QString& title, const QString& iconName, QWidget* parent = nullptr);

    QVBoxLayout* contentLayout() const;
    void setCollapsible(bool collapsible);
    void setIcon(const QString& iconName);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void init(const QString& title, const QString& iconName);
    void toggleCollapsed();

    QLabel* m_iconLabel{};
    QLabel* m_titleLabel{};
    QLabel* m_chevron{};
    QHBoxLayout* m_headerLayout{};
    QWidget* m_headerWidget{};
    QWidget* m_contentWidget{};
    QVBoxLayout* m_contentLayout{};
    bool m_collapsible = false;
    bool m_collapsed = false;
};
