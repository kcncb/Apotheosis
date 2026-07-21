#pragma once

#include <QWidget>
#include <vector>

class QVBoxLayout;
class QScrollArea;
class QPushButton;

class EventPage : public QWidget
{
    Q_OBJECT
public:
    explicit EventPage(QWidget* parent = nullptr);

private slots:
    void onLoadConfig();
    void saveToConfig();
    void addRule();

private:
    void rebuildFromRules();

    QVBoxLayout* m_listLayout{};   // 每个 rule 一个 CardWidget
    QScrollArea* m_scroll{};
    QPushButton* m_addBtn{};

    bool m_loading = false;
};
