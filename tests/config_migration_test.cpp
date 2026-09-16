// 配置槽位迁移的回归 (2026-09-14)。
//
// ── 为什么这个测试值得单独存在 ───────────────────────────────────────────────
// `pidf_mapping_version` 是本项目【唯一】的配置迁移机制, 而迁移写错的后果是
// 【静默的】: 用户的老配置被按新语义误读, 手感突然变了却查不出原因 —— 而且
// 用户不会怀疑"是上次升级改的", 只会以为是自己调坏了。
//
// 所以每一档迁移都必须有可执行的回归。本文件验证 v6 -> v7:
//
//   尺度调度的槽位语义从"两个绝对框高阈值"改成"一个自动学的基准框高"。
//   旧键 aim_scale_near_h / aim_scale_far_h 已删除, 新增 aim_scale_base_h。
//
// ★ 迁移的【关键断言】: 旧阈值【绝不能】被当成新基准。
//   160px 与 45px 是当初拍出来的经验常数, 而 base_h 的语义是"用户整定时目标
//   真实有多大"。拿前者当后者, 等于给用户一个他从没调过的距离当参照系 —— 表现
//   是尺度调度在错误的距离上生效, 比"完全不生效"更难查。
//   正确做法是清 0(= 还没学到 -> 整条链路中性), 用户下次调参时自动学出来。
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
// 那样测的就不是迁移而是默认值了。
std::string write_config(const std::string& path, const std::string& version_line,
                         const std::string& extra = "")
{
    std::ofstream f(path, std::ios::binary);
    f << "# 迁移回归用配置\n"
      << "[hotkey.0]\n"
      // ★ 先把基准键写死, 再写 extra —— 后写的同键会覆盖先写的,
      //   所以 extra 必须放在【后面】, 否则用例里的值会被这里覆盖掉。
      << "pidf_kp_x = 35\n"
      << "pidf_kp_y = 35\n"
      << "pidf_ki_x = 1.0\n"
      << "pidf_ki_y = 1.0\n"
      << "pidf_kd_x = 0.0\n"
      << "pidf_kd_y = 0.0\n"
      << "aim_scale_enabled = 1\n"
      << "aim_scale_max = 1.50\n"
      << extra
      << version_line << "\n";
    f.close();
    return path;
}

} // namespace

int main()
{
    std::printf("=== config_migration_test: 配置槽位迁移 ===\n");

    // ── [1] v6 老配置(带旧阈值) -> 必须清空基准, 不迁移旧值 ─────────────────
    std::printf("\n[1] v6 -> v7: 旧阈值不能变成新基准\n");
    {
        const std::string p = write_config("v6.ini",
            "pidf_mapping_version = 6",
            "aim_scale_near_h = 160\naim_scale_far_h = 45\n");

        Config c;
        const bool ok = c.loadConfig(p);
        check(ok, "v6 配置能加载");
        check(!c.hotkeys.empty(), "v6 配置解析出了热键组");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.pidf_mapping_version == 7, "版本推进到 7");
            // ★ 核心断言
            check(hp.aim_scale_base_h == 0.0f,
                  "★ 基准被清 0 (旧阈值【没有】被当成基准)");
            check(hp.aim_scale_min == 1.00f, "s_min 回默认 1.00");
            // 无关的键必须原样保留 —— 迁移只动该动的
            check(hp.aim_scale_max == 1.50f, "s_max 保持不变");
            check(hp.aim_scale_enabled == 1, "开关保持不变");
            check(hp.pidf_kp_x == 35.0f, "Kp 保持不变");
        }
    }

    // ── [2] 已经被迁移过的配置 (v7) -> 幂等, 不能反复清 ────────────────────
    std::printf("\n[2] v7 -> v7: 幂等(已学到的基准不能被再次清掉)\n");
    {
        const std::string p = write_config("v7.ini",
            "pidf_mapping_version = 7",
            "aim_scale_base_h = 123.5\naim_scale_min = 0.70\n");

        Config c;
        check(c.loadConfig(p), "v7 配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            // ★ 这是最关键的一条: 迁移块是 `if (version < 7)`, 若写成 `<= 7`
            //   或漏掉版本推进, 用户学到的基准每次启动都会被清掉 —— 表现是
            //   "尺度调度时灵时不灵", 极难排查。
            check(hp.aim_scale_base_h == 123.5f,
                  "★ 已学到的基准被保留(迁移幂等)");
            check(hp.aim_scale_min == 0.70f, "已设置的 s_min 被保留");
            check(hp.pidf_mapping_version == 7, "版本仍是 7");
        }
    }

    // ── [3] 更老的配置 (v4) -> 应连跳多档, 且全部清干净 ────────────────────
    std::printf("\n[3] v4 -> v7: 跨档迁移要能连跳\n");
    {
        const std::string p = write_config("v4.ini",
            "pidf_mapping_version = 4",
            "aim_scale_near_h = 200\naim_scale_far_h = 30\n"
            "pidf_psat_x = 5\npidf_psat_y = 5\n"
            "pidf_predict_x = 50\npidf_predict_y = 50\n");

        Config c;
        check(c.loadConfig(p), "v4 配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.pidf_mapping_version == 7, "v4 一路推进到 7");
            check(hp.aim_scale_base_h == 0.0f, "v4: 基准被清 0");
            // v6 那档的迁移也必须生效(旧死区值 <=20px 要丢弃)
            check(hp.pidf_psat_x == 0, "v4: 旧 psat(<=20) 被丢弃");
            // v6 那档还会重置越界的预测系数
            check(hp.pidf_predict_x == 0.0f, "v4: 越界预测系数被重置");
        }
    }

    // ── [4] 缺版本键的老文件 -> 视为最老, 全部迁移 ──────────────────────────
    std::printf("\n[4] 没有版本键 -> 按最老处理\n");
    {
        std::ofstream f("nover.ini", std::ios::binary);
        f << "[hotkey.0]\npidf_kp_x = 35\npidf_kp_y = 35\n"
          << "aim_scale_near_h = 160\naim_scale_far_h = 45\n";
        f.close();

        Config c;
        check(c.loadConfig("nover.ini"), "无版本键配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.pidf_mapping_version == 7, "无版本键 -> 推进到 7");
            check(hp.aim_scale_base_h == 0.0f, "无版本键 -> 基准清 0");
        }
    }

    // ── [5] 基准的合法性夹取 ────────────────────────────────────────────────
    std::printf("\n[5] 基准的合法性: 非法值一律回落到 0(中性)\n");
    {
        struct Case { const char* val; float want; const char* name; };
        const Case cases[] = {
            {"-50.0",   0.0f,   "负数 -> 0"},
            {"2.0",     0.0f,   "过小(噪声框) -> 0"},
            {"9999.0",  0.0f,   "过大 -> 0"},
            {"100.0",   100.0f, "合法值保留"},
        };
        for (const auto& cs : cases)
        {
            const std::string p = write_config("base.ini",
                "pidf_mapping_version = 7",
                std::string("aim_scale_base_h = ") + cs.val + "\n");
            Config c;
            c.loadConfig(p);
            if (!c.hotkeys.empty())
                check(c.hotkeys[0].aim_scale_base_h == cs.want, cs.name);
        }
    }

    // ── [6] s_min / s_max 的夹取与顺序保护 ─────────────────────────────────
    std::printf("\n[6] s_min / s_max 夹取\n");
    {
        struct Case { const char* smin; const char* smax; float wmin; float wmax; const char* name; };
        const Case cases[] = {
            {"1.00", "1.50", 1.00f, 1.50f, "默认区间"},
            {"0.50", "1.50", 0.50f, 1.50f, "s_min < 1 允许(远处降增益)"},
            {"0.10", "1.50", 0.30f, 1.50f, "s_min 低于下界 -> 夹到 0.30"},
            {"0.50", "5.00", 0.50f, 2.00f, "s_max 超上界 -> 夹到 2.00"},
            // ★ 填反的情况: 两个域只在 1.0 相接, 所以夹取后得到 [1.0, 1.2] ——
            //   一个【合法但用户没填过】的区间。关键在于它必须仍然是合法的
            //   (min <= max), 不能让下游拿到空区间。
            {"1.80", "1.20", 1.00f, 1.20f, "顺序反了 -> 夹取后仍是合法区间"},
            // 两边都顶到 1.0 -> 区间退化成一点 = 中性(等价于关闭尺度)
            {"1.00", "1.00", 1.00f, 1.00f, "两端都是 1.0 -> 退化成中性"},
        };
        for (const auto& cs : cases)
        {
            const std::string extra = std::string("aim_scale_min = ") + cs.smin
                                    + "\naim_scale_max = " + cs.smax + "\n";
            const std::string p = write_config("sm.ini",
                "pidf_mapping_version = 7", extra);
            Config c;
            c.loadConfig(p);
            if (!c.hotkeys.empty())
            {
                const auto& hp = c.hotkeys[0];
                check(hp.aim_scale_min == cs.wmin &&
                      hp.aim_scale_max == cs.wmax, cs.name);
                check(hp.aim_scale_min <= hp.aim_scale_max,
                      std::string(cs.name) + " (顺序合法)");
            }
        }
    }

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
