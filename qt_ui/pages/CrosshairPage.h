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

    // ---- Laser color table helpers ----
    void addLaserColorRow(const QString& name, bool enabled,
                          int hLo, int hHi, int sLo, int sHi, int vLo, int vHi);
    void addEmptyLaserColor();
    void removeLaserSelectedRows();
    void addLaserPreset();
    void saveLaserColors();

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
    bool m_picking = false;

    // ---- Laser sampling region & params ----
    QSpinBox* m_laserRectW{};
    QSpinBox* m_laserRectH{};
    QSpinBox* m_laserCenterX{};
    QSpinBox* m_laserCenterY{};
    QSpinBox* m_laserTargetCenterX{};
    QSpinBox* m_laserTargetCenterY{};
    QSpinBox* m_laserTargetRectW{};
    QSpinBox* m_laserTargetRectH{};
    QDoubleSpinBox* m_laserElongSpin{};
    QSlider* m_laserElongSlider{};
    QSpinBox* m_laserMinPixels{};
    QSpinBox* m_laserCloseRadius{};
    QDoubleSpinBox* m_laserSmoothSpin{};
    QSlider* m_laserSmoothSlider{};

    // ---- Laser color table ----
    QTableWidget* m_laserColorTable{};
    QPushButton* m_addLaserColorBtn{};
    QPushButton* m_removeLaserColorBtn{};
    QComboBox* m_laserPresetCombo{};
};
