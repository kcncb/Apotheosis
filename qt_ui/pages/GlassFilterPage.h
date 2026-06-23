#pragma once

#include <QWidget>

class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QPushButton;
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

    ToggleSwitch*   m_showPreview{};
    QDoubleSpinBox* m_edgeRingFrac{};
    QSlider*        m_edgeRingFracSlider{};
    QDoubleSpinBox* m_coverageThreshold{};
    QSlider*        m_coverageThresholdSlider{};
    QSpinBox*       m_minBoxShortSide{};
    QSlider*        m_minBoxShortSideSlider{};
    QTableWidget*   m_colorTable{};
};
