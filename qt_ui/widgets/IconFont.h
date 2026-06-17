#pragma once

#include <QChar>
#include <QFont>
#include <QString>

class QLabel;

namespace IconFont {

void load();
bool available();
QFont font(int pixelSize);
QChar glyph(const QString& name);
QLabel* label(const QString& name, int pixelSize, const QString& cssColor = QString());

}
