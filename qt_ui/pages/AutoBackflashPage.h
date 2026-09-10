#pragma once

#include <QWidget>

#include <cstddef>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;
class ToggleSwitch;

class AutoBackflashPage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoBackflashPage(QWidget* parent = nullptr);

private slots:
    void refreshClasses();
    void refreshRuntimeStatus();
    void runTest();

private:
    std::size_t classFingerprint() const;
    void rebuildClassList();
    void saveClassSelection();
    void markChanged();

    ToggleSwitch* m_enabled{};
    QListWidget* m_classes{};
    QLabel* m_classHint{};
    QSpinBox* m_confirmFrames{};
    QSpinBox* m_turnAmount{};
    QSpinBox* m_turnSpeed{};
    QSpinBox* m_returnDelay{};
    QSpinBox* m_returnSpeed{};
    QSpinBox* m_cooldown{};
    QPushButton* m_testButton{};
    QLabel* m_runtimeStatus{};
    QTimer* m_pollTimer{};
    int m_lastClassCount = -1;
    std::size_t m_lastFingerprint = 0;
    bool m_rebuilding = false;
};
