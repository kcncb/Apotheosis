#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QThread>

#include "preview/PreviewWindow.h"
#include "widgets/IconFont.h"

static void applyLightPalette(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#F4F6FA"));
    pal.setColor(QPalette::WindowText, QColor("#17191F"));
    pal.setColor(QPalette::Base, QColor("#FFFFFF"));
    pal.setColor(QPalette::AlternateBase, QColor("#FBFCFD"));
    pal.setColor(QPalette::Text, QColor("#17191F"));
    pal.setColor(QPalette::Button, QColor("#F8F9FB"));
    pal.setColor(QPalette::ButtonText, QColor("#17191F"));
    pal.setColor(QPalette::Highlight, QColor("#5865D8"));
    pal.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    pal.setColor(QPalette::PlaceholderText, QColor("#98A1B0"));
    app.setPalette(pal);

    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI Variable"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Segoe UI")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
}

static QString loadQss() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/preview.qss",
        appDir + "/../preview.qss",
        appDir + "/../../preview/preview.qss",
        "preview.qss",
        "../preview.qss",
    };
    for (const auto& path : candidates) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString::fromUtf8(file.readAll());
    }
    return {};
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Apotheosis Preview");

    applyLightPalette(app);
    IconFont::load();

    if (auto qss = loadQss(); !qss.isEmpty())
        app.setStyleSheet(qss);

    PreviewWindow window;
    window.show();

    if (app.arguments().contains(QStringLiteral("--shot"))) {
        for (int i = 0; i < 12; ++i) {
            QThread::msleep(80);
            QCoreApplication::processEvents();
        }
        QDir().mkpath(QStringLiteral("/tmp/apo_preview"));
        window.grab().save(QStringLiteral("/tmp/apo_preview/overview.png"));
        window.selectPrimary(2);
        for (int i = 0; i < 5; ++i) {
            QThread::msleep(60);
            QCoreApplication::processEvents();
        }
        window.grab().save(QStringLiteral("/tmp/apo_preview/config.png"));
        return 0;
    }

    return app.exec();
}
