#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QThread>

#include <vector>

#include "MainWindow.h"
#include "widgets/IconFont.h"
#include "widgets/LoginDialog.h"

static void applyLightPalette(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#F6F6F8"));
    pal.setColor(QPalette::WindowText, QColor("#1A1A1F"));
    pal.setColor(QPalette::Base, QColor("#FFFFFF"));
    pal.setColor(QPalette::AlternateBase, QColor("#FAFAFB"));
    pal.setColor(QPalette::Text, QColor("#1A1A1F"));
    pal.setColor(QPalette::Button, QColor("#FBFBFC"));
    pal.setColor(QPalette::ButtonText, QColor("#1A1A1F"));
    pal.setColor(QPalette::ToolTipBase, QColor("#1A1A1F"));
    pal.setColor(QPalette::ToolTipText, QColor("#FFFFFF"));
    pal.setColor(QPalette::PlaceholderText, QColor("#A1A1AA"));
    pal.setColor(QPalette::Highlight, QColor("#5E6AD2"));
    pal.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#C2C2CA"));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#C2C2CA"));
    app.setPalette(pal);

    QFont appFont("PingFang SC");
    appFont.setPixelSize(13);
    app.setFont(appFont);
}

static QString loadStyleSheet() {
    QFile resource(":/style/theme.qss");
    if (resource.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(resource.readAll());
    }

    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/../style/theme.qss",
        appDir + "/../../qt_ui/style/theme.qss",
        appDir + "/style/theme.qss",
        "./style/theme.qss",
        "../style/theme.qss",
    };
    for (const auto& path : candidates) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString::fromUtf8(file.readAll());
        }
    }

    return {};
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Apotheosis");
    app.setOrganizationName("Apotheosis");

    applyLightPalette(app);

    IconFont::load();

    if (auto qss = loadStyleSheet(); !qss.isEmpty()) {
        app.setStyleSheet(qss);
    }

    const bool shotMode = app.arguments().contains("--shot");

    if (!shotMode) {
        LoginDialog login;
        if (login.exec() != QDialog::Accepted)
            return 0;
    }

    MainWindow window;
    window.resize(960, 640);
    window.show();

    if (shotMode) {
        QDir().mkpath("/tmp/apo_shots");
        const std::vector<std::tuple<int, int, QString>> shots = {
            {0, 0, "01_session"},
            {0, 1, "02_modeltools"},
            {1, 0, "03_capture"},
            {1, 3, "04_aimodel"},
            {2, 0, "05_hotkey"},
            {3, 0, "06_stats"},
            {3, 1, "07_log"},
        };
        for (const auto& [a, b, name] : shots) {
            window.selectPage(a, b);
            for (int i = 0; i < 8; ++i)
                QCoreApplication::processEvents();
            QThread::msleep(150);
            QCoreApplication::processEvents();
            window.grab().save("/tmp/apo_shots/" + name + ".png");
        }
        return 0;
    }

    return app.exec();
}
