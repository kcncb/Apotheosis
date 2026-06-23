#pragma once

#include <QMainWindow>

class QTabBar;
class QStackedWidget;
class QTimer;
class StatusBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void selectPage(int primary, int secondary);

private slots:
    void onPrimaryTabChanged(int index);
    void onSecondaryTabChanged(int index);
    void pollMonitorTelemetry();

private:
    void setupPages();
    QWidget* createPage(const QString& name);

    QTabBar* m_primaryTabs{};
    QTabBar* m_secondaryTabs{};
    QStackedWidget* m_pageStack{};
    StatusBar* m_statusBar{};
    QTimer* m_monitorTimer{};

    struct PageRange {
        int first{};
        int count{};
    };

    QVector<PageRange> m_tabPages;

    class TargetPage*        m_targetPage{};
    class HotkeyPage*        m_hotkeyPage{};
    class StatsPage*         m_statsPage{};
    class LogPage*           m_logPage{};
    class DebugPage*         m_debugPage{};
    class AutoCapturePage*   m_autoCapPage{};

    // Last AppLog snapshot size — only tail-new lines get pushed each tick.
    int m_logCursor = 0;
};
