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
    pal.setColor(QPalette::Window, QColor("#F6F7F9"));
    pal.setColor(QPalette::WindowText, QColor("#1A1A1F"));
    pal.setColor(QPalette::Base, QColor("#FFFFFF"));
    pal.setColor(QPalette::AlternateBase, QColor("#FAFAFB"));
    pal.setColor(QPalette::Text, QColor("#1A1A1F"));
    pal.setColor(QPalette::Button, QColor("#FBFBFC"));
    pal.setColor(QPalette::ButtonText, QColor("#1A1A1F"));
    pal.setColor(QPalette::Highlight, QColor("#5E6AD2"));
    pal.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    pal.setColor(QPalette::PlaceholderText, QColor("#A1A1AA"));
    app.setPalette(pal);

    QFont appFont("PingFang SC");
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
