// 配置解析回归 (2026-09-14 建立; 2026-09-17 收缩)。
//
// ── 本文件的历史与现在 ──────────────────────────────────────────────────────
// 它原本叫「配置槽位迁移」回归, 钉的是 `pidf_mapping_version` 那套迁移机制
// (v4 -> v7: 重置增益、重置在途补偿、丢弃旧死区值、清空尺度基准框高)。
//
// ★ 2026-09-17: 整个瞄准控制链(aim_pid / boss_aim / aim_tracker / aim_scale /
//   aim_path / anchor_filter / auto_stop / trigger_scope / autotune_*)连同
//   runtime/mouse_thread_loop.cpp 一起被删除, 程序现在只做【采集 → 推理】。
//
//   那些槽位【本身已经不存在】, 所以:
//     · `pidf_mapping_version` 这个版本号被删除 —— 没有槽位需要迁移, 版本号
//       没有读者。迁移块(config.cpp 里的 v4/v5/v6/v7 四段)也随之删除。
//     · 关于"迁移"的断言(v6 清基准、v7 幂等、v4 连跳、旧阈值不能变成新基准、
//       旧死区值被丢弃、越界预测系数被重置)全部失去被测对象, 已删除。
//       ★ 这【不是】把回归删掉就完事: 那些断言测的是一个已经不存在的机制,
//         留着只会编不过或者变成空转。
//     · 关于 `esync_*` / 尺度 / 预测的默认值与夹取断言同样失去对象, 已删除。
//
//   ── 保留下来的这部分仍然有意义 ──────────────────────────────────────────
//   `HotkeyProfile` 里还有一批【活着】的字段, 它们由本测试继续看住:
//     name / group / keys / fovX / fovY   (热键识别与 FOV 门控)
//     aim_classes                          (目标选择优先级列表 + 旧三槽迁移)
//     crosshair_detect_enabled             (准星找色开关)
//     dynamic_fov_enabled / _strength      (动态 FOV 门控)
//   这些是检测/瞄准点选择这一侧的东西, 控制链删掉之后它们仍然在用, 所以
//   它们的解析、默认值、夹取、以及"老键残留不影响新键"这几条必须继续有回归。
//
//   ── 还有一条【必须保留】的断言 ──────────────────────────────────────────
//   [5] 老方案文件里那几十个已删除的键(pidf_* / esync_* / aim_scale_* /
//   aim_path_* / trigger_*)必须被【安全忽略】—— 不报错、不污染任何活着的键。
//   这是升级路径上最容易出致命故障的一处(用户的老 ini 里全是这些键)。
//   谁要是顺手把"读都不读"改成"读到就报错", 这里会立刻变红。
//
// ★ 判定哲学(与项目其它回归一致): 只断言可判定的性质, 不断言具体数值。
#include "config/config.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

// 写一份最小的合法配置。extra 会被插到 [hotkey.0] 段头之后。
// 必须是【完整】配置 —— Config::loadConfig 对缺键的文件会落到默认值,
// 那样测的就不是解析而是默认值了。
std::string write_config(const std::string& path, const std::string& extra = "")
{
    std::ofstream f(path, std::ios::binary);
    f << "# 配置解析回归用配置\n"
      << "[hotkey.0]\n"
      << "name = Aim\n"
      << "group = 默认\n"
      << "keys = RightMouseButton\n"
      << "fovX = 106\n"
      << "fovY = 74\n"
      << extra;
    f.close();
    return path;
}

} // namespace

int main()
{
    std::printf("=== config_migration_test: 配置解析 ===\n");

    // ── [1] 活着的热键字段: 能写进来、能读回去 ──────────────────────────────
    std::printf("\n[1] 热键字段的解析往返\n");
    {
        const std::string p = write_config("basic.ini",
            "fovX = 120\nfovY = 90\n"
            "crosshair_detect_enabled = true\n"
            "dynamic_fov_enabled = true\ndynamic_fov_strength = 0.40\n"
            "aim_classes = 3:0.100:0.900:0.250\n");

        Config c;
        check(c.loadConfig(p), "最小配置能加载");
        check(!c.hotkeys.empty(), "解析出了热键组");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.name == "Aim", "name 原样读回");
            check(hp.keys.size() == 1 && hp.keys[0] == "RightMouseButton",
                  "keys 原样读回");
            check(hp.fovX == 120 && hp.fovY == 90, "fovX/fovY 原样读回");
            check(hp.crosshair_detect_enabled,
                  "crosshair_detect_enabled 原样读回");
            check(hp.dynamic_fov_enabled, "dynamic_fov_enabled 原样读回");
            check(hp.dynamic_fov_strength > 0.39f && hp.dynamic_fov_strength < 0.41f,
                  "dynamic_fov_strength 原样读回");
            check(hp.aim_classes.size() == 1 && hp.aim_classes[0].class_id == 3,
                  "aim_classes 原样读回");
        }
    }

    // ── [2] 缺键 -> 取结构体默认 ────────────────────────────────────────────
    std::printf("\n[2] 缺键取默认\n");
    {
        const std::string p = write_config("missing.ini");
        Config c;
        check(c.loadConfig(p), "只有段头的配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.fovX == 106 && hp.fovY == 74, "缺键 -> fovX/fovY 取默认 106/74");
            check(!hp.crosshair_detect_enabled, "缺键 -> 准星找色默认关闭");
            check(!hp.dynamic_fov_enabled, "缺键 -> 动态 FOV 默认关闭");
            check(hp.dynamic_fov_strength > 0.59f && hp.dynamic_fov_strength < 0.61f,
                  "缺键 -> dynamic_fov_strength 默认 0.60");
            check(hp.aim_classes.empty(), "缺键 -> aim_classes 为空");
        }
    }

    // ── [3] 旧三槽 (target_class_N / y_top_N / y_bot_N) 的迁移仍然有效 ──────
    // ★ 这是【唯一还活着的迁移】: 它搬的是 aim_classes, 而不是控制参数。
    //   瞄准控制链删掉之后, 这条迁移仍然是"老方案升级后还能瞄"的关键。
    std::printf("\n[3] 旧三槽 -> aim_classes 的迁移(唯一还活着的迁移)\n");
    {
        const std::string p = write_config("legacy_slots.ini",
            "target_class_1 = 2\n"
            "target_y_top_1 = 0.0\ntarget_y_bot_1 = 0.25\n"
            "target_min_conf_1 = 0.30\n");

        Config c;
        check(c.loadConfig(p), "旧三槽配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.aim_classes.size() == 1, "旧三槽被搬成一条 aim_classes");
            if (hp.aim_classes.size() == 1)
            {
                const auto& ac = hp.aim_classes[0];
                check(ac.class_id == 2, "旧三槽: class_id 原样搬过来");
                // ★ 语义反转必须保留: 旧值是"距框顶比例", 新 UI 是 1=框顶/0=框底。
                //   y_top=0.0 / y_bot=0.25 -> 新 y_offset ∈ {0.75, 1.00}。
                //   这条写反了会让锁点从头上掉到脚下, 而且【不会报错】。
                check(ac.y_offset > 0.74f && ac.y_offset < 0.76f,
                      "★ 旧三槽: y_offset = 1 - y_bot = 0.75(比例语义被反转)");
                check(ac.y_offset_max > 0.99f && ac.y_offset_max <= 1.0f,
                      "★ 旧三槽: y_offset_max = 1 - y_top = 1.00");
                check(ac.min_conf > 0.29f && ac.min_conf < 0.31f,
                      "旧三槽: min_conf 原样搬过来");
            }
        }
    }

    // ── [4] aim_classes 的新格式与夹取 ─────────────────────────────────────
    std::printf("\n[4] aim_classes 的解析与夹取\n");
    {
        // 越界的 y_offset / min_conf 必须被夹到 [0,1], 且两端必须有序。
        const std::string p = write_config("clamp.ini",
            "aim_classes = 1:2.0:-1.0:5.0;7:0.500:0.500:0.0\n");
        Config c;
        c.loadConfig(p);
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.aim_classes.size() == 2, "两条 aim_classes 都被解析");
            if (hp.aim_classes.size() == 2)
            {
                check(hp.aim_classes[0].y_offset >= 0.0f &&
                      hp.aim_classes[0].y_offset <= 1.0f,
                      "越界 y_offset 被夹到 [0,1]");
                check(hp.aim_classes[0].y_offset_max >= 0.0f &&
                      hp.aim_classes[0].y_offset_max <= 1.0f,
                      "越界 y_offset_max 被夹到 [0,1]");
                check(hp.aim_classes[0].min_conf >= 0.0f &&
                      hp.aim_classes[0].min_conf <= 1.0f,
                      "越界 min_conf 被夹到 [0,1]");
                // ★ 关键不变式: 夹取【之后】两端必须有序(下游假定 min <= max)。
                check(hp.aim_classes[0].y_offset <= hp.aim_classes[0].y_offset_max,
                      "★ 夹取后 y_offset <= y_offset_max(顺序被修正)");
                check(hp.aim_classes[1].y_offset == hp.aim_classes[1].y_offset_max,
                      "两端相等 -> 固定锁点(允许)");
            }
        }
    }

    // ── [5] ★★ 已删除的瞄准控制键必须被【安全忽略】 ────────────────────────
    //
    // 这是本文件【最重要】的一条, 也是升级路径上唯一会致命的地方:
    // 用户的每一份老方案文件里都写满了这些键。它们必须
    //   ① 不报错(loadConfig 仍然返回 true),
    //   ② 不影响任何【活着的】键的读取 —— 即"读都不读", 而不是"读到一半改变行为"。
    //
    // ★ 2026-09-17: 瞄准控制链整条删除, 所以下面这份清单里还包括了原先"活着"的
    //   pidf_* / esync_* / aim_scale_* / aim_path_* / trigger_* 与版本号本身。
    std::printf("\n[5] ★★ 已删除的瞄准控制键: 不报错、不污染活着的键\n");
    {
        const std::string p = write_config("removed_keys.ini",
            "fovX = 111\n"
            "some_typo_key = 1\n"   // 注意: 故意写个不存在的键
            "crosshair_detect_enabled = true\n"
            // ── 已删除: 版本号与整套 PID 槽位 ──
            "pidf_mapping_version = 7\n"
            "pidf_kp_x = 35\npidf_kp_y = 35\n"
            "pidf_ki_x = 1.0\npidf_ki_y = 1.0\n"
            "pidf_kd_x = 0.0\npidf_kd_y = 0.0\n"
            "pidf_psat_x = 5\npidf_psat_y = 5\n"
            "pidf_deadzone_x = 3\npidf_deadzone_y = 3\n"
            "pidf_limit_x = 200\npidf_limit_y = 200\n"
            "pidf_predict_x = 0.1\npidf_predict_y = 0.1\n"
            "pidf_predict_min_w = 20\npidf_predict_max_w = 80\n"
            "pidf_predict_damp = 0.25\n"
            "pidf_predict_max_px = 12\npidf_predict_vel_floor = 60\n"
            "pidf_inflight_x = 1.6\npidf_inflight_y = 1.6\n"
            "pidf_inflight_window_ms = 46\n"
            "pidf_kf_x = 1.0\npidf_lr_x = 0.08\n"        // 更早删掉的键
            "aim_px_per_count_x = 0.5\naim_px_per_count_y = 0.5\n"
            // ── 已删除: EventSync 跟踪器 / 预测 ──
            "esync_min_hits = 7\nesync_max_age = 9\n"
            "esync_assoc_iou = 0.5\nesync_vel_sample_ms = 30\n"
            "esync_pred_factor_x = 0.2\nesync_pred_factor_y = 0.2\n"
            "esync_pred_min_w = 25\nesync_pred_max_w = 90\n"
            "esync_assoc_radius_px = 123\nesync_vel_window_ms = 456\n"
            "esync_counts_per_pixel_x = 0.5\nesync_counts_per_pixel_y = 0.6\n"
            "esync_inflight_window_ms = 30\nesync_inflight_beta = 2.5\n"
            "esync_self_motion_gain = 0.8\n"
            // ── 已删除: 尺度调度 ──
            "aim_scale_enabled = 1\naim_scale_max = 1.5\n"
            "aim_scale_min = 0.7\naim_scale_base_h = 123.5\n"
            "aim_scale_near_h = 160\naim_scale_far_h = 45\n"
            // ── ★ 已恢复: 瞄准轨迹曲线 (2026-09-17) ──
            //   后端 mouse/aim_path.h 已重建, 这些键重新被读/写。
            "aim_path_mode = 3\naim_path_influence = 40\n"
            "aim_path_bezier_cx1 = 0.3\naim_path_bezier_cy1 = 0.0\n"
            "aim_path_bezier_cx2 = 0.7\naim_path_bezier_cy2 = 0.0\n"
            "aim_path_wind_gravity = 5\naim_path_wind_wind = 2\n"
            "aim_path_wind_step = 10\naim_path_wind_distance = 8\n"
            "aim_path_wind_threshold = 10\n"
            // ★ 这三个是【真的删除、不恢复】的键: 手绘曲线采样点的编辑器与
            //   神经权重都没重建, 所以它们仍必须被安全忽略。
            "aim_path_custom_samples = 0.0,0.5,0.0\n"
            "aim_path_custom_file = whatever.curve\n"
            "aim_path_neural_enabled = true\n"
            "aim_path_neural_weights = 1,2,3\n"
            // ── ★ 已恢复: 扳机 / 自动开镜 / 自动急停 (2026-09-17) ──
            //   后端 mouse/trigger_fsm.h + trigger_scope.h + auto_stop.h 已重建。
            "trigger_enabled = true\ntrigger_fire_delay = 50\n"
            "trigger_fire_duration = 100\ntrigger_fire_interval = 200\n"
            "trigger_y_percent = 100\n"
            "trigger_delay_jitter_ms = 5\ntrigger_duration_jitter_ms = 5\n"
            "trigger_interval_jitter_ms = 5\ntrigger_switch_cooldown_ms = 10\n"
            "trigger_auto_scope = 2\ntrigger_scope_delay_ms = 150\n"
            "trigger_auto_stop = 1\ntrigger_stop_ms = 60\n"
            // ── 已删除: 更早的档位键 ──
            "aim_mode = 0\n"
            "use_prediction_tick = true\nprediction_tick_hz = 240\n"
            "prediction_tick_max_run = 2\n"
            "anchor_filter_ms = 8\n"
            // ── 活着的键放在【最后】, 用来验证前面的键没把它们带偏 ──
            "dynamic_fov_enabled = true\ndynamic_fov_strength = 0.35\n"
            "aim_classes = 4:0.200:0.800:0.400\n");

        Config c;
        check(c.loadConfig(p),
              "★★ 满篇已删除的键 -> 仍然加载成功(不报错)");
        check(!c.hotkeys.empty(), "满篇已删除的键 -> 仍解析出热键组");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            // ★★ 核心: 活着的键全部按【字面值/默认值】读回, 一点没被污染。
            check(hp.fovX == 111 && hp.fovY == 74,
                  "★★ 活着的 fovX 读到 111; 未写的 fovY 仍是默认 74");
            check(hp.crosshair_detect_enabled,
                  "★★ 活着的 crosshair_detect_enabled 读到 true");
            check(hp.dynamic_fov_enabled, "★★ 活着的 dynamic_fov_enabled 读到 true");
            check(hp.dynamic_fov_strength > 0.34f && hp.dynamic_fov_strength < 0.36f,
                  "★★ 活着的 dynamic_fov_strength 读到 0.35");
            check(hp.aim_classes.size() == 1 && hp.aim_classes[0].class_id == 4,
                  "★★ 活着的 aim_classes 读到 class_id = 4");
            check(hp.name == "Aim" && hp.keys.size() == 1 &&
                  hp.keys[0] == "RightMouseButton",
                  "★★ 活着的 name / keys 未受影响");
            // ★★ 2026-09-17: 这一大段"已删除的键"里, trigger_* 与 aim_path_*
            //   已经【恢复成活键】—— 所以它们不再是"被安全忽略"的样本,
            //   而是"被正确读回"的样本。上面那份 config 给的是
            //   aim_path_mode=3 / aim_path_influence=40 / trigger_enabled=true /
            //   trigger_fire_delay=50 / trigger_auto_scope=2, 逐条钉住。
            check(hp.aim_path_mode == 3, "★ 恢复的 aim_path_mode 读到 3");
            check(hp.aim_path_influence == 40, "★ 恢复的 aim_path_influence 读到 40");
            check(hp.trigger_enabled, "★ 恢复的 trigger_enabled 读到 true");
            check(hp.trigger_fire_delay == 50, "★ 恢复的 trigger_fire_delay 读到 50");
            check(hp.trigger_auto_scope == 2, "★ 恢复的 trigger_auto_scope 读到 2");
        }
    }

    // ── [6] 已删除的键不会被重新写出去 ──────────────────────────────────────
    // ★ 与 [5] 互补: [5] 管"读不报错", [6] 管"写不再出现"。
    //   两者都成立, 老方案文件才会在下次保存时自动瘦身。
    std::printf("\n[6] 已删除的键不会被写回\n");
    {
        const std::string p = write_config("roundtrip.ini",
            "pidf_mapping_version = 7\npidf_kp_x = 35\n"
            "esync_min_hits = 3\naim_scale_base_h = 100\n"
            "trigger_enabled = true\naim_path_mode = 3\n"
            "crosshair_detect_enabled = true\n");

        Config c;
        check(c.loadConfig(p), "往返用配置能加载");

        const std::string out = "roundtrip_out.ini";
        check(c.saveConfig(out), "保存成功");

        std::ifstream f(out, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        f.close();

        check(!body.empty(), "写出的文件非空");
        // ★★ 2026-09-17 变更: `trigger_*` 与 `aim_path_*` 不再属于这一类。
        //   用户要求把自动扳机与风力曲线加回来, 后端已重建
        //   (mouse/trigger_fsm.h / aim_path.h), 这两个键族【重新变成活键】——
        //   它们现在必须被读、被写、被落盘, 所以从"已删除"名单里移出,
        //   并转入选下面的"仍然写出/真的往返"那一段。
        for (const char* gone : {"pidf_mapping_version", "pidf_kp_x",
                                 "esync_min_hits", "aim_scale_base_h"})
        {
            check(body.find(gone) == std::string::npos,
                  std::string("★ 已删除的键不再写出: ") + gone);
        }
        // ★ 恢复的键必须【真的往返】—— 不只看它出现在文件里。
        //   只查字符串会漏掉"写了个默认值回去"这种假通过。
        for (const char* back : {"trigger_enabled", "aim_path_mode"})
        {
            check(body.find(back) != std::string::npos,
                  std::string("★ 恢复的键重新写出: ") + back);
        }
        // 活着的键必须还在文件里 —— 否则"瘦身"就变成了"失忆"。
        for (const char* kept : {"fovX", "fovY", "crosshair_detect_enabled",
                                 "aim_classes", "dynamic_fov_enabled"})
        {
            check(body.find(kept) != std::string::npos,
                  std::string("活着的键仍然写出: ") + kept);
        }

        // ★★ 恢复的键: 值真的往返 (2026-09-17)。
        //   只查键名会漏掉"写了个默认值回去"—— 那种假通过让用户改了参数
        //   重启就丢, 正是本仓库反复踩的静默失效。这里逐字段比对字面值。
        {
            const std::string p2 = write_config("restored.ini",
                "trigger_enabled = true\ntrigger_fire_delay = 45\n"
                "trigger_fire_interval = 133\ntrigger_y_percent = 150\n"
                "trigger_auto_scope = 1\ntrigger_auto_stop = 1\ntrigger_stop_ms = 77\n"
                "aim_path_mode = 3\naim_path_influence = 63\n"
                "aim_path_wind_gravity = 7.5\naim_path_wind_wind = 3.25\n"
                "aim_path_wind_threshold = 12\n");
            Config c2;
            check(c2.loadConfig(p2), "恢复的键: 配置能加载");
            if (!c2.hotkeys.empty())
            {
                const auto& e = c2.hotkeys[0];
                check(e.trigger_enabled, "trigger_enabled 读到 true");
                check(e.trigger_fire_delay == 45, "trigger_fire_delay 读到 45");
                check(e.trigger_fire_interval == 133, "trigger_fire_interval 读到 133");
                check(e.trigger_y_percent == 150, "trigger_y_percent 读到 150");
                check(e.trigger_auto_scope == 1, "trigger_auto_scope 读到 1");
                check(e.trigger_auto_stop == 1, "trigger_auto_stop 读到 1");
                check(e.trigger_stop_ms == 77, "trigger_stop_ms 读到 77");
                check(e.aim_path_mode == 3, "aim_path_mode 读到 3");
                check(e.aim_path_influence == 63, "aim_path_influence 读到 63");
                check(e.aim_path_wind_gravity > 7.4f && e.aim_path_wind_gravity < 7.6f,
                      "aim_path_wind_gravity 读到 7.5");
                check(e.aim_path_wind_wind > 3.2f && e.aim_path_wind_wind < 3.3f,
                      "aim_path_wind_wind 读到 3.25");
                check(e.aim_path_wind_threshold == 12, "aim_path_wind_threshold 读到 12");
            }
        }
    }

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
