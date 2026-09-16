#include "config/config_profiles.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <atomic>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "runtime/config_snapshot.h"
#include "runtime/inference_session.h"

extern std::atomic<bool> detector_model_changed;

namespace
{

// 方案目录与引导文件名。工作目录此时已经切到 exe 旁 (见 Apotheosis.cpp),
// 所以 "config.ini" / "configs" 都落在程序旁边。
constexpr const char* kProfilesDirName   = "configs";
constexpr const char* kProfileSuffix     = ".ini";
constexpr const char* kCurveSuffix       = ".curves";
constexpr const char* kActiveMarkerName  = "active.txt";
constexpr int         kMaxNameLength     = 48;

// ★ 调参通道的文件【不是方案】(2026-09-15 修)。
//
//   live_tune.ini 与方案文件同住 configs/, 方案列表枚举 *.ini 时会把它当成
//   一个名叫 "live_tune" 的方案。后果实测发生过: 用户看到列表里多了一项,
//   点它 -> active.txt 变成 'live_tune' -> 从此"切方案/保存"都作用在调参
//   通道的文件上, 与真正的方案互相覆盖 —— 又是一次"两份真相"。
//   方案列表必须显式排除它(以及未来任何调参通道的内部文件)。
QStringList internalNonProfileNames()
{
    return { QStringLiteral("live_tune") };
}

QString g_defaultProfileName()
{
    return QString::fromUtf8(u8"默认");
}

QString curveDirFor(const QString& iniPath)
{
    const QFileInfo info(iniPath);
    return info.dir().filePath(info.completeBaseName() + QString::fromLatin1(kCurveSuffix));
}

}  // namespace

ConfigProfiles::ConfigProfiles() : QObject(nullptr) {}

ConfigProfiles& ConfigProfiles::instance()
{
    static ConfigProfiles s;
    return s;
}

QString ConfigProfiles::directory() const
{
    return QDir(QDir::currentPath()).filePath(QString::fromLatin1(kProfilesDirName));
}

QString ConfigProfiles::profileFilePath(const QString& name) const
{
    return QDir(directory()).filePath(name + QString::fromLatin1(kProfileSuffix));
}

QString ConfigProfiles::profileCurveDir(const QString& name) const
{
    return QDir(directory()).filePath(name + QString::fromLatin1(kCurveSuffix));
}

QString ConfigProfiles::activeMarkerPath() const
{
    return QDir(directory()).filePath(QString::fromLatin1(kActiveMarkerName));
}

QString ConfigProfiles::sanitizeName(const QString& raw)
{
    QString name = raw.trimmed();
    if (name.isEmpty())
        return {};
    if (name.size() > kMaxNameLength)
        name = name.left(kMaxNameLength).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return {};

    // Windows 文件名里非法或会让用户困惑的字符; 另外挡掉结尾的 '.' / '空格'
    // (Win32 会静默吃掉, 导致"保存成功了但列表里找不到")。
    static const QString kBad = QStringLiteral("\\/:*?\"<>|");
    for (const QChar ch : name) {
        if (kBad.contains(ch) || ch < QChar(0x20) || ch == QChar(0x7F))
            return {};
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return {};
    return name;
}

QStringList ConfigProfiles::names() const
{
    QStringList out;
    QDir dir(directory());
    if (!dir.exists())
        return out;

    const QStringList internal = internalNonProfileNames();
    const QStringList filters{QStringLiteral("*") + QString::fromLatin1(kProfileSuffix)};
    const auto files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const auto& info : files) {
        if (internal.contains(info.completeBaseName()))
            continue;   // 调参通道的文件不是方案
        out << info.completeBaseName();
    }
    return out;
}

QList<ConfigProfiles::Entry> ConfigProfiles::entries() const
{
    QList<Entry> out;
    QDir dir(directory());
    if (!dir.exists())
        return out;

    const QStringList internal = internalNonProfileNames();
    const QStringList filters{QStringLiteral("*") + QString::fromLatin1(kProfileSuffix)};
    const auto files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const auto& info : files) {
        if (internal.contains(info.completeBaseName()))
            continue;   // 调参通道的文件不是方案
        Entry e;
        e.name     = info.completeBaseName();
        e.filePath = info.absoluteFilePath();
        e.bytes    = info.size();
        e.modified = info.lastModified();
        e.active   = (e.name == m_active);
        out << e;
    }
    return out;
}

bool ConfigProfiles::exists(const QString& name) const
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty())
        return false;
    return QFileInfo::exists(profileFilePath(clean));
}

QString ConfigProfiles::activeName() const
{
    return m_active;
}

QString ConfigProfiles::activeFilePath() const
{
    // 没有活动方案时, 生效配置的落盘目标就是引导文件 config.ini
    // (与 flushCurrent() 的回落规则保持一致 —— 两处必须给出同一个答案)。
    if (m_active.isEmpty())
        return QDir(QDir::currentPath()).filePath(QStringLiteral("config.ini"));
    return profileFilePath(m_active);
}

bool ConfigProfiles::hasActive() const
{
    return !m_active.isEmpty();
}

QString ConfigProfiles::readActiveMarker() const
{
    QFile file(activeMarkerPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString text = QString::fromUtf8(file.readAll()).trimmed();
    return sanitizeName(text);
}

void ConfigProfiles::writeActiveMarker(const QString& name) const
{
    QDir().mkpath(directory());
    QFile file(activeMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    file.write(name.toUtf8());
    file.close();
}

void ConfigProfiles::initialize()
{
    QDir().mkpath(directory());

    // 一份方案都没有: 把当前生效配置 (config.ini 或它的默认值) 另存成「默认」方案。
    if (names().isEmpty()) {
        const QString path = profileFilePath(g_defaultProfileName());
        bool ok = false;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            ok = config.saveConfig(path.toStdString());
        }
        if (!ok) {
            const QString msg = QString::fromUtf8(u8"无法创建默认配置方案: %1")
                                    .arg(QDir::toNativeSeparators(path));
            emit operationFailed(msg);
        } else {
            qInfo("[Profiles] Created default profile: %s",
                  qUtf8Printable(QDir::toNativeSeparators(path)));
        }
    }

    QString active = readActiveMarker();
    const QStringList all = names();
    if (active.isEmpty() || !all.contains(active)) {
        active = all.contains(g_defaultProfileName()) ? g_defaultProfileName()
                                                      : all.value(0);
    }

    m_active = active;
    if (!m_active.isEmpty()) {
        writeActiveMarker(m_active);
        QString error;
        // 内容与 config.ini 一致 (或本来就是从它生成的), 不要重复落盘。
        if (!applyProfileFile(profileFilePath(m_active), /*autoSaveCurrent=*/false, &error)) {
            emit operationFailed(QString::fromUtf8(u8"应用配置方案失败: %1").arg(error));
        }
    }

    emit profilesChanged();
}

void ConfigProfiles::refresh()
{
    const QStringList all = names();
    if (!m_active.isEmpty() && !all.contains(m_active)) {
        // 当前方案文件在外部被删掉/改名了。内存里的配置仍是真相, 但落盘目标
        // 已经不存在 —— 把目标挪回 config.ini, 免得下一次自动保存把用户刚删掉
        // 的文件凭空写回来; 同时清掉选择, 让用户显式重选一个方案。
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            config.retargetConfigPath("config.ini");
        }
        m_active.clear();
        QFile::remove(activeMarkerPath());
    }
    emit profilesChanged();
}

bool ConfigProfiles::flushCurrent(QString* error)
{
    // 即使还没有活动方案也要落盘: 此时落盘目标就是 config.ini, 写下去没有副作用,
    // 却能让「切换前不丢改动」这条规则永远成立。
    // 先停掉防抖计时器并立刻落盘 —— 此时 config_path 仍指向旧方案,
    // 所以用户刚改的东西写回的是旧方案, 不会串到新方案里。
    ConfigBridge::instance().flush();

    const QString target = m_active.isEmpty()
                               ? QString::fromLatin1("config.ini")
                               : QDir::toNativeSeparators(profileFilePath(m_active));
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (!config.saveConfig()) {
        if (error)
            *error = QString::fromUtf8(u8"当前方案写入失败: %1").arg(target);
        return false;
    }
    return true;
}

bool ConfigProfiles::applyProfileFile(const QString& targetPath, bool autoSaveCurrent,
                                      QString* error)
{
    if (!QFileInfo::exists(targetPath)) {
        if (error)
            *error = QString::fromUtf8(u8"方案文件不存在: %1")
                         .arg(QDir::toNativeSeparators(targetPath));
        return false;
    }

    if (autoSaveCurrent && !flushCurrent(error))
        return false;

    std::string oldModel;
    std::string oldInput;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        oldModel = config.ai_model;
        oldInput = config.input_method;
        if (!config.loadConfig(targetPath.toStdString())) {
            // loadConfig() 是先把 config_path 指过去再解析的: 解析失败时目标
            // 已经落在坏文件上了, 必须挪回来 —— 否则下一次自动保存会把当前
            // 内存里的配置(旧方案的)盖到那个坏文件上。
            config.retargetConfigPath("config.ini");
            if (error)
                *error = QString::fromUtf8(u8"方案解析失败: %1")
                             .arg(QDir::toNativeSeparators(targetPath));
            return false;
        }
    }

    // 运行时读的是不可变快照, 换配置后必须重新发布, 否则鼠标/采集线程还在跑旧值。
    runtime_config::publish();

    // 把新配置推回 Qt 侧缓存 (内部 QSignalBlocker, 不会反过来触发回写)。
    ConfigBridge::instance().syncFromRuntime();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (config.ai_model != oldModel) {
            detector_model_changed = true;
            runtime::preload_model_metadata("models/" + config.ai_model, false);
        }
        if (config.input_method != oldInput)
            input_method_changed = true;
    }

    // 让所有页面按新值重读一遍控件 (各页连接的是 configLoaded)。
    ConfigManager::instance().notifyRuntimeReloaded();

    qInfo("[Profiles] Applied profile: %s", qUtf8Printable(QDir::toNativeSeparators(targetPath)));
    emit configApplied();
    return true;
}

bool ConfigProfiles::switchTo(const QString& name, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty()) {
        if (error) *error = QString::fromUtf8(u8"方案名无效。");
        return false;
    }
    if (!QFileInfo::exists(profileFilePath(clean))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(clean);
        return false;
    }
    if (clean == m_active) {
        emit profilesChanged();
        return true;
    }

    if (!applyProfileFile(profileFilePath(clean), /*autoSaveCurrent=*/true, error))
        return false;

    m_active = clean;
    writeActiveMarker(clean);
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::saveCurrent(QString* error)
{
    if (m_active.isEmpty())
        return saveAs(g_defaultProfileName(), /*overwrite=*/true, error);

    ConfigBridge::instance().flush();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (!config.saveConfig()) {
            if (error) *error = QString::fromUtf8(u8"写入配置方案失败: %1")
                                    .arg(QDir::toNativeSeparators(profileFilePath(m_active)));
            return false;
        }
    }
    writeActiveMarker(m_active);
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::saveAs(const QString& name, bool overwrite, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty()) {
        if (error)
            *error = QString::fromUtf8(u8"方案名不能为空, 且不能包含 \\ / : * ? \" < > | 等字符。");
        return false;
    }
    if (clean == m_active)
        return saveCurrent(error);

    const QString path = profileFilePath(clean);
    if (QFileInfo::exists(path) && !overwrite) {
        if (error) *error = QString::fromUtf8(u8"已存在同名配置方案「%1」。").arg(clean);
        return false;
    }

    // 旧的当前方案先落盘, 免得"另存为"把用户没保存的改动弄丢。
    ConfigBridge::instance().flush();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (!config.saveConfig(path.toStdString())) {
            if (error) *error = QString::fromUtf8(u8"无法写入配置方案: %1")
                                    .arg(QDir::toNativeSeparators(path));
            return false;
        }
    }

    m_active = clean;
    writeActiveMarker(clean);

    // saveConfig() 不会改 config_path, 必须重读一次把后续保存指向新方案。
    if (!applyProfileFile(path, /*autoSaveCurrent=*/false, error))
        return false;

    emit profilesChanged();
    return true;
}

bool ConfigProfiles::renameProfile(const QString& from, const QString& to, QString* error)
{
    const QString source = sanitizeName(from);
    const QString target = sanitizeName(to);
    if (source.isEmpty() || target.isEmpty()) {
        if (error) *error = QString::fromUtf8(u8"方案名无效。");
        return false;
    }
    if (source == target) {
        emit profilesChanged();
        return true;
    }
    if (!QFileInfo::exists(profileFilePath(source))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(source);
        return false;
    }
    if (QFileInfo::exists(profileFilePath(target))) {
        if (error) *error = QString::fromUtf8(u8"已存在同名配置方案「%1」。").arg(target);
        return false;
    }

    // 改名的是当前方案时先把内存改动落盘, 否则会丢。
    if (source == m_active)
        flushCurrent(error);

    if (!QFile::rename(profileFilePath(source), profileFilePath(target))) {
        if (error) *error = QString::fromUtf8(u8"重命名失败, 请检查该文件是否被占用。");
        return false;
    }
    // 曲线资产目录跟着一起改名, 否则方案加载后曲线会丢。
    const QString oldCurves = profileCurveDir(source);
    if (QFileInfo::exists(oldCurves))
        QDir().rename(oldCurves, profileCurveDir(target));

    if (source == m_active) {
        m_active = target;
        writeActiveMarker(target);
        if (!applyProfileFile(profileFilePath(target), /*autoSaveCurrent=*/false, error))
            return false;
    }

    emit profilesChanged();
    return true;
}

bool ConfigProfiles::remove(const QString& name, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty() || !QFileInfo::exists(profileFilePath(clean))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(name);
        return false;
    }

    const QStringList all = names();
    if (all.size() <= 1) {
        if (error)
            *error = QString::fromUtf8(u8"至少要保留一个配置方案 —— 否则当前配置没有落盘目标。");
        return false;
    }

    if (clean == m_active) {
        // 先切到别的方案: 不做 autoSave, 否则会把刚删掉的文件又写回来。
        QString fallback;
        for (const QString& n : all) {
            if (n != clean) { fallback = n; break; }
        }
        if (!applyProfileFile(profileFilePath(fallback), /*autoSaveCurrent=*/false, error))
            return false;
        m_active = fallback;
        writeActiveMarker(fallback);
    }

    QFile::remove(profileFilePath(clean));
    QDir(profileCurveDir(clean)).removeRecursively();

    qInfo("[Profiles] Removed profile: %s", qUtf8Printable(clean));
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::openDirectory() const
{
    const QString dir = directory();
    QDir().mkpath(dir);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
