#pragma once

#include <QWidget>

class QLineEdit;
class QTextEdit;
class QLabel;
class QPushButton;
class ToggleSwitch;

class MacroPage : public QWidget {
    Q_OBJECT

public:
    explicit MacroPage(QWidget* parent = nullptr);

private slots:
    void loadConfig();

private:
    void onBrowseScript();
    void onReloadScript();
    void onUnloadScript();
    void onClearLog();
    void updateStatus();

    // Macro script card
    ToggleSwitch* m_enabledToggle{};
    ToggleSwitch* m_primaryBtnToggle{};
    QLineEdit* m_scriptPath{};
    QPushButton* m_browseBtn{};
    QPushButton* m_reloadBtn{};
    QPushButton* m_unloadBtn{};
    QLabel* m_statusLabel{};
    QLabel* m_errorLabel{};

    // Log card
    QTextEdit* m_logView{};
    QPushButton* m_clearLogBtn{};
};
