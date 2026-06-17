#pragma once

#include <QWidget>

class QLabel;

class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);

    void setInferenceStatus(bool running);
    void setFps(double fps);
    void setBackend(const QString& backend);
    void setConfigPath(const QString& path);

private:
    QLabel* m_statusDot{};
    QLabel* m_statusLabel{};
    QLabel* m_fpsLabel{};
    QLabel* m_backendLabel{};
    QLabel* m_configPathLabel{};
};
