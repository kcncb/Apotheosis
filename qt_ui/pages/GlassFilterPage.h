#pragma once

#include <QWidget>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QPushButton;
class QTimer;
class ToggleSwitch;

class GlassFilterPage : public QWidget {
    Q_OBJECT

public:
    explicit GlassFilterPage(QWidget* parent = nullptr);

private:
    void loadConfig();
    void saveGlassColors();
    void addEmptyColor();
    void removeSelectedRows();

    // ---- Glass colour eyedropper (取色) — mirrors CrosshairPage ----
    void toggleColorPick();
    void pollPickedColor();
    void applyPickedColor(int h, int s, int v);
    void finishPicking();

    ToggleSwitch*   m_showPreview{};
    QSpinBox*       m_filterStrength{};         // 过滤强度 0..100 (single macro knob)
    QSlider*        m_filterStrengthSlider{};
    QTableWidget*   m_colorTable{};

    QPushButton*    m_pickColorBtn{};
    QTimer*         m_pickTimer{};
    int             m_pickToken = 0;            // 0 = not picking
};
