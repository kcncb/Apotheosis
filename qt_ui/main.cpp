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
    pal.setColor(QPalette::Window, QColor("#F4F6FA"));
    pal.setColor(QPalette::WindowText, QColor("#17191F"));
    pal.setColor(QPalette::Base, QColor("#FFFFFF"));
    pal.setColor(QPalette::AlternateBase, QColor("#FBFCFD"));
    pal.setColor(QPalette::Text, QColor("#17191F"));
    pal.setColor(QPalette::Button, QColor("#F8F9FB"));
    pal.setColor(QPalette::ButtonText, QColor("#17191F"));
    pal.setColor(QPalette::ToolTipBase, QColor("#17191F"));
    pal.setColor(QPalette::ToolTipText, QColor("#FFFFFF"));
    pal.setColor(QPalette::PlaceholderText, QColor("#98A1B0"));
    pal.setColor(QPalette::Highlight, QColor("#5865D8"));
    pal.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#B8C0CC"));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#B8C0CC"));
    app.setPalette(pal);

    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI Variable"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Segoe UI")});
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

    // 单机自用：跳过登录对话框
    MainWindow window;
    window.resize(960, 640);
    window.show();

    if (shotMode) {
        QDir().mkpath("/tmp/apo_shots");
        const std::vector<std::tuple<int, int, QString>> shots = {
            {0, 0, "00_overview"},
            {1, 0, "01_session"},
            {1, 1, "02_modeltools"},
            {2, 0, "03_capture"},
            {2, 3, "04_aimodel"},
            {3, 0, "05_hotkey"},
            {4, 0, "06_stats"},
            {4, 1, "07_log"},
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
