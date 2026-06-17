#pragma once

#include <QMainWindow>

class QTabBar;
class QStackedWidget;
class StatusBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void selectPage(int primary, int secondary);

private slots:
    void onPrimaryTabChanged(int index);
    void onSecondaryTabChanged(int index);

private:
    void setupPages();
    QWidget* createPage(const QString& name);

    QTabBar* m_primaryTabs{};
    QTabBar* m_secondaryTabs{};
    QStackedWidget* m_pageStack{};
    StatusBar* m_statusBar{};

    struct PageRange {
        int first{};
        int count{};
    };

    QVector<PageRange> m_tabPages;
};
