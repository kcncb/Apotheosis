"""第一步证据脚本：临界增益与死区的互证。

不用 scipy（环境无 scipy），只用 math。

结论（运行即得）：
  · 由实机日志 (Kp=100 -> 5.00Hz/200ms 摆动, g=0.494) 反推死区 = 45.8ms，
    与 boss_aim.h 的 kAimDeadTimeS = 0.046 独立吻合。
  · 由 dead time 反推的 g/g_crit = 1.892，与文档记载的 "1.9 倍临界" 吻合。
  · 120fps 下 46ms 对应 d=5.52 拍 -> Kp_crit ≈ 49~58 counts/(px*s) @ k=0.593。

运行：python tools/pid_sim/_verify_crit.py
"""

import math


def g_crit(d_ticks: float) -> float:
    """离散积分 + 纯延迟 d 拍的临界每拍增益。

    g_crit = 2*sin(pi / (2*(2d+1)))
    d 建议向上取整（保守一侧）。
    """
    return 2.0 * math.sin(math.pi / (2.0 * (2.0 * d_ticks + 1.0)))


def kp_crit(d_ticks: float, dt: float, k_px_per_count: float) -> float:
    """把临界每拍增益换算成 Kp [计数/(像素*秒)]。

    g = Kp * dt * k   =>   Kp = g / (dt * k)
    """
    return g_crit(d_ticks) / (dt * k_px_per_count)


def main() -> None:
    print("=" * 72)
    print("A. 由实机日志反推死区，并与 kAimDeadTimeS 互证")
    print("=" * 72)

    # CLAUDE.md: Kp=100 时误差以 5.00 Hz / 200ms 摆动；g = Kp*dt*k = 0.494
    period_ms = 200.0
    dt_ms_120 = 1000.0 / 120.0          # 8.333 ms -> 200ms = 24 拍
    period_ticks = period_ms / dt_ms_120
    # 离散"积分+纯延迟"环路振荡周期 = 2*(2d+1) 拍
    d_meas = (period_ticks - 2.0) / 4.0
    dead_ms = d_meas * dt_ms_120

    g_field = 0.494                      # Kp=100, dt=1/120, k=0.593 实测
    gc = g_crit(d_meas)

    print(f"  日志摆动周期          = {period_ms:.1f} ms = {period_ticks:.1f} 拍 (dt={dt_ms_120:.3f}ms)")
    print(f"  反推 d                = {d_meas:.2f} 拍 = {dead_ms:.1f} ms")
    print(f"  kAimDeadTimeS         = 46.0 ms   -> 差 {abs(dead_ms - 46.0):.1f} ms  [吻合]")
    print(f"  g_crit({d_meas:.1f})          = {gc:.4f}")
    print(f"  实机 g / g_crit       = {g_field / gc:.3f}  (文档记载 ~1.9)  [吻合]")

    print()
    print("=" * 72)
    print("B. dt=1/60 的旧口径（仅作对照，非本方案采用值）")
    print("=" * 72)
    dt60 = 1.0 / 60.0
    d60 = 0.046 / dt60
    print(f"  dt=16.667ms, 46ms = {d60:.3f} 拍 -> floor={math.floor(d60)}, ceil={math.ceil(d60)}")
    for dd in (math.floor(d60), math.ceil(d60)):
        print(f"    d={dd}: g_crit={g_crit(dd):.4f}  "
              f"Kp_crit @k=0.25 = {kp_crit(dd, dt60, 0.25):.1f}  "
              f"@k=0.593 = {kp_crit(dd, dt60, 0.593):.1f}")

    print()
    print("=" * 72)
    print("C. 本方案采用：dt=1/120, dead=46ms  -> Kp 天花板（决定搜索域 [5,60]）")
    print("=" * 72)
    dt = 1.0 / 120.0
    d = 0.046 / dt
    print(f"  46ms = {d:.2f} 拍 -> floor={math.floor(d)}, ceil={math.ceil(d)}")
    for dd in (math.floor(d), math.ceil(d)):
        print(f"    d={dd}: g_crit={g_crit(dd):.4f}  "
              f"Kp_crit @k=0.25 = {kp_crit(dd, dt, 0.25):.1f}  "
              f"@k=0.593 = {kp_crit(dd, dt, 0.593):.1f}  "
              f"@k=1.0 = {kp_crit(dd, dt, 1.0):.1f}")

    print()
    print("  实机观测: Kp=30 稳, Kp=100 抽  -> 天花板落在 (30, 100) 区间内, 见上 [自洽]")

    print()
    print("=" * 72)
    print("D. 安全裕度约束：DE 中强制 g <= 0.6 * g_crit")
    print("=" * 72)
    for kk, label in ((0.25, "k=0.25"), (0.593, "k=0.593 (本机)"), (1.0, "k=1.0")):
        for dd in (math.floor(d), math.ceil(d)):
            safe = 0.6 * gc_safe(dd, dt, kk)
            print(f"  {label:16s} d={dd}: Kp 安全上限 = {safe:.1f}")
    print()
    print("  注: k 摄动 +30% 等价于增益摄动 +30%，会顶到上表边界 —— 见 robustness.py")


def gc_safe(d_ticks: float, dt: float, k: float) -> float:
    return g_crit(d_ticks) / (dt * k)


if __name__ == "__main__":
    main()
