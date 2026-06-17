#include "widgets/IconFont.h"

#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QLabel>

namespace {

QString g_family;

const QHash<QString, ushort>& glyphMap() {
    static const QHash<QString, ushort> kMap = {
        {"adjustments", 0xea03},
        {"adjustments-horizontal", 0xec38},
        {"brain", 0xf59f},
        {"bug", 0xea48},
        {"camera", 0xea54},
        {"chart-line", 0xea5c},
        {"chevron-down", 0xea5f},
        {"circle-plus", 0xea69},
        {"color-swatch", 0xeb61},
        {"copy", 0xea7a},
        {"cpu", 0xef8e},
        {"crosshair", 0xec3e},
        {"device-desktop", 0xea89},
        {"dots-vertical", 0xea94},
        {"file-export", 0xede9},
        {"folder", 0xeaad},
        {"gauge", 0xeab1},
        {"history", 0xebea},
        {"key", 0xeac7},
        {"keyboard", 0xebd6},
        {"layers-intersect", 0xeac9},
        {"lock", 0xeae2},
        {"login", 0xeba7},
        {"minus", 0xeaf2},
        {"palette", 0xeb01},
        {"photo", 0xeb0a},
        {"player-play", 0xed46},
        {"player-stop", 0xed4a},
        {"plug", 0xebd9},
        {"plus", 0xeb0b},
        {"refresh", 0xeb13},
        {"settings", 0xeb20},
        {"stack-2", 0xeef7},
        {"target", 0xeb35},
        {"terminal-2", 0xebef},
        {"trash", 0xeb41},
        {"user-circle", 0xef68},
        {"user-plus", 0xeb4b},
        {"vector-spline", 0xf565},
        {"world", 0xeb54},
    };
    return kMap;
}

QString resolveFontPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/../assets/tabler-icons.ttf",
        appDir + "/../../qt_ui/assets/tabler-icons.ttf",
        appDir + "/assets/tabler-icons.ttf",
        "assets/tabler-icons.ttf",
        "../assets/tabler-icons.ttf",
        "../qt_ui/assets/tabler-icons.ttf",
    };
    for (const auto& path : candidates) {
        if (QFile::exists(path))
            return path;
    }
    return {};
}

}

namespace IconFont {

void load() {
    const QString path = resolveFontPath();
    if (path.isEmpty())
        return;
    const int id = QFontDatabase::addApplicationFont(path);
    if (id < 0)
        return;
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (!families.isEmpty())
        g_family = families.front();
}

bool available() {
    return !g_family.isEmpty();
}

QFont font(int pixelSize) {
    QFont f(g_family);
    f.setPixelSize(pixelSize);
    return f;
}

QChar glyph(const QString& name) {
    return QChar(glyphMap().value(name, 0));
}

QLabel* label(const QString& name, int pixelSize, const QString& cssColor) {
    auto* lbl = new QLabel(QString(glyph(name)));
    lbl->setFont(font(pixelSize));
    lbl->setAlignment(Qt::AlignCenter);
    if (!cssColor.isEmpty())
        lbl->setStyleSheet("color:" + cssColor + ";");
    return lbl;
}

}
