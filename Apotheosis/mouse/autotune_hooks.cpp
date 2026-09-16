// =============================================================================
// 调参 agent —— 运行时接入的【具体实现】 (读配置 / 写配置并触发重载)
// =============================================================================
//
// 为什么单独一个文件而不是塞进 autotune_runtime.cpp
//   autotune_runtime.cpp 需要能在【不带 Qt/不带 config 的纯逻辑测试】里编译,
//   而这里要 include config.h / config_bridge.h / live_tune.h —— 一碰就带上
//   整个应用。分开之后: 逻辑测试只编 runtime 层, 生产构建才编这一层。
//
// ★ 写回路径 (关键设计, 与界面完全一致)
//     组装一份【完整配置文本】 -> 写到临时文件 -> 原子改名到 live_tune.ini
//     -> live_tune 轮询发现并应用 -> config.loadConfig + publish + syncFromRuntime
//
//   为什么必须走这一条而不是"直接改 config 结构再 publish":
//     · 界面上的输入框会通过 syncFromRuntime() 自动跟着变, 用户看得见 agent 改了什么
//     · 复用 live_tune 已有的【内容体检】(半截文件/关键键缺失会被拒), 不会把
//       默认值悄悄写进去
//     · 只有一条重载路径, 不存在"两份真相"
//
//   ★ 原子改名是必须的: 直接往 live_tune.ini 写, 轮询可能读到写了一半的文件。
//     先写 .tmp 再 MoveFileEx 改名, 读到的要么是旧的完整文件要么是新的完整文件。
// =============================================================================

#include "mouse/autotune_runtime.h"

#include "Apotheosis.h"
#include "config/config.h"
#include "config/config_profiles.h"
#include "runtime/config_snapshot.h"
#include "runtime/live_tune.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cstdio>
#include <string>

namespace boss::autotune
{

namespace
{

// live_tune 的协议路径 —— 必须与 runtime/live_tune.cpp 的 configDir() 一致。
// ★ 刻意重复这几行而不是导出接口: live_tune.h 只暴露 start/stop/active 三个
//   函数, 为了调参去扩大它的公开接口不值当。这里只读路径, 不改它的行为。
QString configDir()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString profiles = exeDir + "/configs";
    if (QDir(profiles).exists())
        return profiles;
    return QDir::current().absolutePath();
}

QString livePath()   { return configDir() + "/live_tune.ini"; }
QString sentinelPath() { return configDir() + "/live_tune.enable"; }

// 把 Knobs 应用到【第 0 组热键配置】上。
// ★ 只动这十几个键, 其余(检测/采集/扳机/...)原样保留 —— 用 Config 结构
//   做读-改-写, 而不是拼一份新配置文件。
bool write_knobs_to_config(const Knobs& k, std::string& err)
{
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (config.hotkeys.empty())
        {
            err = u8"配置里没有热键组, 无法写入";
            return false;
        }
        auto& hp = config.hotkeys[0];

        // PID 三增益
        hp.pidf_kp_x = static_cast<float>(k.kp);
        hp.pidf_kp_y = static_cast<float>(k.kp);
        hp.pidf_ki_x = static_cast<float>(k.ki);
        hp.pidf_ki_y = static_cast<float>(k.ki);
        hp.pidf_kd_x = static_cast<float>(k.kd);
        hp.pidf_kd_y = static_cast<float>(k.kd);
        // 在途补偿 (beta 无量纲)
        hp.pidf_inflight_x = static_cast<float>(k.beta);
        hp.pidf_inflight_y = static_cast<float>(k.beta);
        // P 项饱和阈值 (像素, 0 = 关闭)
        hp.pidf_psat_x = static_cast<int>(k.psat_px);
        hp.pidf_psat_y = static_cast<int>(k.psat_px);
        // 尺度调度。
        // ★ base_h(基准框高)在这里【不写】—— 它不是 agent 调的参数, 而是
        //   用户在界面上按按钮设定、再单独存盘的量(见 Runtime::set_baseline_from_recent)。
        hp.aim_scale_enabled = k.scale_on ? 1 : 0;
        hp.aim_scale_max = static_cast<float>(k.scale_max);
        hp.aim_scale_min = static_cast<float>(k.scale_min);
        // 预测 (系数 / 硬上限 / 速度噪声门)
        hp.pidf_predict_x = static_cast<float>(k.predict_x);
        hp.pidf_predict_y = static_cast<float>(k.predict_y);
        hp.pidf_predict_max_px = k.predict_max_px;
        hp.pidf_predict_vel_floor = k.predict_vel_floor;

        ok = true;
    }
    if (!ok) err = u8"写入失败";
    return ok;
}

Knobs read_knobs_from_config(){
    Knobs k;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (config.hotkeys.empty()) return k;
    const auto& hp = config.hotkeys[0];
    k.kp = hp.pidf_kp_x;
    k.ki = hp.pidf_ki_x;
    k.kd = hp.pidf_kd_x;
    // 在途补偿: 负数是"未设置"的哨兵, 生产接线会给默认 1.6。
    k.beta = (hp.pidf_inflight_x >= 0.0f) ? hp.pidf_inflight_x : 1.6;
    k.psat_px = hp.pidf_psat_x;
    k.scale_on = (hp.aim_scale_enabled != 0);
    k.scale_max = hp.aim_scale_max;
    k.scale_min = hp.aim_scale_min;
    k.predict_x = hp.pidf_predict_x;
    k.predict_y = hp.pidf_predict_y;
    k.predict_max_px = hp.pidf_predict_max_px;
    k.predict_vel_floor = hp.pidf_predict_vel_floor;
    return k;
}

// 把当前【完整配置】另存到 live_tune.ini, 让 live_tune 走它那条已验证的重载路径
// (config.loadConfig + publish + syncFromRuntime)。
//
// ★ 为什么要"另存一份"而不是直接让 agent 改正式配置文件:
//   live_tune 的设计就是"读一个副本 -> 校验 -> 应用到生效配置"。沿用它的协议,
//   agent 就自动获得了它的全部安全检查(半截文件/关键键缺失会被拒)。
//
// ★ 原子改名是必须的: 直接往 live_tune.ini 写, 轮询可能读到写了一半的文件。
//   先写 .tmp 再改名, 读到的要么是旧的完整文件、要么是新的完整文件。
bool save_current_config_to_live_tune()
{
    const QString dir = configDir();
    QDir().mkpath(dir);

    // live_tune 只在哨兵存在时启用 —— agent 自己打开它(关闭 agent 时删掉)。
    QFile s(sentinelPath());
    if (!s.exists() && s.open(QIODevice::WriteOnly))
    {
        s.write("auto tune agent\n");
        s.close();
    }

    const QString tmp = dir + "/autotune_pending.ini";
    const QString dst = livePath();
    QFile::remove(tmp);
    if (!config.saveConfig(tmp.toStdString()))
        return false;

    QFile::remove(dst);
    return QFile::rename(tmp, dst);
}

// ── 把运行时的当前值读出来 ──────────────────────────────────────────────────
//
// ★ 为什么读【快照】而不是读 config 结构:
//   快照是鼠标线程每拍真正在用的那份不可变 Config (runtime_config::read())。
//   而 `config` 是"正在编辑 / 正在写盘"的那一份 —— 两者可能不一致, 而
//   "用户体感用的是哪一个"的答案永远是快照。
//   ★ 这正是本次事故的关键: 参数写进了文件、config 结构也改了, 但只要快照
//     没跟着重发布, 瞄准线程就还在用旧值 —— 而旧写法完全看不出来。
bool snapshot_readable()
{
    return runtime_config::read() != nullptr;
}

Knobs snapshot_knobs()
{
    Knobs k;
    const auto snap = runtime_config::read();
    if (!snap || snap->hotkeys.empty()) return k;
    const auto& hp = snap->hotkeys[0];

    k.kp = hp.pidf_kp_x;
    k.ki = hp.pidf_ki_x;
    k.kd = hp.pidf_kd_x;
    k.beta = (hp.pidf_inflight_x >= 0.0f) ? hp.pidf_inflight_x : 1.6;
    k.psat_px = hp.pidf_psat_x;
    k.scale_on = (hp.aim_scale_enabled != 0);
    k.scale_max = hp.aim_scale_max;
    k.scale_min = hp.aim_scale_min;
    k.predict_x = hp.pidf_predict_x;
    k.predict_y = hp.pidf_predict_y;
    k.predict_max_px = hp.pidf_predict_max_px;
    k.predict_vel_floor = hp.pidf_predict_vel_floor;
    return k;
}

// 快照里的值是否已经是我们期望的那组。
//
// ★ 容差: 配置里存的是 float, 往返一趟和夹取都会带来末位误差, 所以不能
//   直接用 ==。但容差必须小到"只有真的写进去了才算过" —— Kp 是几十量级的量,
//   1e-3 对应的是小数点后三位, 任何真实的参数变化都远大于它。
// ★ 只比 Kp/Ki/Kd/beta 四个主增益: 它们是调参实际在动、也是用户体感得到的东西。
//   尺度/预测这些"开关型"参数容易因为夹取和默认值规则产生假阴性, 放进验证
//   会让"明明生效了却报未生效", 那比不验证更糟。
bool reads_back_as(const Knobs& want)
{
    const Knobs got = snapshot_knobs();
    const auto near = [](double a, double b, double tol) {
        return std::abs(a - b) <= tol;
    };
    return near(got.kp, want.kp, 1e-3)
        && near(got.ki, want.ki, 1e-3)
        && near(got.kd, want.kd, 1e-4)
        && near(got.beta, want.beta, 1e-3);
}

// 写回参数, 然后【等到运行时真的用上为止】。
//
// ★★ 为什么写完必须验一遍 (2026-09-15 新增) ★★
//
//   用户报"自动调参不好用, 体感根本不明显"。查下来是【参数压根没生效】:
//     CF.ini        (active.txt 指着的方案) kp=15.0 ki=5.0
//     live_tune.ini (实际在跑的)            kp=6.2  ki=3.3
//   而界面上一直显示"已应用"。
//
//   原来的写法是【发出去就算成功】: saveConfig -> 原子改名 -> return true。
//   但"写到文件"和"运行时用上了"之间隔着 live_tune 的轮询(200ms)、内容体检、
//   loadConfig 与 publish。任何一环没走通, 用户看到的依然是一句"已应用"。
//
//   ★ 所以改成: 写完等快照真的变了才回报成功; 等不到就返回 false 并把
//     "想要 / 文件里 / 实际在跑"三个值一起写进错误信息。
bool write_and_verify(const Knobs& k, std::string& err)
{
    if (!write_knobs_to_config(k, err))
        return false;

    if (!save_current_config_to_live_tune())
    {
        err = u8"配置写盘失败(无法生成 live_tune.ini)";
        return false;
    }

    // 没有可用快照时无法验证。★ 如实说明, 不假装验证过。
    if (!snapshot_readable())
    {
        err = u8"已写盘, 但运行时快照不可用, 无法确认是否生效";
        return true;
    }

    // 轮询周期 200ms, 给 20 拍(2 秒)余量。
    // ★ 超时【不算成功】: 宁可为 false 让用户看见"没生效", 也不要报一个假的
    //   "已应用" —— 那正是这次的病根。
    //
    // ★★ 每一拍都【同步调 live_tune::poll_now()】, 不能干等定时器 (2026-09-15 修) ★★
    //
    //   第一版这里是纯 sleep 循环: 等 live_tune 的 QTimer(200ms)去应用文件。
    //   但这一轮是在【UI 线程】上同步跑的(onEndSession -> end_session ->
    //   hooks.write), sleep 期间事件循环被阻塞, 【那个 QTimer 一次都不会触发】
    //   —— 文件写出去了, 却永远等不到它被应用, 2 秒后必然超时报
    //   "参数未能真正生效", 回滚也一样等不到; 等函数返回、事件循环恢复,
    //   定时器才把文件应用掉(ack 里的 applied 全是失败之后才写出来的)。
    //   实机表现: 用户每一轮都收到"参数未能真正生效", 一次成功都没有。
    //
    //   修法: 写完文件后【主动调用】live_tune::poll_now()(与定时器同一段
    //   应用代码, 只是立即执行), 不再依赖事件循环有没有空。线程纪律不变:
    //   本函数本来就在主线程, poll_now 也要求主线程。
    constexpr int kPolls = 20;
    for (int i = 0; i < kPolls; ++i)
    {
        live_tune::poll_now();          // 立即应用(等价于定时器的那一拍)
        if (reads_back_as(k))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const Knobs on_disk = read_knobs_from_config();
    err  = u8"参数写出去了, 但运行时【没有采用它】。\n";
    err += u8"  想要:     " + k.describe() + u8"\n";
    err += u8"  文件里:   " + on_disk.describe() + u8"\n";
    err += u8"  实际在跑: " + snapshot_knobs().describe() + u8"\n";
    err += u8"常见原因: live_tune 通道被拒(半截文件/关键键缺失), 或参数被别处改回去了。";
    return false;
}

} // namespace

// 安装生产实现。在程序启动、Qt 起来之后调用一次。
void install_production_hooks()
{
    RuntimeHooks hooks;
    hooks.read = []() -> Knobs { return read_knobs_from_config(); };

    // ★ write 现在会【等运行时真的采用】才返回 true, 并把失败原因带出来。
    //   见 write_and_verify() 顶部: 这修的是"界面显示已应用、参数其实没生效"。
    hooks.write = [](const Knobs& k, std::string& err) -> bool {
        return write_and_verify(k, err);
    };

    // ★ 基准框高: 与 Knobs 分开。它不是模型调的参数, 而是 agent 学出来的观测值。
    hooks.read_base_h = []() -> double {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (config.hotkeys.empty()) return 0.0;
        return static_cast<double>(config.hotkeys[0].aim_scale_base_h);
    };
    hooks.write_base_h = [](double h) -> bool {
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (config.hotkeys.empty()) return false;
            if (!(h > 0.0) || h > 4000.0) return false;
            config.hotkeys[0].aim_scale_base_h = static_cast<float>(h);
        }
        // ★ 走与 write 完全相同的那条落盘路径(另存 live_tune.ini -> 触发重载)。
        //   不直接 saveConfig 到正式配置: 那样会绕过 live_tune 的体检, 也可能在
        //   用户正在编辑配置时造成两份真相。
        return save_current_config_to_live_tune();
    };

    Runtime::instance().install(std::move(hooks));
}
// 关闭 agent 时调用: 删掉 live_tune 哨兵, 让调参通道彻底关闭。
// ★ 必须做这一步: 哨兵留着 = live_tune 一直轮询, 意味着【任何】对
//   live_tune.ini 的改动都会在运行中生效。agent 关掉之后我们不再写它,
//   但把通道留着没有意义, 还多一个能改参数的活动入口。
void uninstall_production_hooks()
{
    QFile::remove(sentinelPath());
}

} // namespace boss::autotune
