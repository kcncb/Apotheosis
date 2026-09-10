#pragma once

#include <QWidget>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QPushButton;
class QComboBox;
class QTimer;

class CrosshairPage : public QWidget {
    Q_OBJECT

public:
    explicit CrosshairPage(QWidget* parent = nullptr);

private:
    void loadConfig();

    // ---- Crosshair color table helpers ----
    void addCrosshairColorRow(const QString& name, bool enabled,
                              int hLo, int hHi, int sLo, int sHi, int vLo, int vHi);
    void addEmptyCrosshairColor();
    void removeCrosshairSelectedRows();
    void addCrosshairPreset();
    void saveCrosshairColors();

    // ---- Crosshair colour eyedropper (取色) ----
    // Arm/cancel pick mode, poll the OpenCV preview thread for a result, and
    // turn one sampled HSV point into a (wide-tolerance) band row.
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

    // ---- Crosshair color table ----
    QTableWidget* m_colorTable{};
    QPushButton* m_addColorBtn{};
    QPushButton* m_removeColorBtn{};
    QComboBox* m_presetCombo{};

    // ---- Crosshair colour eyedropper ----
    QPushButton* m_pickColorBtn{};
    QTimer* m_pickTimer{};
    int m_pickToken = 0;   // 0 = not picking; else this page's color_picker token


};
