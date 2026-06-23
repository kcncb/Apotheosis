#pragma once

#include <QWidget>
#include <cstddef>

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

class TargetPage : public QWidget {
    Q_OBJECT

public:
    explicit TargetPage(QWidget* parent = nullptr);

signals:
    void classFiltersChanged();

public slots:
    void refreshFromRuntime();

private:
    void rebuildTable();
    static size_t computeFilterFingerprint();

    QLabel* m_statusLabel{};
    QVBoxLayout* m_tableLayout{};
    QWidget* m_tableWidget{};
    QTimer* m_pollTimer{};
    int m_lastFilterCount = -1;
    size_t m_lastFingerprint = 0;
};
