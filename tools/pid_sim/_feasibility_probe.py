"""第一步辅助脚本：约束可行性扫描。

目的只有一个 —— 在写 DE 之前先回答「超调<10% + ts<1s 到底有没有解」。
没有解的话后面全部白做。

被控对象与 tests/aim_pid_test.cpp 同构（测量延迟 + 执行延迟 + k px/count）。
本脚本是【单点扫描】(固定 Ki/Kd/饱和)，不是寻优；寻优归 de_optimize.py。

运行：python tools/pid_sim/_feasibility_probe.py
"""

import math

import numpy as np

DT = 1.0 / 120.0          # 由实机日志反推（见 _verify_crit.py 证据 A）
DEAD_S = 0.046            # kAimDeadTimeS
K_PX_PER_COUNT = 0.593    # 本机实测 k̂
D_TAU = 0.020             # 微分低通 tau，沿用 aim_pid.cpp
LIMIT = 200               # kDefaultLimitCounts


def simulate(Kp, Ki, Kd, p_sat, limit, *, dt=DT, k=K_PX_PER_COUNT,
             dead=DEAD_S, step_px=400.0, duration=3.0):
    """同构被控对象上的闭环仿真。

    返回 (overshoot_ratio, settle_s, iae, peak_abs)。
    settle 口径: 最后一次离开 ±2% 阶跃带之后不再进入；-1 表示全程没进带。
    """
    # 死区拆两段: 2/3 执行, 1/3 测量（总和守恒，总和才是稳定性的决定量）
    act_ticks = max(1, int(round((2.0 / 3.0 * dead) / dt)))
    meas_ticks = max(0, int(round((1.0 / 3.0 * dead) / dt)))

    act_line = np.zeros(act_ticks)
    meas_line = np.zeros(meas_ticks)

    target = step_px
    camera = 0.0
    n = int(round(duration / dt))

    integral = 0.0
    d_filt = 0.0
    prev_e = 0.0
    carry = 0.0
    first = True

    errs = np.zeros(n)

    for i in range(n):
        # 测量延迟线：当前真实误差入线，最老的出线进控制器
        if meas_ticks > 0:
            meas_line = np.roll(meas_line, 1)
            meas_line[0] = target - camera
            e = meas_line[-1]
        else:
            e = target - camera

        e_sat = p_sat * math.tanh(e / p_sat) if p_sat > 0 else e

        d_raw = 0.0 if first else (e - prev_e) / dt
        alpha = dt / (D_TAU + dt)
        d_filt += alpha * (d_raw - d_filt)
        prev_e = e
        first = False

        u_raw = dt * Kp * (e_sat + Ki * integral + Kd * d_filt)
        u = float(np.clip(u_raw, -limit, limit))

        # 抗积分饱和: back-calculation (Tt = sqrt(Ti*Td) 的经典 IM C 法则)
        if Ki > 0.0 and Kp > 0.0:
            Ti = 1.0 / Ki
            Td = Kd if Kd > 0.0 else dt
            Tt = max(math.sqrt(Ti * Td), dt)
            integral += (u - u_raw) / (dt * Kp * Ki) * (dt / Tt)
        else:
            integral += e_sat * dt

        carry += u
        counts = float(np.round(carry))
        carry -= counts

        # 执行延迟线
        act_line = np.roll(act_line, 1)
        act_line[0] = counts * k
        camera += act_line[-1]

        errs[i] = target - camera

    overshoot = max(0.0, -float(errs.min())) / step_px
    band = 0.02 * step_px
    idx = np.where(np.abs(errs) > band)[0]
    if idx.size == 0:
        settle = 0.0
    else:
        last = int(idx[-1])
        settle = (last + 1) * dt if last + 1 < n else -1.0
    iae = float(np.sum(np.abs(errs)) * dt)
    return overshoot, settle, iae, float(np.abs(errs).max())


def main() -> None:
    print("=" * 78)
    print("约束可行性扫描 (400px 阶跃, k=0.593, 46ms 死区, 饱和 100px, 限幅 200)")
    print("=" * 78)
    print(f"{'Kp':>5} {'超调%':>8} {'ts(s)':>8} {'IAE':>9}  判定")
    for Kp in (20, 25, 29, 35, 45, 60, 90, 120):
        os_, ts_, iae, _pk = simulate(Kp, 1.0, 0.01, 100.0, LIMIT)
        ok = (os_ < 0.10) and (0.0 <= ts_ < 1.0)
        print(f"{Kp:>5} {os_ * 100:>8.2f} {ts_:>8.3f} {iae:>9.1f}  "
              f"{'PASS' if ok else 'fail'}")

    print()
    print("=" * 78)
    print("安全裕度上限 vs IAE 最优点的冲突（见 DESIGN.md §3.3.1/§3.3.2）")
    print("=" * 78)
    gc = 2.0 * math.sin(math.pi / (2.0 * (2.0 * 5.0 + 1.0)))   # d=5
    kp_safe = 0.6 * gc / (DT * K_PX_PER_COUNT)
    print(f"  d=5 时 g_crit = {gc:.4f}, 0.6 倍裕度下 Kp 安全上限 = {kp_safe:.1f}")
    print(f"  -> 搜索域 [5,60] 的上端被该约束压到约 {kp_safe:.0f}")
    print("  -> 报告须同时给出「压裕度前/后」最优解，量化裕度的 IAE 代价")


if __name__ == "__main__":
    main()
