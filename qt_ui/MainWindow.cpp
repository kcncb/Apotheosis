#include "MainWindow.h"

#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include "widgets/StatusBar.h"
#include "pages/SessionPage.h"
#include "pages/ModelToolsPage.h"
#include "pages/CapturePage.h"
#include "pages/TargetPage.h"
#include "pages/HardwarePage.h"
#include "pages/AiModelPage.h"
#include "pages/DepthPage.h"
#include "pages/HotkeyPage.h"
#include "pages/CrosshairPage.h"
#include "pages/StatsPage.h"
#include "pages/LogPage.h"
#include "pages/DebugPage.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Apotheosis");
    resize(960, 640);
    setMinimumSize(720, 480);

    auto* central = new QWidget(this);
    central->setObjectName("centralWidget");
    setCentralWidget(central);

    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_primaryTabs = new QTabBar(central);
    m_primaryTabs->setObjectName("primaryTabs");
    m_primaryTabs->setDocumentMode(true);
    m_primaryTabs->setExpanding(false);
    m_primaryTabs->setDrawBase(false);
    m_primaryTabs->setTabsClosable(false);

    m_secondaryTabs = new QTabBar(central);
    m_secondaryTabs->setObjectName("secondaryTabs");
    m_secondaryTabs->setDocumentMode(true);
    m_secondaryTabs->setExpanding(false);
    m_secondaryTabs->setDrawBase(false);

    m_pageStack = new QStackedWidget(central);
    m_statusBar = new StatusBar(central);

    layout->addWidget(m_primaryTabs);
    layout->addWidget(m_secondaryTabs);
    layout->addWidget(m_pageStack, 1);
    layout->addWidget(m_statusBar);

    setupPages();

    m_statusBar->setInferenceStatus(false);
    m_statusBar->setBackend("TRT");

    connect(m_primaryTabs, &QTabBar::currentChanged,
            this, &MainWindow::onPrimaryTabChanged);
    connect(m_secondaryTabs, &QTabBar::currentChanged,
            this, &MainWindow::onSecondaryTabChanged);

    m_primaryTabs->setCurrentIndex(0);
    onPrimaryTabChanged(0);
}

void MainWindow::setupPages() {
    const QVector<QPair<QString, QStringList>> tabStructure = {
        {QStringLiteral("会话"), {QStringLiteral("推理启动"), QStringLiteral("模型工具")}},
        {QStringLiteral("配置"), {QStringLiteral("画面采集"), QStringLiteral("目标"), QStringLiteral("硬件"), QStringLiteral("AI 模型"), QStringLiteral("深度模型")}},
        {QStringLiteral("控制"), {QStringLiteral("瞄准热键"), QStringLiteral("准星找色")}},
        {QStringLiteral("监控"), {QStringLiteral("性能统计"), QStringLiteral("日志"), QStringLiteral("调试")}},
    };

    int pageIndex = 0;

    for (const auto& [primary, secondaries] : tabStructure) {
        m_primaryTabs->addTab(primary);

        PageRange range;
        range.first = pageIndex;
        range.count = secondaries.size();

        for (const auto& name : secondaries) {
            QWidget* page = createPage(name);
            m_pageStack->addWidget(page);
            ++pageIndex;
        }

        m_tabPages.append(range);
    }
}

void MainWindow::selectPage(int primary, int secondary) {
    m_primaryTabs->setCurrentIndex(primary);
    m_secondaryTabs->setCurrentIndex(secondary);
    onSecondaryTabChanged(secondary);
}

void MainWindow::onPrimaryTabChanged(int index) {
    if (index < 0 || index >= m_tabPages.size()) {
        return;
    }

    const QVector<QStringList> secondaryLabels = {
        {QStringLiteral("推理启动"), QStringLiteral("模型工具")},
        {QStringLiteral("画面采集"), QStringLiteral("目标"), QStringLiteral("硬件"), QStringLiteral("AI 模型"), QStringLiteral("深度模型")},
        {QStringLiteral("瞄准热键"), QStringLiteral("准星找色")},
        {QStringLiteral("性能统计"), QStringLiteral("日志"), QStringLiteral("调试")},
    };

    QSignalBlocker blocker(m_secondaryTabs);
    while (m_secondaryTabs->count() > 0) {
        m_secondaryTabs->removeTab(0);
    }

    for (const auto& label : secondaryLabels[index]) {
        m_secondaryTabs->addTab(label);
    }

    blocker.unblock();
    m_secondaryTabs->setCurrentIndex(0);
    onSecondaryTabChanged(0);
}

void MainWindow::onSecondaryTabChanged(int index) {
    int primaryIndex = m_primaryTabs->currentIndex();
    if (primaryIndex < 0 || primaryIndex >= m_tabPages.size()) {
        return;
    }
    if (index < 0 || index >= m_tabPages[primaryIndex].count) {
        return;
    }

    m_pageStack->setCurrentIndex(m_tabPages[primaryIndex].first + index);
}

QWidget* MainWindow::createPage(const QString& name) {
    if (name == QStringLiteral("推理启动"))   return new SessionPage();
    if (name == QStringLiteral("模型工具"))   return new ModelToolsPage();
    if (name == QStringLiteral("画面采集"))   return new CapturePage();
    if (name == QStringLiteral("目标"))       return new TargetPage();
    if (name == QStringLiteral("硬件"))       return new HardwarePage();
    if (name == QStringLiteral("AI 模型"))    return new AiModelPage();
    if (name == QStringLiteral("深度模型"))   return new DepthPage();
    if (name == QStringLiteral("瞄准热键"))   return new HotkeyPage();
    if (name == QStringLiteral("准星找色"))   return new CrosshairPage();
    if (name == QStringLiteral("性能统计"))   return new StatsPage();
    if (name == QStringLiteral("日志"))       return new LogPage();
    if (name == QStringLiteral("调试"))       return new DebugPage();
    return new QWidget();
}
