#pragma once

#include <QWidget>
#include <QList>
#include "config/ConfigManager.h"

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QTimer;
class QVBoxLayout;

class CrosshairPage : public QWidget {
    Q_OBJECT

public:
    explicit CrosshairPage(QWidget* parent = nullptr);

private:
    void loadConfig();
    void rebuildColorList();
    void saveCrosshairColors();

    // ---- Color operations ----
    void addPreset(int presetIdx);
    void addNewColor();
    void removeColorAt(int index);

    // ---- Screen eyedropper (取色) ----
    void toggleColorPick();
    void pollPickedColor();
    void applyPickedColor(int h, int s, int v);
    void finishPicking();

    // ---- Crosshair sampling region ----
    QSpinBox* m_rectW{};
    QSpinBox* m_rectH{};

    // ---- Crosshair shape tolerance ----
    QSpinBox* m_minPixels{};
    QSpinBox* m_closeRadius{};
    QDoubleSpinBox* m_smoothSpin{};
    QSlider* m_smoothSlider{};

    // ---- Color Palette UI ----
    QList<ConfigManager::ColorProfile> m_colors;
    QWidget* m_colorListContainer{};
    QVBoxLayout* m_colorListLayout{};
    QComboBox* m_presetCombo{};
    QPushButton* m_addPresetBtn{};
    QPushButton* m_addColorBtn{};

    // ---- Screen eyedropper ----
    QPushButton* m_pickColorBtn{};
    QTimer* m_pickTimer{};
    int m_pickToken = 0;
};
