#include "widgets/FormKit.h"
#include "widgets/ToggleSwitch.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QWidget>

#include <cmath>

namespace {

constexpr int kLabelWidth = 88;
constexpr int kValueWidth = 70;

QLabel* makeLabel(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("class", "secondary");
    l->setFixedWidth(kLabelWidth);
    return l;
}

QWidget* makeRow(QHBoxLayout*& layoutOut) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(12);
    layoutOut = h;
    return row;
}

}

namespace FormKit {

QWidget* sliderRow(const QString& label, int min, int max, int value,
                   QSlider*& sliderOut, QSpinBox*& spinOut,
                   const QString& suffix) {
    QHBoxLayout* h = nullptr;
    QWidget* row = makeRow(h);

    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(value);

    auto* spin = new QSpinBox;
    spin->setObjectName("valueSpin");
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setRange(min, max);
    spin->setValue(value);
    spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spin->setFixedWidth(kValueWidth);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);

    QObject::connect(slider, &QSlider::valueChanged, spin, [spin](int v) {
        QSignalBlocker b(spin);
        spin->setValue(v);
    });
    QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), slider, [slider](int v) {
        QSignalBlocker b(slider);
        slider->setValue(v);
    });

    h->addWidget(makeLabel(label));
    h->addWidget(slider, 1);
    h->addWidget(spin);

    sliderOut = slider;
    spinOut = spin;
    return row;
}

QWidget* sliderRowD(const QString& label, double min, double max, double value,
                    double step, int decimals,
                    QSlider*& sliderOut, QDoubleSpinBox*& spinOut,
                    const QString& suffix) {
    QHBoxLayout* h = nullptr;
    QWidget* row = makeRow(h);

    const int steps = static_cast<int>(std::lround((max - min) / step));

    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, steps);
    slider->setValue(static_cast<int>(std::lround((value - min) / step)));

    auto* spin = new QDoubleSpinBox;
    spin->setObjectName("valueSpin");
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spin->setFixedWidth(kValueWidth);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);

    QObject::connect(slider, &QSlider::valueChanged, spin, [spin, min, step](int v) {
        QSignalBlocker b(spin);
        spin->setValue(min + v * step);
    });
    QObject::connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), slider,
                     [slider, min, step](double v) {
        QSignalBlocker b(slider);
        slider->setValue(static_cast<int>(std::lround((v - min) / step)));
    });

    h->addWidget(makeLabel(label));
    h->addWidget(slider, 1);
    h->addWidget(spin);

    sliderOut = slider;
    spinOut = spin;
    return row;
}

QWidget* fieldRow(const QString& label, QWidget* control) {
    QHBoxLayout* h = nullptr;
    QWidget* row = makeRow(h);
    h->addWidget(makeLabel(label));
    h->addWidget(control, 1);
    return row;
}

QWidget* toggleRow(const QString& label, bool checked, ToggleSwitch*& toggleOut) {
    QHBoxLayout* h = nullptr;
    QWidget* row = makeRow(h);

    auto* l = new QLabel(label);
    auto* toggle = new ToggleSwitch;
    toggle->setChecked(checked);

    h->addWidget(l);
    h->addStretch();
    h->addWidget(toggle);

    toggleOut = toggle;
    return row;
}

}
