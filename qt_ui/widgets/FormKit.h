#pragma once

#include <QString>

class QWidget;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class ToggleSwitch;

// Standardized form rows matching the refined light design.
// Each builder returns a row QWidget ready to add to a card's contentLayout(),
// and hands back the inner controls through out-parameters.
namespace FormKit {

// label (fixed width) + slider + right-aligned integer value (no spin buttons)
QWidget* sliderRow(const QString& label, int min, int max, int value,
                   QSlider*& sliderOut, QSpinBox*& spinOut,
                   const QString& suffix = QString());

// label (fixed width) + slider + right-aligned float value (no spin buttons)
QWidget* sliderRowD(const QString& label, double min, double max, double value,
                    double step, int decimals,
                    QSlider*& sliderOut, QDoubleSpinBox*& spinOut,
                    const QString& suffix = QString());

// label (fixed width) + arbitrary control filling the rest (combo / line edit)
QWidget* fieldRow(const QString& label, QWidget* control);

// label (stretch) + iOS toggle aligned right
QWidget* toggleRow(const QString& label, bool checked, ToggleSwitch*& toggleOut);

}
