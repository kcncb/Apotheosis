#pragma once

#include <QObject>

class QTimer;

class ConfigBridge : public QObject {
    Q_OBJECT

public:
    static ConfigBridge& instance();

    void syncToRuntime();
    void syncFromRuntime();
    void markDirty();
    void flush();

private slots:
    void onSaveTimeout();

private:
    ConfigBridge();
    QTimer* m_saveTimer = nullptr;
};
