#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class ToggleSwitch;

class AutoCapturePage : public QWidget {
    Q_OBJECT

public:
    explicit AutoCapturePage(QWidget* parent = nullptr);

    // Live readouts, driven by MainWindow's monitor poll timer.
    void setForceHeld(bool held);
    void setSavedCounts(int session, int total);

private slots:
    void onLoadConfig();
    void saveToConfig();
    void onResetCounter();
    void onOpenDir();

private:
    void buildSwitchCard();
    void buildThresholdCard();
    void buildForceKeyCard();
    void buildOutputCard();
    void buildStatusCard();

    // ── Switch ──
    ToggleSwitch* m_enabled{};

    // ── Threshold zone ──
    ToggleSwitch*   m_useHigh{};
    QDoubleSpinBox* m_highConf{};
    ToggleSwitch*   m_useLow{};
    QDoubleSpinBox* m_lowConf{};
    ToggleSwitch*   m_anyDetection{};    // 忽略阈值,任意检测触发
    ToggleSwitch*   m_useFlashlight{};   // 寻光命中触发
    QSpinBox*       m_cooldownMs{};

    // ── Force key ──
    QLineEdit*      m_forceKeys{};

    // ── Output ──
    QLineEdit*      m_outputDir{};
    ToggleSwitch*   m_saveLabel{};
    QPushButton*    m_openDirBtn{};

    // ── Status (read-only) ──
    QLabel*         m_forceHeldLabel{};
    QLabel*         m_savedSessionLabel{};
    QLabel*         m_savedTotalLabel{};
    QPushButton*    m_resetBtn{};

    bool m_loading = false;
};
