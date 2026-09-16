#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// ─────────────────────────────────────────────────────────────────────────────
// 全局配置方案 (profile) 管理
//
// 一个「方案」就是一份自包含的 ini 快照, 存放在 exe 同目录的 configs/ 下:
//
//     configs/<名称>.ini          完整配置 (等价于一份 config.ini)
//     configs/<名称>.curves/      该方案自己的轨迹曲线二进制资产 (有才存在)
//
// 运行中的「生效配置」永远是**当前方案文件本身**:
// Config::loadConfig(方案路径) 会把 Config::config_path 指到那份文件, 而
// Config::saveConfig() 在 filename=="config.ini" 时会重定向到 config_path
// (config.cpp:768), 所以既有所有 `config.saveConfig("config.ini")` 调用点
// (Apotheosis.cpp / inference_session.cpp) 不用改一行就会写回当前方案。
//
// exe 同目录的 config.ini 因此退化为: ①首次运行生成默认值的引导文件,
// ②还没建立任何方案时的落盘位置。建立方案后它不再被写入, 也不会被删掉。
//
// 切换方案 = 先把当前内存里的改动 flush 回旧方案, 再 loadConfig(新方案),
// 重新发布运行时快照, 然后让界面重新读一遍值。
// ─────────────────────────────────────────────────────────────────────────────
class ConfigProfiles : public QObject {
    Q_OBJECT

public:
    struct Entry {
        QString   name;
        QString   filePath;
        qint64    bytes = 0;
        QDateTime modified;
        bool      active = false;
    };

    static ConfigProfiles& instance();

    // 扫描 configs/, 必要时创建目录; 一份方案都没有时把当前生效配置另存为
    // 「默认」方案。最后把 active.txt 里记着的方案设为生效方案。
    // 必须在 ConfigManager::load() / ConfigBridge::syncFromRuntime() 之后调用。
    void initialize();
    void refresh();

    QString     directory() const;
    QString     activeName() const;
    bool        hasActive() const;

    // 当前生效方案的完整路径 (没有活动方案时回落到 exe 旁的 config.ini)。
    // ★ 为什么要把这个暴露出来: 生效配置的"落盘目标"就是这份文件, 而调参
    //   agent 写回参数时必须写到【用户正在用的那一份】—— 写到一个旁路文件
    //   (如 live_tune.ini) 就会产生两份真相。历史上正是这么翻车的:
    //   active.txt 指向 CF, 而调参把值写进了 live_tune.ini, 于是用户看到的
    //   参数(CF.ini)和实际在跑的(live_tune.ini)完全不是一回事。
    QString     activeFilePath() const;
    QList<Entry> entries() const;
    QStringList names() const;

    // 一键切换
    bool switchTo(const QString& name, QString* error = nullptr);

    // 一键保存: 当前内存配置写回当前方案 (含曲线资产)。
    bool saveCurrent(QString* error = nullptr);

    // 另存为 / 新建: 当前配置写到新方案并立刻切过去。
    bool saveAs(const QString& name, bool overwrite, QString* error = nullptr);

    bool renameProfile(const QString& from, const QString& to, QString* error = nullptr);

    // 一键删除。最后一个方案不允许删 (否则生效配置没有落盘目标)。
    bool remove(const QString& name, QString* error = nullptr);

    bool openDirectory() const;

    // 过滤非法字符与保留名; 返回空串表示这个名字不可用。
    static QString sanitizeName(const QString& raw);
    // 该方案名是否已存在。
    bool exists(const QString& name) const;

signals:
    // 方案列表/活动方案发生变化 (增删改名切换后)。UI 重新填下拉用。
    void profilesChanged();
    // 生效配置被整体换掉 (切换/另存为后)。界面页据此重读控件。
    void configApplied();
    void operationFailed(const QString& message);

private:
    ConfigProfiles();
    ConfigProfiles(const ConfigProfiles&) = delete;
    ConfigProfiles& operator=(const ConfigProfiles&) = delete;

    QString profileFilePath(const QString& name) const;
    QString profileCurveDir(const QString& name) const;
    QString activeMarkerPath() const;

    void     writeActiveMarker(const QString& name) const;
    QString  readActiveMarker() const;

    // 把 config_path 指到 targetPath 并重读一遍配置 + 重新发布快照。
    // skipAutoSave 为真时不动当前方案 (删除活动方案时用)。
    bool applyProfileFile(const QString& targetPath, bool autoSaveCurrent,
                          QString* error = nullptr);

    // 切换到 targetPath 之前把内存改动写回旧方案。
    bool flushCurrent(QString* error);

    QString m_active;
};
