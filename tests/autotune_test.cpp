// ============================================================================
//  调参 agent 的回归测试 —— 只测【可判定】的性质, 不测 LLM 内容。
//
//  覆盖:
//    [1] chainlog 行解析 (正常/缺字段/非 SecPid 段/畸形)
//    [2] 指标计算 (过冲/翻转率/发散/分位数) —— 用【人造序列】验证定义
//    [3] 安全夹取 (单步幅度/beta 不对称/NaN/绝对域)
//    [4] 回滚判定 (发散/抖动/明显变差)
//    [5] JSON 提取与解析 (围栏/前后废话/缺字段)
//    [6] 端到端: 造假日志 -> 指标 -> 夹取, 断言全链路数值
//
//  ★ 刻意【不】联网、不测 LLM 返回内容 —— 那不可重复。
//    测的是"无论模型说什么, 我们的处理都是对的"。
// ============================================================================

#include "mouse/autotune_agent.h"
#include "mouse/autotune_llm.h"
#include "mouse/autotune_metrics.h"
#include "mouse/autotune_runtime.h"
#include "mouse/autotune_safety.h"
#include "runtime/chain_log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  [FAIL] %s\n", what); }
    else       { std::printf("  [ ok ] %s\n", what); }
}

void check_near(double a, double b, double tol, const char* what)
{
    ++g_checks;
    const bool ok = std::fabs(a - b) <= tol;
    if (!ok) { ++g_fail; std::printf("  [FAIL] %s (得到 %.4f, 期望 %.4f)\n", what, a, b); }
    else     { std::printf("  [ ok ] %s (%.4f)\n", what, a); }
}

using boss::autotune::Sample;
using boss::autotune::Metrics;

// 造一个样本
Sample mk(double t, double ex, double ey, double cmdy, double dt_ms = 8.33)
{
    Sample s;
    s.valid = true;
    s.t = t;
    s.ex = ex;
    s.ey = ey;
    s.cmdy = cmdy;
    s.dt_ms = dt_ms;
    s.frame = static_cast<std::int64_t>(t * 120.0);
    return s;
}

} // namespace

int main()
{
    std::printf("=== autotune 回归 ===\n\n");

    // ── [1] chainlog 解析 ──────────────────────────────────────────────────
    std::printf("[1] chainlog 行解析\n");
    {
        const char* line =
            "L,2,frame=1234,t=10.2817,dt_ms=8.33,sup=0,switch=0,jump_px=0.0,"
            "ex=12.50,ey=-3.25,eux=12.50,euy=-3.25,"
            "ipx=45.00,ipy=-11.00,dpx=0.10,dpy=0.02,ix=300.0,iy=-80.0,"
            "carryx=0.42,carryy=-0.11,cmdx=8.30,cmdy=-2.10,lim=200/200";
        Sample s;
        check(boss::autotune::parse_pid_line(line, s), "解析正常 SecPid 行");
        check(s.frame == 1234, "frame 正确");
        check_near(s.t, 10.2817, 1e-6, "t 正确");
        check_near(s.ex, 12.50, 1e-9, "ex 正确");
        check_near(s.ey, -3.25, 1e-9, "ey 正确");
        check_near(s.cmdx, 8.30, 1e-9, "cmdx 正确");
        check_near(s.cmdy, -2.10, 1e-9, "cmdy 正确");
        check_near(s.carryx, 0.42, 1e-9, "carryx 正确");
        check(!s.suppressed, "sup=0 -> 未压制");

        // 非 SecPid 段必须拒绝
        Sample t2;
        check(!boss::autotune::parse_pid_line("L,5,frame=1,t=1.0,ex=1.0", t2),
              "拒绝 SecControl 段");
        check(!boss::autotune::parse_pid_line("L,event,frame=1,t=1.0", t2),
              "拒绝 event 行");
        check(!boss::autotune::parse_pid_line("", t2), "拒绝空行");
        check(!boss::autotune::parse_pid_line("garbage", t2), "拒绝垃圾行");
        // 没有误差字段的行不算有效(避免把无关记录当样本)
        check(!boss::autotune::parse_pid_line("L,2,frame=1,t=1.0,dt_ms=8.3", t2),
              "缺 ex/ey 不算有效样本");
        // 未知键必须被忽略而不是让解析失败(向前兼容)
        Sample t3;
        check(boss::autotune::parse_pid_line(
                  "L,2,frame=1,t=1.0,ex=1.0,ey=2.0,future_key=999,another=x", t3),
              "未知键被忽略(向前兼容)");
        check_near(t3.ex, 1.0, 1e-9, "未知键不影响已解析字段");
    }

    // ── [2] 指标计算 ───────────────────────────────────────────────────────
    std::printf("\n[2] 指标计算\n");
    {
        // ① 纯收敛: 误差指数衰减, 无过冲。
        // ★ 注意 err_max 必然会很大(第一拍的误差本来就大) —— "最大误差大"
        //   不等于"发散"。发散判据看的是【尾段是否比前段明显更差】。
        {
            std::vector<Sample> v;
            for (int i = 0; i < 100; ++i)
                v.push_back(mk(i * 0.00833, 400.0 * std::exp(-0.05 * i), 0.0, -5.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.n == 100, "样本数正确");
            check_near(m.overshoot_y, 0.0, 1e-9, "单调收敛 -> 过冲为 0");
            check(!m.divergent, "单调收敛 -> 未发散");
            check(m.err_max > 300.0, "首拍误差本来就大(max 不受发散判据影响)");
        }
        // ② 有过冲: 误差穿过零点后反向到 20px
        {
            std::vector<Sample> v;
            for (int i = 0; i < 50; ++i)
                v.push_back(mk(i * 0.00833, 100.0 - 4.0 * i, 0.0, -5.0));
            const Metrics m = boss::autotune::compute(v);
            // ex 从 +100 线性降到 -96, 反向最大 96
            check_near(m.overshoot_x, 96.0, 1e-6, "过冲 = 反号后最大幅值");
            check(m.overshoot_y == 0.0, "Y 轴无换向 -> 过冲 0");
        }
        // ③ 翻转率: 每拍反号 -> 接近 100%
        {
            std::vector<Sample> v;
            for (int i = 0; i < 60; ++i)
                v.push_back(mk(i * 0.00833, 0.2, 0.2, (i % 2 == 0) ? 1.0 : -1.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.nonzero_out == 60, "全部算作下发");
            check(m.sign_flips == 59, "翻转次数 = n-1");
            check(m.flip_ratio > 0.95, "每拍反号 -> 翻转率 >95%");
        }
        // ④ 零指令不计入翻转率(否则"偶尔停一下"会被误判为抖动)
        {
            std::vector<Sample> v;
            for (int i = 0; i < 60; ++i)
                v.push_back(mk(i * 0.00833, 0.2, 0.2, (i % 2 == 0) ? 1.0 : 0.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.nonzero_out == 30, "零指令不计数");
            check(m.sign_flips == 0, "同号不翻转");
            check_near(m.flip_ratio, 0.0, 1e-9, "翻转率为 0");
        }
        // ⑤ 发散: 尾段误差远大于前段
        {
            std::vector<Sample> v;
            for (int i = 0; i < 200; ++i)
                v.push_back(mk(i * 0.00833, 1.0 + 0.5 * std::max(0, i - 100), 0.0, 5.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.divergent, "误差持续增大 -> 判为发散");
        }
        // ⑥ 分位数
        {
            std::vector<Sample> v;
            for (int i = 1; i <= 100; ++i) v.push_back(mk(i * 0.00833, i, 0.0, 1.0));
            const Metrics m = boss::autotune::compute(v);
            check_near(m.err_median, 50.5, 1.0, "中位数");
            check_near(m.err_p90, 90.5, 1.0, "p90");
            check_near(m.err_max, 100.0, 1e-6, "最大值");
        }
        // ⑦ 非有限值 -> 发散
        {
            std::vector<Sample> v;
            for (int i = 0; i < 40; ++i) v.push_back(mk(i * 0.00833, 1.0, 0.0, 1.0));
            v.push_back(mk(1.0, std::nan(""), 0.0, 1.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.divergent, "NaN 误差 -> 判为发散");
        }
    }

    // ── [3] 安全夹取 ───────────────────────────────────────────────────────
    std::printf("\n[3] 安全夹取\n");
    {
        using boss::autotune::Knobs;
        using boss::autotune::apply_limits;

        // 模型想把 Kp 从 35 直接跳到 200 -> 只允许 ±20%
        Knobs prev; prev.kp = 35.0;
        Knobs want = prev; want.kp = 200.0;
        auto r = apply_limits(prev, want);
        check(r.any_clamped, "超幅度被夹取");
        check_near(r.applied.kp, 42.0, 1e-6, "Kp 单步上限 = 35*1.2");

        // 模型想砍到 1 -> 同样只允许 -20%
        want = prev; want.kp = 1.0;
        r = apply_limits(prev, want);
        check_near(r.applied.kp, 28.0, 1e-6, "Kp 单步下限 = 35*0.8");

        // beta 上调更保守(±10%), 下调用常规(±20%)
        prev.beta = 1.6;
        want = prev; want.beta = 3.0;
        r = apply_limits(prev, want);
        check_near(r.applied.beta, 1.76, 1e-6, "beta 上调只允许 +10%");
        want = prev; want.beta = 0.0;
        r = apply_limits(prev, want);
        check_near(r.applied.beta, 1.28, 1e-6, "beta 下调允许 -20%");

        // NaN / Inf 一律保持原值(不能让垃圾值进控制器)
        prev.kp = 35.0;
        want = prev; want.kp = std::nan("");
        r = apply_limits(prev, want);
        check_near(r.applied.kp, 35.0, 1e-9, "NaN -> 保持原值");
        want = prev; want.kp = std::numeric_limits<double>::infinity();
        r = apply_limits(prev, want);
        check_near(r.applied.kp, 35.0, 1e-9, "Inf -> 保持原值");

        // 绝对域: 即使一步步来也不能越界
        prev.kp = 290.0;
        want = prev; want.kp = 1000.0;
        r = apply_limits(prev, want);
        check(r.applied.kp <= 300.0, "Kp 不超过绝对上限 300");

        // 预测系数受 §4.2 约束
        prev.predict_x = 0.15;
        want = prev; want.predict_x = 5.0;
        r = apply_limits(prev, want);
        check(r.applied.predict_x <= 0.2, "预测系数不超过 0.2");

        // 尺度上界
        prev.scale_max = 1.9;
        want = prev; want.scale_max = 5.0;
        r = apply_limits(prev, want);
        check(r.applied.scale_max <= 2.0, "s_max 不超过 2.0");

        // 无改动时不该报夹取
        Knobs same; same.kp = 35.0;
        r = apply_limits(same, same);
        check(!r.any_clamped, "完全相同的参数 -> 无夹取");
    }

    // ── [4] 回滚判定 ───────────────────────────────────────────────────────
    std::printf("\n[4] 回滚判定\n");
    {
        using boss::autotune::should_rollback;
        Metrics now, prev;

        now.divergent = true;
        check(should_rollback(now, prev, false).rollback, "发散 -> 回滚");

        now = Metrics{}; now.nonzero_out = 100; now.sign_flips = 95; now.flip_ratio = 0.95;
        check(should_rollback(now, prev, false).rollback, "翻转率 95% -> 回滚");

        // 翻转但样本太少不该回滚(避免噪声误触发)
        now = Metrics{}; now.nonzero_out = 10; now.sign_flips = 10; now.flip_ratio = 1.0;
        check(!should_rollback(now, prev, false).rollback, "下发太少 -> 不回滚");

        // 明显变差
        prev = Metrics{}; prev.n = 100; prev.err_p90 = 2.0;
        now  = Metrics{}; now.n = 100;  now.err_p90 = 5.0;
        check(should_rollback(now, prev, true).rollback, "p90 2.0->5.0 -> 回滚");

        // 轻微波动不该回滚
        now.err_p90 = 2.2;
        check(!should_rollback(now, prev, true).rollback, "p90 轻微波动 -> 不回滚");

        // 没有上一轮时不做比较
        now = Metrics{}; now.n = 100; now.err_p90 = 99.0;
        check(!should_rollback(now, prev, false).rollback, "无上一轮 -> 不比较");
    }

    // ── [5] JSON 提取与解析 ────────────────────────────────────────────────
    std::printf("\n[5] JSON 提取与解析\n");
    {
        using boss::autotune::extract_json_object;
        check(extract_json_object("{\"kp\":40}") == "{\"kp\":40}", "纯 JSON");
        check(extract_json_object("```json\n{\"kp\":40}\n```") == "{\"kp\":40}",
              "剥掉 markdown 围栏");
        check(extract_json_object("好的, 我建议:\n{\"kp\":40}\n以上。") == "{\"kp\":40}",
              "剥掉前后废话");
        check(extract_json_object("没有 JSON").empty(), "无 JSON -> 空");

        using boss::autotune::parse_knobs_json;
        boss::autotune::Knobs cur; cur.kp = 35; cur.ki = 1.0; cur.kd = 0.0; cur.beta = 1.6;
        boss::autotune::Knobs out; std::string err;

        check(parse_knobs_json("{\"kp\":40,\"kd\":0.01}", cur, out, err), "解析成功");
        check_near(out.kp, 40.0, 1e-9, "kp 被解析");
        check_near(out.kd, 0.01, 1e-9, "kd 被解析");
        check_near(out.ki, 1.0, 1e-9, "未提及的 ki 保持原值");
        check_near(out.beta, 1.6, 1e-9, "未提及的 beta 保持原值");

        // 认不出任何参数 -> 失败(否则会静默把参数改光)
        check(!parse_knobs_json("{\"foo\":1}", cur, out, err),
              "无已知参数名 -> 解析失败");
        check(!parse_knobs_json("", cur, out, err), "空串 -> 解析失败");

        // 布尔与整数
        check(parse_knobs_json("{\"kp\":30,\"scale_on\":true,\"predict_max_px\":16}",
                               cur, out, err), "布尔/整数解析");
        check(out.scale_on, "scale_on=true");
        check(out.predict_max_px == 16, "predict_max_px=16");
    }

    // ── [6] 端到端: 假日志 -> 指标 -> 夹取 ─────────────────────────────────
    std::printf("\n[6] 端到端\n");
    {
        // 造一段"自持抖动"的日志行文本, 走完整解析->指标链路
        std::string log;
        for (int i = 0; i < 80; ++i)
        {
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                "L,2,frame=%d,t=%.4f,dt_ms=8.33,ex=%.2f,ey=%.2f,"
                "cmdx=%.2f,cmdy=%.2f,carryx=0.0,carryy=0.0,sup=0,switch=0\n",
                i, i * 0.00833, (i % 2) ? 0.2 : -0.2,
                (i % 2) ? 0.2 : -0.2, (i % 2) ? 1.0 : -1.0, (i % 2) ? 1.0 : -1.0);
            log += buf;
        }
        std::vector<Sample> samples;
        std::size_t pos = 0;
        while (pos < log.size())
        {
            const std::size_t nl = log.find('\n', pos);
            const std::string line = log.substr(pos, nl - pos);
            boss::autotune::Sample s;
            if (boss::autotune::parse_pid_line(line.c_str(), s)) samples.push_back(s);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        check(samples.size() == 80, "从文本解析出 80 条样本");

        const Metrics m = boss::autotune::compute(samples);
        check(m.n == 80, "指标样本数 80");
        check(m.flip_ratio > 0.9, "端到端识别出自持抖动");
        check(boss::autotune::should_rollback(m, Metrics{}, false).rollback,
              "端到端触发回滚");

        // 模型即使给极端值, 落地也必须安全
        // ★ 用 1e-6 容差比较: 35*1.2 在浮点上是 42.000000000000004,
        //   写死 <=42.0 会假失败(第一版就踩了这个)。
        boss::autotune::Knobs prev; prev.kp = 35; prev.beta = 1.6;
        boss::autotune::Knobs want = prev; want.kp = 9999; want.beta = 99;
        const auto rep = boss::autotune::apply_limits(prev, want);
        check(rep.applied.kp <= 42.0 + 1e-6 && rep.applied.beta <= 1.76 + 1e-6,
              "端到端: 极端请求被安全夹取");
        check_near(rep.applied.kp, 42.0, 1e-6, "端到端 kp 夹到 42");
        check_near(rep.applied.beta, 1.76, 1e-6, "端到端 beta 夹到 1.76");
    }

    // ── [7] 发散判据必须按【时间顺序】取头尾 ───────────────────────────────
    // ★ 这一节是针对一个真实 bug 的回归: 早期实现先对幅值排序再取"前/后 25%",
    //   于是"前 25%"= 误差最小的、后 25% = 误差最大的, 任何有起伏的正常数据
    //   都被判成发散 —— agent 会一直在原地回滚。
    std::printf("\n[7] 发散判据 (时间顺序回归)\n");
    {
        // ① 衰减收敛 + 明显起伏(仍应判为未发散)
        {
            std::vector<Sample> v;
            for (int i = 0; i < 120; ++i)
            {
                // 误差整体衰减, 但带一个 20 倍幅值的正弦起伏
                const double base = 200.0 * std::exp(-0.03 * i);
                const double ripple = 1.0 + 0.9 * std::sin(i * 0.7);
                v.push_back(mk(i * 0.00833, base * ripple, 0.0, 5.0));
            }
            const Metrics m = boss::autotune::compute(v);
            check(m.err_max > 300.0, "该序列最大误差确实很大");
            check(!m.divergent, "★ 衰减收敛即使起伏大也不判发散");
        }
        // ② 先好后坏(应判为发散)
        {
            std::vector<Sample> v;
            for (int i = 0; i < 120; ++i)
                v.push_back(mk(i * 0.00833, (i < 80) ? 0.5 : 30.0, 0.0, 5.0));
            const Metrics m = boss::autotune::compute(v);
            check(m.divergent, "★ 前段好尾段坏 -> 判为发散");
        }
        // ③ 全程稳定(应判为未发散)
        {
            std::vector<Sample> v;
            for (int i = 0; i < 120; ++i)
                v.push_back(mk(i * 0.00833, 0.3 + 0.2 * std::sin(i * 0.5), 0.0, 1.0));
            const Metrics m = boss::autotune::compute(v);
            check(!m.divergent, "全程稳定 -> 未发散");
        }
    }

    // ── [8] LLM 客户端: 不联网也能测的部分 ─────────────────────────────────
    // ★ 刻意不测真实网络 —— 那不可重复。这里测的是【失败路径】:
    //   配置不合法时必须【立刻】返回 ok=false 且带可读原因, 不能挂住、
    //   不能返回 ok=true。这一节不需要网络(空配置在发请求前就返回了)。
    std::printf("\n[8] LLM 客户端失败路径 (不联网)\n");
    {
        using boss::autotune::LlmConfig;
        using boss::autotune::chat_completion;

        // 空地址: 必须在发请求前就拒绝
        {
            LlmConfig c; c.model = "m";
            const auto r = chat_completion(c, "s", "u");
            check(!r.ok, "空 base_url -> 失败");
            check(!r.error.empty(), "空 base_url -> 有可读原因");
            check(r.http_status == 0, "空 base_url -> 未发起请求(http 0)");
            check(r.elapsed_ms == 0.0, "空 base_url -> 立刻返回(未等待)");
        }
        // 空模型名
        {
            LlmConfig c; c.base_url = "https://example.com";
            const auto r = chat_completion(c, "s", "u");
            check(!r.ok, "空 model -> 失败");
            check(r.elapsed_ms == 0.0, "空 model -> 立刻返回");
        }
        // 两者都空
        {
            LlmConfig c;
            const auto r = chat_completion(c, "s", "u");
            check(!r.ok, "全空 -> 失败");
            check(r.http_status == 0, "全空 -> 未发起请求");
        }
        // ★ 关键: 任何失败都必须是 ok=false, 绝不能"假装成功"
        //   否则 agent 会拿着空回答去解析参数。
        {
            LlmConfig c; c.base_url = "not a url at all"; c.model = "m";
            const auto r = chat_completion(c, "s", "u");
            check(!r.ok, "畸形地址 -> 不假装成功");
        }
    }

    // ── [9] 采样会话: "开始/结束" + 模式 (2026-09-14) ─────────────────────
    //
    // ★ 这一版把"后台线程一直连调"换成了"开始/结束, 一次一轮"。
    //   这里测的是【区间切得对不对】—— 那是整个设计的地基: 切错了,
    //   后面所有指标都是错的, 而且错得很隐蔽(数字看着正常)。
    std::printf("\n[9] 采样会话: 两次按钮点击之间的日志 = 一段\n");
    {
        boss::autotune::AutoTuner t;
        t.set_mode(boss::autotune::TuneMode::Moving);
        check(t.mode() == boss::autotune::TuneMode::Moving, "模式可设置/读取");
        t.set_feel_text("跟枪拖后腿");
        check(t.feel_text() == "跟枪拖后腿", "体感文本可设置/读取");

        check(!t.session_open(), "初始 -> 会话关闭");
        check(t.session_sample_count() == 0, "初始 -> 已记录 0 条");

        // 造样本: 模拟 120fps
        auto mk = [](int i, double h) {
            boss::autotune::Sample s;
            s.frame = i;
            s.t = i / 120.0;
            s.dt_ms = 1000.0 / 120.0;
            s.ex = 0.0; s.ey = 0.0;
            s.bbox_h = h;
            s.valid = true;
            return s;
        };

        // ★★ 关键: 点「开始」【之前】的日志不算数 ★★
        for (int i = 0; i < 50; ++i) t.buffer().push(mk(i, 100.0));

        t.begin_session();
        check(t.session_open(), "★ 点「开始」后会话打开");
        check(t.session_sample_count() == 0, "★ 刚开始时 0 条(起点=现在)");

        for (int i = 50; i < 350; ++i) t.buffer().push(mk(i, 100.0));
        check(t.session_sample_count() == 300, "★ 采到 300 条(实时计数)");

        // ★ 点「结束」之后又来的样本【不该】进这一段 —— 验证终点标记
        const long long pushed_at_end = t.buffer().pushed();
        check(pushed_at_end == 350, "结束时缓冲里有 350 条");

        // 手工复现 close_segment 会选的区间: [begin 标记, end 标记)
        const auto seg_samples = t.buffer().range(50, pushed_at_end);
        check(seg_samples.size() == 300, "★ 段 = 两次点击之间恰好 300 条");
        check(seg_samples.front().frame == 50, "段的起点是「开始」那一刻");
        check(seg_samples.back().frame == 349, "段的终点是「结束」那一刻");

        // 结束之后再来的样本不改变已定的区间
        for (int i = 350; i < 400; ++i) t.buffer().push(mk(i, 100.0));
        const auto seg_again = t.buffer().range(50, pushed_at_end);
        check(seg_again.size() == 300, "★ 结束后的新样本不会混进上一段");

        // ★ 环形淘汰后仍能正确切段 —— 这是用【绝对序号】而不是下标的意义。
        //   用户采样很久(超过缓冲容量)时, 起点会被挤掉, 但 range() 会裁掉
        //   已淘汰的部分而不是报错, 剩下的数据仍然可用。
        t.buffer().clear();
        for (long long i = 0; i < 20000; ++i) t.buffer().push(mk(0, 90.0));
        check(t.buffer().pushed() == 20000, "绝对序号持续递增");
        check(t.buffer().oldest() == 20000 - static_cast<long long>(t.buffer().size()),
              "最旧序号 = 已推入 - 缓冲长度");
        const auto tail = t.buffer().range(19900, 20000);
        check(tail.size() == 100, "★ 淘汰后仍能精确切出尾部 100 条");
        const auto stale = t.buffer().range(0, 100);
        check(stale.empty(), "★ 已被淘汰的区间 -> 空(而不是越界读到脏数据)");
        const auto partial = t.buffer().range(0, 20000);
        check(partial.size() == t.buffer().size(),
              "★ 起点已淘汰 -> 自动裁到还剩的部分(不报错)");
    }

    // ── [10] judge_segment: 只挡【真的会让指标失真】的事 ──────────────────
    //
    // ★★ 这一节的教训值得记下来 ★★
    //   上一版的判据里有一条"中途换目标必须为 0", 它读的是
    //   `Sample::switched`, 而那个字段来自 `boss_aim.cpp` 的
    //   `target_switched = motion_suppressed && anchor_jump >= 25px`
    //   —— 真实含义是【玩家甩枪】, 不是"换了目标"。
    //   结果: 正常跟枪里每次甩都触发, 几乎每一段都被判成脏数据,
    //   用户侧的表现就是"一直提示我中途换了目标"。
    //
    //   ★ 教训: 判据本身比被测现象更容易出错。凡是要拦人的门限, 都得先问
    //     "它读的那个量, 语义到底是什么", 而不是"名字看起来像什么"。
    //     这也是为什么这一版把判据压到最少 —— 只留真正影响统计量的。
    std::printf("\n[10] judge_segment: 只挡真正会让指标失真的数据\n");
    {
        auto make_seg = [](int n, double dur_hint) {
            boss::autotune::TrackSegment s;
            for (int i = 0; i < n; ++i)
            {
                boss::autotune::Sample x;
                x.frame = i;
                x.t = i * dur_hint / n;
                x.dt_ms = 1000.0 * dur_hint / n;
                x.ex = 1.0; x.ey = 1.0;
                x.bbox_h = 100.0;
                x.valid = true;
                s.samples.push_back(x);
            }
            s.t_begin = s.samples.front().t;
            s.t_end   = s.samples.back().t;
            s.bbox_h_median = 100.0;
            s.bbox_h_p10 = 100.0;
            s.bbox_h_p90 = 100.0;
            s.coverage = 1.0;
            return s;
        };

        // 干净的一段 -> 通过
        {
            auto s = make_seg(240, 2.0);
            const auto v = boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static);
            check(v.ok, "干净数据(2秒240条) -> 通过");
        }
        // 太短
        {
            auto s = make_seg(20, 0.2);
            const auto v = boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static);
            check(!v.ok, "样本太少 -> 拒绝");
            check(!v.reason.empty(), "拒绝时给出可读原因");
        }
        // ★★ 回归: 甩枪【不再】被误判成"换了目标" ★★
        //   switched 字段现在根本不参与判据。这里把一段正常数据标上一堆
        //   switched, 断言它【仍然通过】—— 这是这次改版的核心回归。
        {
            auto s = make_seg(240, 2.0);
            for (int i = 0; i < 40; ++i) s.samples[static_cast<std::size_t>(i * 5)].switched = true;
            check(boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok,
                  "★ 甩枪(switched=true)不再被判成脏数据");
        }
        // 覆盖率低 -> 真的没法用
        {
            auto s = make_seg(240, 2.0);
            s.coverage = 0.40;
            check(!boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok,
                  "覆盖率 40% -> 拒绝");
        }
        // ★ 覆盖率门限放宽的边界: 0.55 应当【通过】(旧版 0.75 会拒)
        {
            auto s = make_seg(240, 2.0);
            s.coverage = 0.55;
            check(boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok,
                  "★ 覆盖率 55% -> 通过(门限已放宽到 50%)");
        }
        // 野值太多
        {
            auto s = make_seg(240, 2.0);
            s.jumps = 40;
            check(!boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok,
                  "野值跳变 40 次 -> 拒绝");
        }
        // ★ 少量野值应当通过 —— 偶发几次检测抖动不该让整段作废
        {
            auto s = make_seg(240, 2.0);
            s.jumps = 5;
            check(boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok,
                  "★ 野值 5 次 -> 通过(门限 12)");
        }
        // 没有有效框高 = 压根没锁到目标
        {
            auto s = make_seg(240, 2.0);
            s.bbox_h_median = 0.0;
            check(!boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static).ok, "无有效框高 -> 拒绝");
        }
        // ★ 模式错配: 选静态但目标明显在远近变化
        {
            auto s = make_seg(240, 2.0);
            s.bbox_h_p10 = 60.0;
            s.bbox_h_p90 = 180.0;      // 离散度 1.2
            const auto v = boss::autotune::judge_segment(s, boss::autotune::TuneMode::Static);
            check(!v.ok, "★ 选「静态」但尺寸变化大 -> 拒绝并提示换模式");
            check(v.reason.find("动态") != std::string::npos, "提示里提到改用动态模式");
        }
        // ★ 但同一个段在【动态】模式下应当被接受 —— 尺寸变化本来是动态靶的正常现象
        {
            auto s = make_seg(240, 2.0);
            s.bbox_h_p10 = 60.0;
            s.bbox_h_p90 = 180.0;
            check(boss::autotune::judge_segment(s, boss::autotune::TuneMode::Moving).ok,
                  "★ 同样数据在动态模式下通过(尺寸变化是正常的)");
            check(boss::autotune::judge_segment(s, boss::autotune::TuneMode::Feel).ok, "体感模式不查尺寸稳定性");
        }
    }

    // ── [11] 模式真的进了提示词 ──────────────────────────────────────────
    //
    // ★ 提示词是模型的全部上下文。模式选错等于调参方向错, 所以要断言
    //   "模式说明确实被写进去了", 而不是只在代码里传了个枚举。
    std::printf("\n[11] 提示词按模式给不同解读\n");
    {
        boss::autotune::TrackSegment seg;
        seg.samples.resize(300);
        seg.t_begin = 0.0;
        seg.t_end = 2.5;
        seg.coverage = 0.98;
        seg.jumps = 0;
        seg.gaps = 0;
        seg.bbox_h_median = 120.0;
        seg.bbox_h_p10 = 110.0;
        seg.bbox_h_p90 = 132.0;

        const boss::autotune::Knobs k;
        const boss::autotune::Metrics m;

        const std::string ps = boss::autotune::build_user_prompt(k, m, seg, boss::autotune::TuneMode::Static, "", 1, "");
        const std::string pm = boss::autotune::build_user_prompt(k, m, seg, boss::autotune::TuneMode::Moving, "", 1, "");
        const std::string pf = boss::autotune::build_user_prompt(k, m, seg, boss::autotune::TuneMode::Feel, "跟枪拖后腿", 1, "");

        check(ps.find(u8"静态目标") != std::string::npos, "静态提示含模式名");
        check(pm.find(u8"动态目标") != std::string::npos, "动态提示含模式名");
        check(pf.find(u8"体感") != std::string::npos, "体感提示含模式名");

        // ★ 三个模式的解读指令必须【不同】—— 否则模式选择就是摆设
        check(ps != pm && pm != pf && ps != pf, "★ 三个模式给出不同的提示词");
        check(pm.find(u8"物理上消不掉") != std::string::npos,
              "★ 动态模式明确告诉模型: 匀速滞后消不掉, 别硬加 Kp");
        check(pf.find(u8"跟枪拖后腿") != std::string::npos, "★ 体感模式带上了用户原话");
        check(pf.find(u8"主证据") != std::string::npos,
              "★ 体感模式明确: 用户的话是主证据");
        check(ps.find(u8"跟枪拖后腿") == std::string::npos, "静态模式不该出现用户原话");

        // 数据来历必须写清楚 —— 模型得知道这是"用户主动采的一段"
        check(ps.find(u8"点「开始」后") != std::string::npos,
              "提示里说明了数据来历(按按钮采的)");
        check(ps.find(u8"甩枪") != std::string::npos,
              "★ 提示里说明甩枪峰值是正常的, 别当成异常");
        check(ps.find(u8"2.5") != std::string::npos, "提示里带了这一段时长");

        // 三个模式的枚举名
        check(std::string(boss::autotune::mode_name(boss::autotune::TuneMode::Static)) == u8"静态目标", "mode_name 静态");
        check(std::string(boss::autotune::mode_name(boss::autotune::TuneMode::Moving)) == u8"动态目标", "mode_name 动态");
        check(std::string(boss::autotune::mode_name(boss::autotune::TuneMode::Feel))   == u8"体感",     "mode_name 体感");
    }

    // ── [12] 覆盖率公式 (真实日志回归) ───────────────────────────────────
    //
    // ★★ 这个公式之前算错过两次, 两次都是【在用一个可能错的模型推测本该有
    //   多少条】, 而不是直接看引擎报的 dt。 ★★
    //
    //   实测用户日志: 4454 条 SecPid, dt_ms 分布 min 7.66 / 中位 8.74 /
    //   max 15.58 ms, **零大间隔**, 数据完全正常。
    //
    //   但旧公式 `n ÷ (跨度 ÷ 平均dt)` 给出 **31.9%**, 直接过不了 50% 门限,
    //   用户侧的现象就是"为什么覆盖率一直不够"。两个原因:
    //   ① 日志里有 9.5s/9.9s/12s/18s/29.6s 的**空洞**(那段时间瞄准循环没有
    //      输出帧), 跨度和平均 dt 相除把这些死时间算成了"本该有的样本数";
    //   ② 平均 dt(9.26ms) 比典型步进(8.3ms) 大 —— 慢帧把均值拉高, 于是连
    //      完全连续的一段也只能算出 111%, 永远不可能是 100%。
    //
    //   新公式: coverage = (dt_ms ≤ 中位数×4) 的比例。
    //   ★ 本用例走的是**真实代码路径**(begin_session → push → end_session
    //     里的 close_segment), 不是自己复现一遍公式 —— 否则测的是测试自己。
    std::printf("\n[12] 覆盖率公式: 不能被死时间和均值带偏\n");
    {
        // 造一段样本喂进去。gap_at >= 0 时在该下标插入一次 30 秒断档。
        auto run = [](int n, int gap_at, double gap_ms) -> boss::autotune::TrackSegment {
            boss::autotune::AutoTuner t;
            t.begin_session();
            for (int i = 0; i < n; ++i)
            {
                boss::autotune::Sample x;
                x.frame = i;
                // t 累加(空洞后整体平移), dt_ms 如实反映"这一拍等了多久"
                const bool is_gap = (gap_at >= 0 && i == gap_at);
                x.dt_ms = is_gap ? gap_ms : 8.7;
                x.t = i * 0.0087 + ((gap_at >= 0 && i > gap_at) ? (gap_ms / 1000.0) : 0.0);
                x.ex = 1.0; x.ey = 1.0; x.bbox_h = 100.0;
                x.jump_px = 0.0; x.valid = true;
                t.buffer().push(x);
            }
            // ★ 走真实路径: end_session 内部会调 close_segment。
            //   它还会尝试发 LLM 请求, 但没配 base_url -> 立刻失败返回,
            //   不会联网(chat_completion 对空地址是直接失败的)。
            t.end_session();
            boss::autotune::TrackSegment out;
            t.last_segment_info(out);
            return out;
        };

        // ① 完全连续 400 条 -> 覆盖率必须是 100%(旧的均值公式会给 111%)
        {
            const auto s = run(400, -1, 0.0);
            // ★ 注意: last_segment_info() 刻意【不回传样本】(UI 不需要几千条),
            //   所以这里断言的是算出来的统计量, 不是样本数。
            check(s.coverage > 0.999 && s.coverage <= 1.0,
                  "★ 完全连续 -> 覆盖率恰好 100%(不是 111%)");
            check(s.gaps == 0, "完全连续 -> 0 处断档");
            check(s.bbox_h_median > 0.0, "连续段有有效框高(说明确实算到了样本)");
        }

        // ② ★ 核心回归: 插一个 30 秒断档。样本只少不了一条(那条本身还在),
        //    覆盖率应当只掉一两拍, 而【不是】掉到 30% 以下。
        //    这就是用户遇到的情况: 程序有过暂停, 但采到的数据本身没问题。
        {
            const auto s = run(400, 200, 30000.0);
            check(s.coverage > 0.99,
                  "★ 一次 30 秒断档只让覆盖率掉 ~0.25%(旧公式会掉到 20% 左右)");
            check(s.gaps == 1, "断档被计为 1 次");
            check(s.bbox_h_median > 0.0, "含断档段仍有有效框高");
            // 样本数也够(400 条 > 60), 所以整段应当通过
            // ★ 用 duration 反推样本规模: 400 × 8.7ms ≈ 3.48s
            check(s.duration() > 3.0, "含断档段时长 >3 秒(说明 400 条都在)");
        }

        // ②b 该通过 —— 用户不该因为程序中途卡了一下就被拒
        //     这条单独跑, 拿到完整的 Round 看判据结论。
        {
            boss::autotune::AutoTuner t;
            t.begin_session();
            for (int i = 0; i < 400; ++i)
            {
                boss::autotune::Sample x;
                x.frame = i;
                const bool is_gap = (i == 200);
                x.dt_ms = is_gap ? 30000.0 : 8.7;
                x.t = i * 0.0087 + ((i > 200) ? 30.0 : 0.0);
                x.ex = 1.0; x.ey = 1.0; x.bbox_h = 100.0; x.valid = true;
                t.buffer().push(x);
            }
            t.end_session();
            const auto hist = t.history();
            check(hist.size() == 1, "结束一次 -> 恰好产生 1 轮记录");
            if (!hist.empty())
            {
                const auto& r = hist.front();
                // 没配 base_url, 所以会停在"问模型"那一步并失败;
                // 关键是要确认它【通过了判据】(error 里不该是判据拒绝语)。
                const bool rejected = r.error.find(u8"不连续") != std::string::npos
                                   || r.error.find(u8"太少")   != std::string::npos
                                   || r.error.find(u8"时间")   != std::string::npos;
                check(!rejected,
                      "★ 只有一次断档的数据不该被判据拒绝(旧公式会误拒)");
            }
        }

        // ③ 反过来: 真的一直断(每 5 条断一次)就该被拒 —— 门限不能形同虚设
        {
            boss::autotune::AutoTuner t;
            t.begin_session();
            for (int i = 0; i < 400; ++i)
            {
                boss::autotune::Sample x;
                x.frame = i;
                const bool bad = (i % 5 == 0);
                x.dt_ms = bad ? 500.0 : 8.7;      // 每 5 条空 0.5 秒
                x.t = i * 0.1;
                x.ex = 1.0; x.ey = 1.0; x.bbox_h = 100.0; x.valid = true;
                t.buffer().push(x);
            }
            t.end_session();
            boss::autotune::TrackSegment s;
            t.last_segment_info(s);
            check(s.coverage < 0.9, "★ 每 5 条断一次 -> 覆盖率明显下降");
        }

        // ④ 空段/单条不崩(除零保护)
        {
            boss::autotune::AutoTuner t;
            t.begin_session();
            t.end_session();                    // 一条都没有
            boss::autotune::TrackSegment s;
            const bool has = t.last_segment_info(s);
            check(!has || s.samples.empty() || s.coverage >= 0.0,
                  "空段不崩(覆盖率有定义)");
        }
    }

    // ── [13] Runtime 接入层: "点了开始却 0 条" 的回归 ──────────────────
    //
    // ★★ 用户实测症状: 「一直显示这段数据只有 0 条」★★
    //
    //   根因在 `Runtime::begin_session()` 里的 `last_seq_ = 0;`。
    //   `feed_from_chainlog()` 用 0 当"尚未初始化"的哨兵:
    //
    //       if (last_seq_ == 0) { last_seq_ = seq - total; ... }
    //
    //   于是点「开始」把它归零之后, 下一次 feed 以为自己是"第一次接入",
    //   把起点重设到【当前最旧一条】并把 seq 也赋成同一个值 —— 循环
    //   `for (i = last_seq_; i < seq; ++i)` 一条都不跑, 永远收不到样本。
    //
    //   ★ 教训: 一个变量如果有"哨兵值", 就不要拿它当"重置"用。
    //     重置和未初始化是两件不同的事, 复用同一个值会让"重新开始"
    //     被误读成"第一次开始"。
    //
    //   ★ 这一节走【真实的 Runtime + 真实的 chainlog】, 而不是直接测
    //     AutoTuner —— 上面那条 bug 恰恰只在两层交界处才显形, 只测
    //     AutoTuner 是测不到的(它自己那层完全正确)。
    std::printf("\n[13] Runtime 接入层: 点开始之后必须真的收到样本\n");
    {
        auto& rt = boss::autotune::Runtime::instance();
        auto& sink = runtime::chainlog::sink();

        // 装一组假 hooks(不碰真实 config/live_tune)
        boss::autotune::RuntimeHooks hooks;
        hooks.read  = []() { return boss::autotune::Knobs{}; };
        hooks.write = [](const boss::autotune::Knobs&, std::string&) { return true; };
        hooks.read_base_h  = []() { return 0.0; };
        hooks.write_base_h = [](double) { return true; };
        rt.install(hooks);
        check(rt.installed(), "hooks 已安装");

        // 往 chainlog 里灌一段 "看起来像真的" SecPid 行
        auto emit = [&](int n, double t0) {
            for (int i = 0; i < n; ++i)
            {
                // ★ chainlog::write 只写 "L,2" (不带尾部逗号), 逗号要靠
                //   format 自己带上 —— 解析器要求行首恰好是 "L,2,"。
                //   少了这个逗号 => 一行都解析不出来 => "0 条"。
                char line[runtime::chainlog::kLineSize];
                std::snprintf(line, sizeof(line),
                    ",frame=%d,t=%.4f,dt_ms=8.70,sup=0,switch=0,jump_px=0.0,"
                    "ex=1.20,ey=-0.80,eux=1.20,euy=-0.80,ipx=1.1,ipy=-0.7,"
                    "dpx=0.00,dpy=0.00,ix=0.3,iy=-0.2,carryx=0.1,carryy=0.0,"
                    "cmdx=5.0,cmdy=-3.0,lim=200/200,bbh=100.0",
                    i, t0 + i * 0.0087);
                runtime::chainlog::write(runtime::chainlog::SecPid, 0, "%s", line);
            }
        };

        // ① 先让它跑起来(模拟程序已经在正常运行, 会话还没开)
        emit(50, 0.0);
        for (int k = 0; k < 3; ++k) rt.feed_from_chainlog();
        check(!rt.session_open(), "初始: 会话关闭");

        // ② ★★ 核心回归: 点「开始」→ 再喂帧 → 必须收得到 ★★
        rt.begin_session();
        check(rt.session_open(), "★ 点开始 -> 会话打开");
        check(rt.session_sample_count() == 0, "刚开始 0 条(起点=现在)");

        emit(200, 1.0);
        for (int k = 0; k < 3; ++k) rt.feed_from_chainlog();

        // ★ 这一条就是用户报的 bug: 旧代码这里恒为 0
        const auto got = rt.session_sample_count();
        std::printf("       [诊断] 收到 %zu 条 (期望 200)\n", got);
        check(got == 200,
              "★★ 点开始后收到 200 条(旧代码恒为 0 —— 用户报的 bug)");

        // ③ 再喂更多, 计数继续涨(证明不是"只收到第一批")
        emit(100, 3.0);
        for (int k = 0; k < 3; ++k) rt.feed_from_chainlog();
        check(rt.session_sample_count() == 300, "继续喂 -> 计数继续涨");

        // ④ 结束 -> 会话关闭, 之后进来的帧不再计入
        rt.end_session();
        check(!rt.session_open(), "点结束 -> 会话关闭");
        emit(50, 5.0);
        for (int k = 0; k < 3; ++k) rt.feed_from_chainlog();
        check(rt.session_sample_count() == 0, "结束后 count 归 0");

        // ⑤ ★ 第二次采样必须同样有效 —— 这条专门盯"重置之后还能不能再用"。
        //    (旧代码归零 last_seq_ 的另一个后果: 第二次点开始同样收不到。)
        rt.begin_session();
        emit(150, 7.0);
        for (int k = 0; k < 3; ++k) rt.feed_from_chainlog();
        check(rt.session_sample_count() == 150,
              "★★ 第二次采样同样收到 150 条(重置后仍可用)");
        rt.end_session();

        // ⑥ 收尾: 把 sink 清掉, 别影响别的用例
        {
            std::lock_guard<std::mutex> g(sink.mutex);
            for (auto& l : sink.lines) l[0] = '\0';
            sink.sequence = 0;
        }
    }

    // ── [14] 响应体解释: "测试连接成功但没 content" 的回归 ────────────────
    //
    // ★★ 用户实测症状: 「测试连接成功, 但显示没 content」★★
    //
    //   根因是【推理模型 + max_tokens 太小】: 带思维链的模型会先产出一大段
    //   reasoning_content, 而【思考 token 也算在 max_tokens 里】。预算被吃光
    //   之后 finish_reason="length"、content=""。这在多个上游都被记录为
    //   已知失败模式(reasoning models starve to empty responses)。
    //
    //   为什么"测试连接"却好的: 那个测试用的提示词极短("请只回复一个 JSON"),
    //   模型不用想太多; 真调参的提示词长得多, 思考量大一个量级 ——
    //   所以同一个配置, 测试通过、调参失败。
    //
    //   ★ 这一节拿【真实的响应体 JSON】直接断言诊断结论。之所以能这么测,
    //     是因为把响应解释从 chat_completion 里拆成了 parse_completion_body
    //     (不碰网络)。
    std::printf("\n[14] 响应体解释: 必须说清\"为什么没 content\"\n");
    {
        using boss::autotune::parse_completion_body;

        // ① 正常响应 -> 取到正文
        {
            const std::string body = R"({"id":"x","choices":[{"index":0,)"
                R"("message":{"role":"assistant","content":"{\"kp\":35}"},)"
                R"("finish_reason":"stop"}],"usage":{"completion_tokens":12}})";
            const auto r = parse_completion_body(body, 200, 4096);
            check(r.ok, "正常响应 -> ok");
            check(r.text == "{\"kp\":35}", "正常响应 -> 正文正确");
            check(r.finish_reason == "stop", "正常响应 -> finish_reason=stop");
            check(r.completion_tokens == 12, "正常响应 -> 取到 token 用量");
            check(r.reasoning_len == 0, "正常响应 -> 没有思考内容");
        }

        // ② ★★ 核心回归: 推理模型预算用光 ★★
        //    真实的形态就是: reasoning_content 一大段, content 是空串,
        //    finish_reason = "length"。
        {
            std::string body = R"({"choices":[{"index":0,"message":{)"
                R"("role":"assistant","content":"",)"
                R"("reasoning_content":"让我先看看这些指标……【很长很长的思考】"},)"
                R"("finish_reason":"length"}],)"
                R"("usage":{"completion_tokens":800}})";
            const auto r = parse_completion_body(body, 200, 800);
            check(!r.ok, "★ 预算用光 -> 不算成功");
            check(r.error.find(u8"预算") != std::string::npos,
                  "★ 错误信息点明是【输出预算】用光了");
            check(r.error.find(u8"最大输出") != std::string::npos,
                  "★ 错误信息告诉用户去调「最大输出」");
            check(r.error.find("800") != std::string::npos,
                  "★ 错误信息带上实际的 max_tokens 数值");
            check(r.reasoning_len > 0, "★ 识别出这是带思维链的模型");
            check(r.finish_reason == "length", "★ finish_reason 被带出来");
        }

        // ③ 有思考但 finish_reason 不是 length (例如模型就是不输出正文)
        {
            const std::string body = R"({"choices":[{"index":0,"message":{)"
                R"("role":"assistant","content":null,)"
                R"("reasoning_content":"我思考了"},)"
                R"("finish_reason":"stop"}]})";
            const auto r = parse_completion_body(body, 200, 4096);
            check(!r.ok, "只有思考没有正文 -> 不算成功");
            check(r.error.find(u8"思考") != std::string::npos,
                  "错误信息点明只返回了思考");
            check(r.reasoning_len == std::string(u8"我思考了").size(),
                  "★ 思考长度按 UTF-8 字节数如实统计");
        }

        // ④ 完全不是 OpenAI 格式 -> 把响应开头带出来, 便于排查
        {
            const std::string body = R"({"unexpected":"shape"})";
            const auto r = parse_completion_body(body, 200, 4096);
            check(!r.ok, "非预期格式 -> 不算成功");
            check(r.error.find("unexpected") != std::string::npos,
                  "★ 错误信息里带上了响应原文(用户才能自己看出问题)");
        }

        // ⑤ HTTP 层失败 -> 仍然是 HTTP 错误, 不要把上游的 message 丢掉
        {
            const std::string body =
                R"({"error":{"message":"Invalid API key","type":"auth"}})";
            const auto r = parse_completion_body(body, 401, 4096);
            check(!r.ok, "HTTP 401 -> 失败");
            check(r.error.find("401") != std::string::npos, "带上状态码");
            check(r.error.find("Invalid API key") != std::string::npos,
                  "★ 把上游给的原因一并显示(而不是只说 HTTP 401)");
        }

        // ⑥ 边界: 空响应体不该崩
        {
            const auto r = parse_completion_body("", 200, 4096);
            check(!r.ok, "空响应体 -> 失败而不是崩");
            check(!r.error.empty(), "空响应体 -> 给出可读原因");
        }

        // ⑦ 边界: content 里含转义引号/换行, 不能被解析带偏
        {
            const std::string body = R"({"choices":[{"message":{)"
                R"("content":"第一行\n第二行 \"带引号\" 结束"},)"
                R"("finish_reason":"stop"}]})";
            const auto r = parse_completion_body(body, 200, 4096);
            check(r.ok, "含转义的正文 -> 仍能取到");
            check(r.text == "第一行\n第二行 \"带引号\" 结束",
                  "★ 转义(\\n 和 \\\")被正确还原");
        }

        // ⑧ ★ 默认预算必须足够大 —— 这条是防"又把默认值调回 800"
        {
            const boss::autotune::LlmConfig def;
            check(def.max_tokens >= 4096,
                  "★ 默认 max_tokens >= 4096(推理模型的思考也占这个预算)");
        }
    }

    // ── [15] 本轮改动幅度 (保守/平稳/激进) ───────────────────────────────
    //
    // ★★ 为什么这一节必须存在 ★★
    //
    //   用户实测反馈: "我昨天试了两次显示已应用但是体感没什么变化"。
    //   查下来: 三轮加起来 Kp 只动了 10%、Ki 一动没动 —— 因为提示词里写着
    //   "幅度要克制", 而硬夹取只给 ±20%, 两处叠加让模型永远只挪一点点。
    //
    //   ★ 这个旋钮的本质是【两处必须一致】:
    //     提示词告诉模型"你最多能改 ±50%", 而夹取只放 ±20% —— 用户就会看到
    //     "选了激进还是没变化"。所以这一节把两个数字都钉住。
    std::printf("\n[15] 本轮改动幅度: 三档必须同时作用于提示词和硬夹取\n");
    {
        // ① 三档的额度
        check_near(boss::autotune::step_style_rel(boss::autotune::StepStyle::Conservative),
                   0.10, 1e-9, "保守 = ±10%");
        check_near(boss::autotune::step_style_rel(boss::autotune::StepStyle::Steady),
                   0.20, 1e-9, "平稳 = ±20%(与原行为一致)");
        check_near(boss::autotune::step_style_rel(boss::autotune::StepStyle::Aggressive),
                   0.50, 1e-9, "激进 = ±50%");

        // ② beta 的上调额度三档都更小(它补过头会正反馈发散)
        check(boss::autotune::step_style_beta_up(boss::autotune::StepStyle::Conservative) <
              boss::autotune::step_style_rel(boss::autotune::StepStyle::Conservative),
              "★ beta 上调比同档的普通幅度更紧(保守)");
        check(boss::autotune::step_style_beta_up(boss::autotune::StepStyle::Aggressive) <
              boss::autotune::step_style_rel(boss::autotune::StepStyle::Aggressive),
              "★ beta 上调比同档的普通幅度更紧(激进)");
        check_near(boss::autotune::step_style_beta_up(boss::autotune::StepStyle::Aggressive),
                   0.20, 1e-9, "激进档 beta 上调仍是 +20%(不是 +50%)");

        // ③ ★ 硬夹取真的按档位放开 —— 这条是"选了激进却没变化"的直接回归
        {
            boss::autotune::Knobs prev;      // 默认 kp=35
            boss::autotune::Knobs want = prev;
            want.kp = 100.0;                 // 模型想一次跳到 100

            const auto c = boss::autotune::apply_limits(
                prev, want, boss::autotune::StepStyle::Conservative);
            const auto s = boss::autotune::apply_limits(
                prev, want, boss::autotune::StepStyle::Steady);
            const auto a = boss::autotune::apply_limits(
                prev, want, boss::autotune::StepStyle::Aggressive);

            check_near(c.applied.kp, 35.0 * 1.10, 1e-6, "★ 保守: 35 -> 最多 38.5");
            check_near(s.applied.kp, 35.0 * 1.20, 1e-6, "★ 平稳: 35 -> 最多 42.0");
            check_near(a.applied.kp, 35.0 * 1.50, 1e-6, "★ 激进: 35 -> 最多 52.5");

            // 额度必须严格递增, 否则档位形同虚设
            check(a.applied.kp > s.applied.kp && s.applied.kp > c.applied.kp,
                  "★ 三档额度严格递增(激进 > 平稳 > 保守)");

            // 但绝对值域的硬边界【不】因档位放开: kp 上限仍是 300
            boss::autotune::Knobs want2 = prev;
            want2.kp = 5000.0;
            const auto a2 = boss::autotune::apply_limits(
                prev, want2, boss::autotune::StepStyle::Aggressive);
            check(a2.applied.kp <= 300.0,
                  "★ 选激进也不能突破绝对上限(kp <= 300)");
        }

        // ④ 不传 style 时必须是 Steady —— 老调用点行为不变
        {
            boss::autotune::Knobs prev;
            boss::autotune::Knobs want = prev;
            want.kp = 100.0;
            const auto d = boss::autotune::apply_limits(prev, want);   // 两参数
            check_near(d.applied.kp, 42.0, 1e-6,
                       "★ 默认档 = 平稳(±20%), 老调用点不受影响");
        }

        // ⑤ 提示词: 三档必须给出【不同】的幅度说明, 且带上真实百分比
        {
            boss::autotune::TrackSegment seg;
            seg.samples.resize(300);
            seg.t_begin = 0.0; seg.t_end = 2.5;
            seg.coverage = 0.98;
            seg.jumps = 0; seg.gaps = 0;
            seg.bbox_h_median = 120.0;
            seg.bbox_h_p10 = 110.0;
            seg.bbox_h_p90 = 132.0;
            const boss::autotune::Knobs k;
            const boss::autotune::Metrics m;

            auto prompt = [&](boss::autotune::StepStyle st) {
                return boss::autotune::build_user_prompt(
                    k, m, seg, boss::autotune::TuneMode::Static, "", 1, "", st);
            };
            const std::string pc = prompt(boss::autotune::StepStyle::Conservative);
            const std::string ps = prompt(boss::autotune::StepStyle::Steady);
            const std::string pa = prompt(boss::autotune::StepStyle::Aggressive);

            check(pc.find(u8"保守") != std::string::npos, "保守提示词含「保守」");
            check(ps.find(u8"平稳") != std::string::npos, "平稳提示词含「平稳」");
            check(pa.find(u8"激进") != std::string::npos, "激进提示词含「激进」");

            check(pc.find("10%") != std::string::npos, "★ 保守提示词写明 ±10%");
            check(ps.find("20%") != std::string::npos, "★ 平稳提示词写明 ±20%");
            check(pa.find("50%") != std::string::npos, "★ 激进提示词写明 ±50%");

            check(pc != ps && ps != pa && pc != pa,
                  "★ 三档给出不同提示词");

            // ★ 激进档必须明确鼓励用满额度 —— 模型的默认倾向是极度保守,
            //   不给这句话它只会挪一点点, 那正是这次的病。
            check(pa.find(u8"一次调到位") != std::string::npos,
                  "★ 激进档明确要求「一次调到位」");
            check(pa.find(u8"用满") != std::string::npos,
                  "★ 激进档明确要求把额度「用满」");

            // ★ 回归: 旧的"幅度要克制"不能再出现 —— 就是它导致改动过小
            check(pc.find(u8"幅度要克制") == std::string::npos &&
                  ps.find(u8"幅度要克制") == std::string::npos &&
                  pa.find(u8"幅度要克制") == std::string::npos,
                  "★★ 提示词里不再有「幅度要克制」(实测导致改动过小)");
        }

        // ⑥ AutoTuner 的 set/get + 默认值
        {
            boss::autotune::AutoTuner t;
            check(t.step_style() == boss::autotune::StepStyle::Steady,
                  "★ AutoTuner 默认是平稳档");
            t.set_step_style(boss::autotune::StepStyle::Aggressive);
            check(t.step_style() == boss::autotune::StepStyle::Aggressive,
                  "档位可设置/读取");
        }

        // ⑦ 档位名字(UI/历史里显示用)
        check(std::string(boss::autotune::step_style_name(
                  boss::autotune::StepStyle::Conservative)) == u8"保守", "名字: 保守");
        check(std::string(boss::autotune::step_style_name(
                  boss::autotune::StepStyle::Steady)) == u8"平稳", "名字: 平稳");
        check(std::string(boss::autotune::step_style_name(
                  boss::autotune::StepStyle::Aggressive)) == u8"激进", "名字: 激进");
    }

    // ─────────────────────────────────────────────────────────────────────
    std::printf("\n[16] 写回必须【确认生效】, 不能写出去就算成功\n");
    //
    // ★★ 这一节钉的是一个实测事故 (2026-09-15) ★★
    //
    //   用户报: "自动调参不好用, 体感根本不明显"。
    //   查下来【参数压根没生效】:
    //       CF.ini        (active.txt 指着的方案) kp=15.0 ki=5.0
    //       live_tune.ini (实际在跑的)            kp=6.2  ki=3.3
    //   而界面上一直显示"已应用" —— 因为旧写法把"写盘成功"直接当成了成功。
    //
    //   ★ 教训: 【把值写进文件】和【运行时真的用上了】是两件事。
    //     中间隔着轮询、体检、loadConfig、publish 四道关。判据必须取
    //     "运行时那侧可观测的证据", 而不是"我们这边动作做完了"。
    {
        // 造一段能过【全部】可用性判据的数据, 直接把 run_one_round 跑到"落地"那一步。
        const auto make_seg = []() {
            boss::autotune::TrackSegment seg;
            for (int i = 0; i < 400; ++i)
            {
                boss::autotune::Sample s;
                s.valid = true;
                s.t = i * 0.008;
                s.dt_ms = 8.0;
                s.ex = 1.0; s.ey = 0.5;
                s.ex_used = 1.0;
                s.cmdx = 4.0; s.cmdy = 2.0;
                s.bbox_h = 120.0;
                seg.samples.push_back(s);
            }
            seg.t_begin = 0.0;
            seg.t_end = 399 * 0.008;
            seg.coverage = 1.0;
            seg.bbox_h_median = 120.0;
            seg.bbox_h_p10 = 118.0;
            seg.bbox_h_p90 = 122.0;
            return seg;
        };
        // 一个"想改 Kp"的模型回答(走真实的 JSON 解析 + 夹取路径)
        const auto reply_wants_kp_up = [](const boss::autotune::LlmConfig&,
                                          const std::string&,
                                          const std::string&) {
            boss::autotune::LlmResult r;
            r.ok = true;
            r.text = "{\"kp\":42,\"reason\":\"test\"}";
            return r;
        };

        // ① ★★ 写盘成功但运行时没采用 -> 这一轮【不能】报成功 ★★
        //
        //   这就是 2026-09-15 用户实测的那个 bug 的判据: 参数压根没生效,
        //   界面上却显示"已应用"。旧写法(无视 set_ 的返回值)在这里会
        //   返回一个 error 为空的 Round —— 也就是假成功。
        {
            boss::autotune::AutoTuner t;
            t.set_getter([]() { return boss::autotune::Knobs{}; });
            t.set_setter([](const boss::autotune::Knobs&, std::string& err) {
                err = u8"模拟: 运行时不采用";
                return false;          // ★ 写盘成功, 但没生效
            });
            t.set_reply_fn(reply_wants_kp_up);

            const boss::autotune::Round r = t.end_session_for_test(make_seg());
            check(!r.error.empty(),
                  "★★ 写回没生效时, 这一轮必须有错误(不能显示「已应用」)");
            check(r.error.find(u8"运行时不采用") != std::string::npos,
                  "★★ 错误里要带上底层给的原因");
        }

        // ② 写回真的生效 -> 这一轮成功, 且落地的是夹取后的值
        {
            boss::autotune::AutoTuner t;
            boss::autotune::Knobs landed;
            bool wrote = false;
            t.set_getter([]() { return boss::autotune::Knobs{}; });
            t.set_setter([&](const boss::autotune::Knobs& k, std::string&) {
                landed = k;
                wrote = true;
                return true;
            });
            t.set_reply_fn(reply_wants_kp_up);

            const boss::autotune::Round r = t.end_session_for_test(make_seg());
            check(r.error.empty(), "写回生效时这一轮没有错误");
            check(wrote, "★ 确实调用了写回");
            // ★ 默认平稳档 ±20%: Kp 默认 35 -> 最多 42, 正好是模型要的值
            check(std::abs(landed.kp - 42.0) < 1e-6,
                  "★ 落地值经过夹取(35 -> 42, 平稳档上限)");
        }

        // ③ 回滚路径: 检测到发散 -> 自动回滚 -> 回滚本身失败时必须报出来。
        //
        //   ★ 为什么要单独测回滚: 回滚是"安全网", 而安全网自己失败的时候
        //     最不能沉默 —— 用户看到"已回滚"会以为参数退回了安全值, 实际上
        //     还停在那个发散的参数上。
        //   构造: 同一个 tuner 先跑一轮成功(建立 last_good_), 再喂一段
        //   【判为发散】的数据触发回滚, 并让这一次写回失败。
        {
            boss::autotune::AutoTuner t;
            int writes = 0;
            t.set_getter([]() { return boss::autotune::Knobs{}; });
            t.set_setter([&](const boss::autotune::Knobs&, std::string& err) {
                ++writes;
                if (writes >= 2)
                {
                    err = u8"回滚时写回失败";
                    return false;
                }
                return true;               // 第一轮成功 -> last_good_ 有值
            });
            t.set_reply_fn(reply_wants_kp_up);

            // 第一轮: 正常数据, 成功, last_good_ 建立
            const boss::autotune::Round r1 = t.end_session_for_test(make_seg());
            check(r1.error.empty(), "第①轮建立 last_good_ 成功");

            // 第二轮: 造一段【发散】数据(尾段误差中位远大于头段)
            boss::autotune::TrackSegment bad = make_seg();
            for (std::size_t i = 0; i < bad.samples.size(); ++i)
            {
                // 前 3/4 很小, 后 1/4 很大 -> 触发 divergent
                const bool tail = (i > bad.samples.size() * 3 / 4);
                bad.samples[i].ex = tail ? 80.0 : 0.5;
                bad.samples[i].ey = tail ? 80.0 : 0.5;
            }
            const boss::autotune::Round r2 = t.end_session_for_test(bad);
            check(!r2.error.empty(),
                  "★★ 回滚失败时必须有错误(不能让用户以为已退回安全值)");
            check(r2.error.find(u8"回滚") != std::string::npos,
                  "★★ 错误里要说明是回滚没生效");
        }

        // ④ RuntimeHooks::write 的错误必须能穿过 Runtime 传到调用方
        {
            auto& rt = boss::autotune::Runtime::instance();
            boss::autotune::RuntimeHooks hooks;
            hooks.read  = []() { return boss::autotune::Knobs{}; };
            hooks.write = [](const boss::autotune::Knobs&, std::string& err) {
                err = u8"通道被拒";
                return false;
            };
            hooks.read_base_h  = []() { return 0.0; };
            hooks.write_base_h = [](double) { return true; };
            rt.install(hooks);

            std::string err;
            const bool ok = rt.write_knobs_for_test(boss::autotune::Knobs{}, err);
            check(!ok, "★ 写回失败必须返回 false(不能静默成功)");
            check(err.find(u8"通道被拒") != std::string::npos,
                  "★★ 失败原因必须原样带出来, 供界面显示");
        }

        // ⑤ 成功的写回要返回 true
        {
            auto& rt = boss::autotune::Runtime::instance();
            boss::autotune::RuntimeHooks hooks;
            hooks.read  = []() { return boss::autotune::Knobs{}; };
            hooks.write = [](const boss::autotune::Knobs&, std::string&) {
                return true;
            };
            hooks.read_base_h  = []() { return 0.0; };
            hooks.write_base_h = [](double) { return true; };
            rt.install(hooks);

            std::string err;
            const bool ok = rt.write_knobs_for_test(boss::autotune::Knobs{}, err);
            check(ok, "写回成功时返回 true");
            check(err.empty(), "成功时不留错误信息");
        }
    }

    std::printf("\n=== %d 项检查, %d 项失败 ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
