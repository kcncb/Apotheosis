#include "runtime/live_tune.h"

#include "Apotheosis.h"
#include "config/config.h"
#include "runtime/config_snapshot.h"
#include "qt_ui/config/config_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QString>

#include <cstdio>
#include <string>

namespace live_tune
{
namespace
{
QTimer* g_timer = nullptr;
bool g_enabled_once = false;     // 是否曾经成功应用过
qint64 g_applied_write_time = 0; // 上次应用的 live_tune.ini 写时间 (ms since epoch)
qint64 g_applied_size = -1;
int g_seq = 0;
// 【拒绝去重】: 失败时故意不更新水位(这样脚本写到一半能被重试), 但代价是同一个
// 坏文件每 200ms 都会被重新报一次 rejected —— 实测把回执刷成了几百行, 真正有用
// 的 "applied" 被埋掉了。所以单独记一个"上次拒绝的内容指纹", 内容没变就不重复报。
qint64 g_rejected_write_time = -1;
qint64 g_rejected_size = -1;

// 配置目录。
//
// 【为什么不读 config.config_path】: 它是 private (见 config.h:527), 没有公开
// 访问器。为了这个通道去给 Config 加一个 getter, 等于为了调试工具扩大生产类的
// 接口 —— 不划算。而且 config_path 在运行期会被"方案切换"重定向, 让调参通道
// 跟着它到处跑反而更容易出错。
//
// 直接按已知布局算: 程序从 config.ini 启动, 方案文件在 configs/ 子目录。
// 调参文件就放在【方案目录】旁边 —— 这正是 build\cuda\Release\configs\。
QString configDir()
{
    // 优先: 可执行文件同级的 configs/ (方案目录, 平时真正在用的那个)。
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString profiles = exeDir + "/configs";
    if (QDir(profiles).exists())
        return profiles;

    // 回退: 当前工作目录。便于从别处启动时也能用。
    return QDir::current().absolutePath();
}

QString sentinelPath() { return configDir() + "/live_tune.enable"; }
QString livePath()     { return configDir() + "/live_tune.ini"; }
QString ackPath()      { return configDir() + "/live_tune_ack.txt"; }

void writeAck(const QString& status, int seq, const QString& detail)
{
    QFile f(ackPath());
    // 回执用 Append 而不是重写: 保留历史, 脚本可以只看最后一行。
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QString("%1 seq=%2 status=%3 %4\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(seq).arg(status).arg(detail);
    f.write(line.toUtf8());
}

// 应用 live_tune.ini。返回 true 表示这个文件被吃进去了。
// suppress_report = true 时, 失败【不写 ack】—— 用于抑制同一坏内容的重复告警。
bool applyLiveFile(const QString& path, bool suppress_report)
{
    const auto reject = [&](const QString& why) {
        if (!suppress_report)
            writeAck("rejected", g_seq, why);
        return false;
    };

    // ── 步骤 0: 内容体检 ────────────────────────────────────────────────────
    //
    // 【为什么必须有这一步】实测踩到的坑: 往 live_tune.ini 里写一行 "this is not
    // an ini [[[" , loadConfig() 会【成功返回 true】—— CSimpleIniA 把它当成一个
    // 没有任何 section 的空文件, 于是每一个键都落到默认值上。结果就是 kp 从 150
    // 悄悄变成 20, 而回执里写的是 "applied"。
    //
    // 这在快扫参时是致命的: 脚本正好在写文件(非原子写)时被读到半截, 参数就会被
    // 重置成默认值, 而你以为正在测的是刚写进去的那组值。
    //
    // 判据用【关键键必须出现】而不是"有没有 [hotkey.0] section" —— 后者挡不住
    // 只写了 section 头、正文还没写完的半截文件(那个也会解析成功并落到默认值)。
    // 要求这些键出现, 半截文件就不可能通过。
    //
    // ★ 2026-09-17: 这里原来还检查 pidf_kp_x / pidf_kp_y / pidf_predict_min_w /
    //   pidf_predict_max_w 四个键。它们已随瞄准控制链删除, 所以判据收缩成
    //   [hotkey.0] 一条。★ 这一条比原来弱 —— 通道现在能挡住的坏文件类型更少了。
    //   如果这个通道将来重新被用来扫参, 应该按【当时活着的键】补回同样强度的判据。
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        const QByteArray head = f.read(256 * 1024);   // 配置不会超过 256KB
        f.close();
        for (const char* key : {"[hotkey.0]"})
        {
            if (!head.contains(key))
            {
                return reject(QString("missing '%1' (truncated or not a config?)").arg(key));
            }
        }
    }

    // ── 步骤 1: 试解析 ──────────────────────────────────────────────────────
    // 用一个临时 Config 试着解析, 成功了才动真格的 —— 直接 loadConfig 失败会把
    // config_path 挪到坏文件上 (见 config_profiles.cpp 里那段注释)。
    Config probe;
    if (!probe.loadConfig(path.toStdString()))
        return false;

    // ── 步骤 2: 再体检一次"解析出来的东西像不像话" ──────────────────────────
    // ★ 2026-09-17: 这里原本还会拒绝 "insane Kp" 与 "insane predict width window"
    //   —— 那两条检查的都是瞄准参数, 而那些槽位已经没有消费者了。
    //   现在只剩"解析出来的东西里到底有没有档位"这一条结构判据。
    if (probe.hotkeys.empty())
    {
        return reject("no hotkey profiles parsed");
    }

    // ── 步骤 3: 验证通过, 走【和 Qt 界面完全相同】的那条重载路径 ────────────
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);

        // ★★★ 先把 config_path 救回来, 再解析。这一步【不是防御性编程, 是修一个
        //     实测事故】, 见下面这段说明。 ★★★
        //
        //   `loadConfig()` 的第一件事是 `config_path = <传进去的路径>`
        //   (见 config.cpp 的 setConfigPath 调用)。也就是说: 只要调参通道应用了
        //   `live_tune.ini`, **落盘目标就被重定向到 live_tune.ini 了**。
        //
        //   而 ConfigBridge 是靠 `saveConfig()` 落盘的, 它有一条规则:
        //       if (target == "config.ini" && !config_path.empty())
        //           target = config_path;
        //   于是从那以后, 界面上任何一次修改(以及退出时的 flush)**都写进
        //   live_tune.ini**, 而不再写用户当前激活的方案文件。
        //
        //   实测症状 (2026-09-15 用户报"自动调参不好用, 体感根本不明显"):
        //       active.txt -> CF           (生效方案 = configs/CF.ini)
        //       CF.ini        kp=15.0 ki=5.0 kd=0.010     <- 用户手改的, 再没被读过
        //       live_tune.ini kp=6.2  ki=3.3 kd=0.024     <- 实际在跑的
        //   调参写一次, 落盘目标就永久留在了 live_tune.ini 上; 用户之后无论怎么改
        //   CF.ini 都不会生效, 而"参数已应用"的提示仍然照常显示 —— 这正是
        
        //
        //   ★ 修法: 在 loadConfig() 之前把 config_path 记下来, 解析完立刻恢复。
        //     这样"读一份副本 → 应用"就不再有重定向落盘目标的副作用。
        const std::string saved_path = config.configPath();
        const bool loaded = config.loadConfig(path.toStdString());
        config.setConfigPath(saved_path);
        if (!loaded)
            return false;   // 理论上到不了: 上面刚验证过
    }

    // 运行时读的是不可变快照, 换配置后必须重新发布, 否则鼠标/采集线程还在跑旧值。
    runtime_config::publish();

    // 把新配置推回 Qt 侧缓存, 这样界面上的输入框会跟着变, 用户能看见脚本改了什么。
    ConfigBridge::instance().syncFromRuntime();

    return true;
}

void poll()
{
    // ── 哨兵: 不存在就直接返回。这是"默认关闭"的实现。
    if (!QFile::exists(sentinelPath()))
    {
        if (g_enabled_once)
        {
            // 哨兵被删掉了 → 停用, 并留一条回执让人知道已经关了。
            g_enabled_once = false;
            writeAck("disabled", g_seq, "sentinel removed");
        }
        return;
    }

    const QString live = livePath();
    QFileInfo fi(live);
    if (!fi.exists())
        return;

    // ── 便宜的变更检测: 写时间 + 大小都相同就跳过。
    //    注意: 只用写时间不够 —— 某些文件系统写时间精度只有 1~2 秒, 快扫参时
    //    会在同一秒内改好几次, 那样就漏了。加上大小做第二判据。
    const qint64 wt = fi.lastModified().toMSecsSinceEpoch();
    const qint64 sz = fi.size();
    if (wt == g_applied_write_time && sz == g_applied_size)
        return;

    // ── 变更了。但如果解析失败, 【不更新】水位 —— 这样下一拍会再试一次。
    //    这是有意的: 脚本可能正好在写文件(非原子的写会短暂出现半截内容)。
    //    如果那次尝试恰好落在半截上, 保持水位不变就能在下一拍读到完整内容。
    if (!fi.exists() || fi.size() == 0)
        return;

    // 同一个坏内容只报一次(见 g_rejected_* 的说明)。
    const bool already_rejected =
        (wt == g_rejected_write_time && sz == g_rejected_size);

    if (applyLiveFile(live, already_rejected))
    {
        g_applied_write_time = wt;
        g_applied_size = sz;
        g_rejected_write_time = -1;   // 成功一次就把拒绝记录清掉
        g_rejected_size = -1;
        ++g_seq;
        g_enabled_once = true;

        // 回执里带上关键参数, 脚本可以直接确认"生效的到底是什么"。
        //
        // ★ 2026-09-17: 原来这里把 kp/ki/kd/pred/minw/maxw/damp/lim 十二个瞄准参数
        //   打进回执。那些槽位已随控制链删除, 所以现在回执只报【档位数量】——
        //   也就是本通道客观上还能验证的东西。
        //   这是信息量的【下降】: 脚本不再能一眼看出"生效的是哪组参数"。原因是
        //   通道原本服务的对象没了, 不是回执写错了。
        std::string detail;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            const int idx = config.hotkeys.empty() ? -1 : 0;
            if (idx >= 0)
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "profile_idx=%d profiles=%zu",
                    idx, config.hotkeys.size());
                detail = buf;
            }
        }
        writeAck("applied", g_seq, QString::fromStdString(detail));
    }
    else
    {
        // 记录这次拒绝的内容指纹, 避免每 200ms 重复刷同一行。
        g_rejected_write_time = wt;
        g_rejected_size = sz;
    }
    // 失败时【故意不更新 g_applied_* 水位】, 所以下一拍会重试: 如果失败是因为脚本
    // 正在写文件(半截内容), 重试就能在下一拍读到完整内容。
    // ack 由 applyLiveFile 内部按【具体原因】写(除非 already_rejected)。
}
} // namespace

void start(int poll_ms)
{
    if (g_timer)
        return;
    if (poll_ms <= 0)
        poll_ms = 200;

    // 【必须】在主线程建, 这样 timeout 回调就在主线程执行。
    // publish() 需要 configMutex, 而鼠标线程每拍都在 atomic_load 快照 ——
    // 放主线程就自动串行化了, 不需要引入第二把锁。
    g_timer = new QTimer(QCoreApplication::instance());
    QObject::connect(g_timer, &QTimer::timeout, [] { poll(); });
    g_timer->start(poll_ms);

    writeAck("started", 0,
             QString("poll=%1ms dir=%2").arg(poll_ms).arg(configDir()));
}

void stop()
{
    if (g_timer)
    {
        g_timer->stop();
        g_timer->deleteLater();
        g_timer = nullptr;
    }
}

void poll_now()
{
    // 与定时器回调完全相同的函数 —— 见 live_tune.h 里为什么要这个入口。
    poll();
}

bool active() { return g_enabled_once; }
} // namespace live_tune

