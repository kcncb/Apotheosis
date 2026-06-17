#pragma once

#include <QWidget>

class QTextEdit;
class QComboBox;
class QPushButton;

class LogPage : public QWidget {
    Q_OBJECT

public:
    explicit LogPage(QWidget* parent = nullptr);
    void appendLog(const QString& line);

private:
    void clearLog();
    void copyToClipboard();
    void exportLog();
    void onLevelChanged(int index);

    QComboBox* m_levelCombo{};
    QTextEdit* m_logText{};
    QPushButton* m_clearBtn{};
    QPushButton* m_copyBtn{};
    QPushButton* m_exportBtn{};
};
