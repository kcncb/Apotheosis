#include "pages/MacroPage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QVBoxLayout>

MacroPage::MacroPage(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto& cfg = ConfigManager::instance();

    // ====================================================================
    // Card 1: 宏脚本
    // ====================================================================
    auto* scriptCard = new CardWidget(
        QStringLiteral("\xe5\xae\x8f\xe8\x84\x9a\xe6\x9c\xac"),   // 宏脚本
        QStringLiteral("script"));
    auto* sc = scriptCard->contentLayout();

    // 启用 Lua 宏 toggle
    sc->addWidget(
        FormKit::toggleRow(
            QStringLiteral("\xe5\x90\xaf\xe7\x94\xa8 Lua \xe5\xae\x8f (G HUB \xe5\x85\xbc\xe5\xae\xb9)"),  // 启用 Lua 宏 (G HUB 兼容)
            cfg.macroEnabled(), m_enabledToggle));
    m_enabledToggle->setToolTip(tr(
        "\xe5\xbc\x80\xe5\x90\xaf\xe5\x90\x8e\xe4\xbc\x9a\xe5\x8a\xa0\xe8\xbd\xbd\xe4\xb8\x8b\xe6\x96\xb9\xe9\x85\x8d\xe7\xbd\xae\xe7\x9a\x84 Lua \xe8\x84\x9a\xe6\x9c\xac\xef\xbc\x8c\xe9\xbc\xa0\xe6\xa0\x87\xe6\x8c\x89\xe9\x94\xae\xe4\xba\x8b\xe4\xbb\xb6\xe4\xbc\x9a\xe6\xb4\xbe\xe5\x8f\x91\xe5\x88\xb0\xe8\x84\x9a\xe6\x9c\xac\xe7\x9a\x84 OnEvent \xe5\x9b\x9e\xe8\xb0\x83\xe3\x80\x82\n"
        "\xe8\xbe\x93\xe5\x85\xa5\xe4\xbc\x9a\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x9a\xe8\xbf\x87\xe5\xbd\x93\xe5\x89\x8d\xe9\x80\x89\xe5\xae\x9a\xe7\x9a\x84\xe7\xa1\xac\xe4\xbb\xb6\xe5\x90\x8e\xe7\xab\xaf\xe5\x8f\x91\xe5\x87\xba\xe3\x80\x82"));
    // 开启后会加载下方配置的 Lua 脚本，鼠标按键事件会派发到脚本的 OnEvent 回调。
    // 输入会自动通过当前选定的硬件后端发出。
    connect(m_enabledToggle, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setMacroEnabled(v); });

    // 接收左键事件 toggle
    sc->addWidget(
        FormKit::toggleRow(
            QStringLiteral("\xe6\x8e\xa5\xe6\x94\xb6\xe5\xb7\xa6\xe9\x94\xae\xe4\xba\x8b\xe4\xbb\xb6"),  // 接收左键事件
            cfg.macroPrimaryButtonEvents(), m_primaryBtnToggle));
    m_primaryBtnToggle->setToolTip(tr(
        "\xe7\xad\x89\xe5\x90\x8c\xe4\xba\x8e\xe8\x84\x9a\xe6\x9c\xac\xe9\x87\x8c\xe8\xb0\x83\xe7\x94\xa8 EnablePrimaryMouseButtonEvents(true)\xe3\x80\x82\n"
        "\xe5\x85\xb3\xe9\x97\xad\xe6\x97\xb6\xe5\xb7\xa6\xe9\x94\xae\xe4\xba\x8b\xe4\xbb\xb6\xe4\xb8\x8d\xe4\xbc\x9a\xe6\xb4\xbe\xe5\x8f\x91\xe5\x88\xb0 OnEvent\xe3\x80\x82"));
    // 等同于脚本里调用 EnablePrimaryMouseButtonEvents(true)。
    // 关闭时左键事件不会派发到 OnEvent。
    connect(m_primaryBtnToggle, &ToggleSwitch::toggled,
            this, [](bool v) { ConfigManager::instance().setMacroPrimaryButtonEvents(v); });

    // Script path row: label + line edit + browse button
    m_scriptPath = new QLineEdit;
    m_scriptPath->setText(cfg.macroScriptPath());
    m_scriptPath->setPlaceholderText(tr("Lua \xe8\x84\x9a\xe6\x9c\xac\xe8\xb7\xaf\xe5\xbe\x84..."));  // Lua 脚本路径...
    m_scriptPath->setToolTip(tr(
        "Lua \xe8\x84\x9a\xe6\x9c\xac\xe7\x9a\x84\xe7\xbb\x9d\xe5\xaf\xb9/\xe7\x9b\xb8\xe5\xaf\xb9\xe8\xb7\xaf\xe5\xbe\x84\xef\xbc\x8c\xe5\x8f\xaf\xe4\xbb\xa5\xe7\x9b\xb4\xe6\x8e\xa5\xe7\xb2\x98\xe8\xb4\xb4\xe6\x88\x96\xe7\x94\xa8\xe2\x80\x98\xe6\xb5\x8f\xe8\xa7\x88\xe2\x80\x99\xe6\x8c\x89\xe9\x92\xae\xe9\x80\x89\xe6\x8b\xa9\xe3\x80\x82\xe4\xbf\xae\xe6\x94\xb9\xe5\x90\x8e\xe8\xaf\xb7\xe7\x82\xb9\xe2\x80\x98\xe9\x87\x8d\xe6\x96\xb0\xe5\x8a\xa0\xe8\xbd\xbd\xe2\x80\x99\xe7\x94\x9f\xe6\x95\x88\xe3\x80\x82"));
    // Lua 脚本的绝对/相对路径，可以直接粘贴或用'浏览'按钮选择。修改后请点'重新加载'生效。
    connect(m_scriptPath, &QLineEdit::textChanged,
            this, [](const QString& text) {
                ConfigManager::instance().setMacroScriptPath(text);
            });

    m_browseBtn = new QPushButton(
        QStringLiteral("\xe6\xb5\x8f\xe8\xa7\x88\xe2\x80\xa6"));  // 浏览…
    m_browseBtn->setToolTip(tr(
        "\xe6\x89\x93\xe5\xbc\x80\xe6\x96\x87\xe4\xbb\xb6\xe9\x80\x89\xe6\x8b\xa9\xe6\xa1\x86\xe6\x8c\x91\xe4\xb8\x80\xe4\xb8\xaa .lua \xe6\x96\x87\xe4\xbb\xb6\xef\xbc\x8c\xe6\x8c\x91\xe5\xae\x8c\xe4\xbc\x9a\xe8\x87\xaa\xe5\x8a\xa8\xe5\x8a\xa0\xe8\xbd\xbd\xef\xbc\x88\xe8\x8b\xa5\xe5\xb7\xb2\xe5\x90\xaf\xe7\x94\xa8\xef\xbc\x89\xe3\x80\x82"));
    // 打开文件选择框挑一个 .lua 文件，挑完会自动加载（若已启用）。
    connect(m_browseBtn, &QPushButton::clicked,
            this, &MacroPage::onBrowseScript);

    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(8);
    auto* pathLabel = new QLabel(
        QStringLiteral("\xe8\x84\x9a\xe6\x9c\xac\xe8\xb7\xaf\xe5\xbe\x84"));  // 脚本路径
    pathLabel->setFixedWidth(100);
    pathRow->addWidget(pathLabel);
    pathRow->addWidget(m_scriptPath, 1);
    pathRow->addWidget(m_browseBtn);
    sc->addLayout(pathRow);

    // Action buttons: reload / unload
    m_reloadBtn = new QPushButton(
        QStringLiteral("\xe9\x87\x8d\xe6\x96\xb0\xe5\x8a\xa0\xe8\xbd\xbd"));  // 重新加载
    m_reloadBtn->setToolTip(tr(
        "\xe9\x87\x8d\xe6\x96\xb0\xe8\xaf\xbb\xe5\x8f\x96\xe5\xb9\xb6\xe6\x89\xa7\xe8\xa1\x8c\xe5\xbd\x93\xe5\x89\x8d\xe8\x84\x9a\xe6\x9c\xac\xef\xbc\x88\xe8\x84\x9a\xe6\x9c\xac\xe8\xa2\xab\xe5\xa4\x96\xe9\x83\xa8\xe7\xbc\x96\xe8\xbe\x91\xe5\x90\x8e\xe7\x94\xa8\xe6\xad\xa4\xe6\x8c\x89\xe9\x92\xae\xe7\xab\x8b\xe5\x8d\xb3\xe7\x94\x9f\xe6\x95\x88\xef\xbc\x89\xe3\x80\x82"));
    // 重新读取并执行当前脚本（脚本被外部编辑后用此按钮立即生效）。
    connect(m_reloadBtn, &QPushButton::clicked,
            this, &MacroPage::onReloadScript);

    m_unloadBtn = new QPushButton(
        QStringLiteral("\xe5\x8d\xb8\xe8\xbd\xbd"));  // 卸载
    m_unloadBtn->setToolTip(tr(
        "\xe7\xab\x8b\xe5\x8d\xb3\xe5\x8d\xb8\xe8\xbd\xbd\xe5\xbd\x93\xe5\x89\x8d\xe8\x84\x9a\xe6\x9c\xac\xef\xbc\x88\xe4\xb8\x8d\xe5\xbd\xb1\xe5\x93\x8d\xe2\x80\x98\xe5\x90\xaf\xe7\x94\xa8 Lua \xe5\xae\x8f\xe2\x80\x99\xe5\x8b\xbe\xe9\x80\x89\xe9\xa1\xb9\xef\xbc\x89\xe3\x80\x82"));
    // 立即卸载当前脚本（不影响'启用 Lua 宏'勾选项）。
    connect(m_unloadBtn, &QPushButton::clicked,
            this, &MacroPage::onUnloadScript);

    auto* actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    actionRow->addWidget(m_reloadBtn);
    actionRow->addWidget(m_unloadBtn);
    actionRow->addStretch();
    sc->addLayout(actionRow);

    // Status indicator
    m_statusLabel = new QLabel(
        QStringLiteral("\xe2\x97\x8b \xe6\x9c\xaa\xe5\x8a\xa0\xe8\xbd\xbd"));  // ○ 未加载
    m_statusLabel->setStyleSheet(QStringLiteral("color: #d98c5a;"));
    sc->addWidget(m_statusLabel);

    // Error label (hidden by default)
    m_errorLabel = new QLabel;
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #e66666;"));
    m_errorLabel->setVisible(false);
    sc->addWidget(m_errorLabel);

    layout->addWidget(scriptCard);

    // ====================================================================
    // Card 2: 脚本日志
    // ====================================================================
    auto* logCard = new CardWidget(
        QStringLiteral("\xe8\x84\x9a\xe6\x9c\xac\xe6\x97\xa5\xe5\xbf\x97"),   // 脚本日志
        QStringLiteral("file-text"));
    auto* lc = logCard->contentLayout();

    // Clear log button + hint
    auto* logHeaderRow = new QHBoxLayout;
    logHeaderRow->setContentsMargins(0, 0, 0, 0);
    logHeaderRow->setSpacing(8);

    m_clearLogBtn = new QPushButton(
        QStringLiteral("\xe6\xb8\x85\xe7\xa9\xba\xe6\x97\xa5\xe5\xbf\x97"));  // 清空日志
    connect(m_clearLogBtn, &QPushButton::clicked,
            this, &MacroPage::onClearLog);

    auto* logHintLabel = new QLabel(
        QStringLiteral("OutputLogMessage / print \xe8\xbe\x93\xe5\x87\xba"));  // OutputLogMessage / print 输出
    logHintLabel->setStyleSheet(QStringLiteral("color: #888;"));

    logHeaderRow->addWidget(m_clearLogBtn);
    logHeaderRow->addWidget(logHintLabel);
    logHeaderRow->addStretch();
    lc->addLayout(logHeaderRow);

    // Read-only log text area
    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(200);
    m_logView->setPlaceholderText(tr(
        "\xe8\x84\x9a\xe6\x9c\xac\xe6\x97\xa5\xe5\xbf\x97\xe5\xb0\x86\xe6\x98\xbe\xe7\xa4\xba\xe5\x9c\xa8\xe8\xbf\x99\xe9\x87\x8c\xe2\x80\xa6"));  // 脚本日志将显示在这里…
    lc->addWidget(m_logView);

    layout->addWidget(logCard);

    layout->addStretch();

    // Load initial config values
    loadConfig();
    connect(&cfg, &ConfigManager::configLoaded, this, &MacroPage::loadConfig);
}

void MacroPage::loadConfig() {
    auto& cfg = ConfigManager::instance();
    m_enabledToggle->setChecked(cfg.macroEnabled());
    m_primaryBtnToggle->setChecked(cfg.macroPrimaryButtonEvents());
    m_scriptPath->setText(cfg.macroScriptPath());
}

void MacroPage::onBrowseScript() {
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("\xe9\x80\x89\xe6\x8b\xa9\xe5\xae\x8f\xe8\x84\x9a\xe6\x9c\xac"),  // 选择宏脚本
        QString(),
        tr("Lua \xe8\x84\x9a\xe6\x9c\xac (*.lua);;All files (*.*)"));   // Lua 脚本 (*.lua);;All files (*.*)
    if (path.isEmpty())
        return;

    m_scriptPath->setText(path);
    ConfigManager::instance().setMacroScriptPath(path);
}

void MacroPage::onReloadScript() {
    // Placeholder: the actual reload logic calls into macro::runtime_load_script
    // which lives in the engine side. The Qt UI just persists the config; the
    // engine picks up changes via ConfigManager signals.
}

void MacroPage::onUnloadScript() {
    // Placeholder: engine-side macro::runtime_unload_script() handles actual
    // unloading. The Qt UI signals intent through config / future bridge API.
}

void MacroPage::onClearLog() {
    m_logView->clear();
}

void MacroPage::updateStatus() {
    // Placeholder: in a full integration this would query macro::runtime_is_loaded()
    // and macro::runtime_last_error() to update m_statusLabel and m_errorLabel.
}
