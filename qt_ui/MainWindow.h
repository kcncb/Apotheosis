#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

#include <chrono>

class QStackedWidget;
class QTimer;
class StatusBar;
class TopNavBar;
class SideNav;
class OverviewPage;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void selectPage(int primary, int secondary);

private slots:
    void onPrimaryChanged(int index);
    void onSecondaryChanged(int index);
    void onHeroToggleInference();
    void pollMonitorTelemetry();

private:
    void setupPages();
    QWidget* createPage(const QString& name);

    TopNavBar* m_topNav{};
    SideNav* m_sideNav{};
    QStackedWidget* m_pageStack{};
    StatusBar* m_statusBar{};
    QTimer* m_monitorTimer{};

    struct PageRange {
        int first{};
        int count{};
        QString name;
        QStringList subs;
        QStringList icons;
    };

    QVector<PageRange> m_tabPages;

    class OverviewPage*      m_overviewPage{};
    class TargetPage*        m_targetPage{};
    class HotkeyPage*        m_hotkeyPage{};
    class StatsPage*         m_statsPage{};
    class LogPage*           m_logPage{};
    class DebugPage*         m_debugPage{};
    class AutoCapturePage*   m_autoCapPage{};

    // Last AppLog snapshot size — only tail-new lines get pushed each tick.
    int m_logCursor = 0;

    // Session uptime: stamped on the running false→true edge, read each poll.
    bool m_sessionRunning = false;
    std::chrono::steady_clock::time_point m_sessionStart{};
};
