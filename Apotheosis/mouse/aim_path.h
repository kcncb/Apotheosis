#ifndef MOUSE_AIM_PATH_H
#define MOUSE_AIM_PATH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace boss
{

// Per-frame trajectory shaper.
//
// Four modes:
//   Linear  — direct proportional move, identical to ART::drive(). Each
//             frame the cursor steps `speed * err` toward the aim point.
//             No anchoring, no arc; this is the legacy / default behaviour.
//   Bezier  — cubic Bezier from (0,0) to (1,0) with two control points
//             cx1,cy1,cx2,cy2.  X = travel progress along the start→goal
//             chord, Y = perpendicular deviation in units of chord length.
//   Custom  — a high-resolution piecewise-linear deviation curve drawn by the
//             user in the UI. Endpoints are pinned to zero so the path
//             still ends on the goal.
//   WindMouse — 仿 AimMagic 的 enable_mouse_curve: 用 WindMouse 物理模型
//             (重力 G0 / 风力 W0 / 步长 M0 / 距离 D0) 生成一条"像人甩出来的"
//             路径, 再把它重采样成同一条 Y(t) 偏差剖面走上面那套整形机制。
//             额外带 AM 的 curve_threshold 门控: 误差很小时整段曲线旁路,
//             直接走直线 (小修正保精度, 大甩枪才拟人化)。
//
// On lock or large goal drift the driver re-anchors: start = current cursor,
// goal = current aim. Travel progress is then driven by cursor PROJECTION
// onto the start→goal axis (not wall clock), so a moving target slowing
// you down doesn't break the shape; the cursor just takes more frames to
// arrive. Residual pixels from sub-integer moves accumulate the same way
// ART::drive does, so micro-motion still adds up correctly over time.
//
// ★ 四种模式都【只旋转不缩放】控制器原始输出: 曲线给出的是局部切线方向,
//   幅值仍由 PID 决定。这是现役控制回路(46ms 死区 + Smith 在途补偿)不被
//   轨迹层拖成振荡的前提, 加新模式时必须保持。
class AimPathDriver
{
public:
    enum class Mode : int
    {
        Linear = 0,
        Bezier = 1,
        Custom = 2,
        WindMouse = 3,
    };

    static constexpr int kCustomSamples = 32768;
    // WindMouse 路径重采样成多少段 Y(t)。256 段足够平滑, 又不用在鼠标线程上
    // 存一条几千点的折线。
    static constexpr int kWindSamples = 256;
    // 单段路径最多迭代多少步 (L/M0 量级; 纯保护, 正常几十步就收敛)。
    static constexpr int kWindMaxSteps = 4096;

    struct Params
    {
        Mode  mode = Mode::Linear;
        // 只把曲线作为 PIDF 原始方向的影响量。0=完全透传 PIDF，
        // 1=完整曲线切线；默认 25% 避免曲线接管移动。
        double strength = 0.25;

        // Bezier control points (X∈[0,1], Y∈[-1,1]).
        double cx1 = 0.30, cy1 = 0.00;
        double cx2 = 0.70, cy2 = 0.00;

        // Custom curve samples (Y at X = 0..1 in uniform N steps).
        std::shared_ptr<const std::vector<float>> custom_samples;
        bool neural_enabled = false;
        std::array<float, 25> neural_weights{};

        // ── WindMouse (Mode::WindMouse) ────────────────────────────────────
        // 单位都是【像素】, 与 AimMagic 的 wind_mouse_G0/W0/M0/D0 同名同量纲:
        //   wind_gravity   重力(向目标的吸引) —— 越大越"坚决", 路径越直
        //   wind_wind      风力(横向随机游走幅度) —— 越大越飘
        //   wind_step      单步最大长度 —— 越小路径越碎、越慢
        //   wind_distance  风力开始衰减的距离 —— 越接近目标风越小
        //   wind_threshold 门控(px): 两个轴的误差都不超过它时整段曲线旁路。
        //                  AM 的 curve_threshold。默认 10px: 微修正走直线。
        double wind_gravity    = 5.0;
        double wind_wind       = 2.0;
        double wind_step       = 10.0;
        double wind_distance   = 8.0;
        double wind_threshold_px = 10.0;

        // Re-anchor when the goal drifts more than this many pixels from
        // the current path's endpoint. Below this we keep the existing
        // shape (target jitter shouldn't restart the path).
        double reanchor_px = 40.0;
    };

    struct Result
    {
        double move_x = 0;
        double move_y = 0;
    };

    void configure(const Params& p)
    {
        const bool shape_changed =
            p.mode != p_.mode ||
            p.cx1 != p_.cx1 || p.cy1 != p_.cy1 ||
            p.cx2 != p_.cx2 || p.cy2 != p_.cy2 ||
            p.custom_samples != p_.custom_samples ||
            p.neural_enabled != p_.neural_enabled ||
            p.neural_weights != p_.neural_weights ||
            p.wind_gravity != p_.wind_gravity ||
            p.wind_wind != p_.wind_wind ||
            p.wind_step != p_.wind_step ||
            p.wind_distance != p_.wind_distance;
        if (shape_changed)
            reset();
        p_ = p;
    }

    void reset()
    {
        engaged_ = false;
        last_id_ = -1;
        progress_ = 0.0;
        reference_length_ = 1.0;
        axis_x_ = 1.0;
        axis_y_ = 0.0;
        smoothed_slope_ = 0.0;
        rx_ = ry_ = 0.0;
        wind_profile_.clear();
    }

    // 由上层在最终 Y 力度缩放之后回报真正发送的位移。step() 只计算，
    // 不提前假设原始输出一定被采用。
    void applyMove(int dx, int dy)
    {
        (void)dx;
        (void)dy;
    }

    // dt is unused in Linear mode (matches ART); other modes use it only
    // as a soft hint for very slow frames — progress is otherwise driven
    // by cursor projection.
    Result step(double aim_x, double aim_y,
                double cur_x, double cur_y,
                double /*dt*/, int target_id,
                double base_dx, double base_dy,
                double settle_radius_px = 0.0)
    {
        Result out;

        // 直线模式只透传 mover 已经算好的输出。这样 PDFLr / PID / ART 的
        // 增益、阻尼、死区在所有轨迹模式下都保持同一套语义。
        if (p_.mode == Mode::Linear)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double screen_err_x = aim_x - cur_x;
        const double screen_err_y = aim_y - cur_y;
        const double screen_err_mag = std::hypot(screen_err_x, screen_err_y);

        // ── AM 的 curve_threshold 门控 (只对 WindMouse 生效) ──
        // 两个轴的误差都不超过阈值时整段曲线旁路: 微修正走直线(保精度),
        // 只有大甩枪才走拟人路径。判据照抄 AimMagic FUN_140071a60 行 70-79
        // 的逐轴形式 |dx| <= T && |dy| <= T。
        // 旁路期间不维持路径状态: 误差再次超过阈值时重新起一段。
        if (p_.mode == Mode::WindMouse)
        {
            const double gate = std::max(0.0, p_.wind_threshold_px);
            if (std::abs(screen_err_x) <= gate && std::abs(screen_err_y) <= gate)
            {
                engaged_ = false;
                wind_profile_.clear();
                smoothed_slope_ = 0.0;
                rx_ = ry_ = 0.0;
                out.move_x = base_dx;
                out.move_y = base_dy;
                return out;
            }
        }

        // 每次锁定只固定一次轨迹坐标系。检测框的微小抖动不应每帧旋转
        // 曲线的法向量，否则 1~2 px 的目标噪声会被放大成交替侧向位移。
        const bool id_changed = (target_id >= 0 && target_id != last_id_);
        if (!engaged_ || id_changed)
        {
            engaged_ = true;
            last_id_ = target_id;
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            smoothed_slope_ = 0.0;
            rx_ = ry_ = 0.0;
            // WindMouse 的路径是随机生成的, 每段重新摇一条; 段的长度就是
            // 此刻的真实误差, 所以 G0/W0/M0/D0 的像素量纲才有意义。
            if (p_.mode == Mode::WindMouse)
                build_wind_profile(reference_length_, target_id);
        }
        last_id_ = target_id;

        // mover 输出为 0 就必须停。旧实现会绕过 mover 自己的死区/阻尼，
        // 导致已经收敛时曲线仍继续发位移。
        const double base_mag = std::hypot(static_cast<double>(base_dx),
                                           static_cast<double>(base_dy));
        if (base_mag < 0.5)
        {
            rx_ = ry_ = 0.0;
            return out;
        }

        // 极短路径或控制器正在反向制动时直接采用原输出。
        if (screen_err_mag < 1.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 近目标时必须逐渐退回控制器原始方向。否则曲线切线会在目标已经
        // 很近时继续制造侧向分量，移动目标反复开新段后表现为绕目标画圈。
        // settle_radius 由上层按 bbox 大小提供，0 保持旧行为。
        const double settle_radius = std::max(0.0, settle_radius_px);
        if (settle_radius > 0.0 && screen_err_mag <= settle_radius)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double forward = static_cast<double>(base_dx) * screen_err_x
                             + static_cast<double>(base_dy) * screen_err_y;
        if (forward <= 0.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 上一段已到达曲线终点，但移动目标仍让原控制器产生有效
        // 前进输出时，以当前误差开启下一段。否则 progress 永远停在 1，
        // 尾部切线为零的曲线会在第一段之后全部退化成直线。
        if (progress_ >= 1.0 - 1e-9)
        {
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            smoothed_slope_ = 0.0;
            rx_ = ry_ = 0.0;
            if (p_.mode == Mode::WindMouse)
                build_wind_profile(reference_length_, last_id_);
        }

        // 自定义曲线有 32768 个采样点，直接逐点求导会把手绘噪声放大成
        // 每帧方向跳变。先使用宽窗导数，再做跨帧低通；曲线只作为
        // PIDF 原始向量的混合影响，不再接管完整方向。
        const double raw_slope = std::clamp(
            curve_derivative(progress_), -3.0, 3.0);
        constexpr double kSlopeAlpha = 0.18;
        smoothed_slope_ += (raw_slope - smoothed_slope_) * kSlopeAlpha;
        const double dy_dt = smoothed_slope_;

        const auto smoothstep01 = [](double value) {
            const double u = clamp01(value);
            return u * u * (3.0 - 2.0 * u);
        };
        const double entry_fade = smoothstep01(progress_ / 0.10);
        const double exit_fade = smoothstep01((1.0 - progress_) / 0.15);
        double influence = std::clamp(p_.strength, 0.0, 1.0)
                         * entry_fade * exit_fade;
        if (settle_radius > 0.0)
        {
            const double fade_end = settle_radius * 2.5;
            const double u = clamp01((screen_err_mag - settle_radius) /
                                     std::max(1.0, fade_end - settle_radius));
            const double smooth = u * u * (3.0 - 2.0 * u);
            influence *= smooth;
        }
        const double local_scale = std::sqrt(1.0 + dy_dt * dy_dt);
        progress_ = clamp01(progress_ + base_mag /
            std::max(1.0, reference_length_ * local_scale));

        if (std::abs(dy_dt) < 1e-9 || influence < 1e-4)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 在锁定时的稳定坐标系内，只按曲线局部切线旋转原 mover
        // 输出，不改变幅值。平坦曲线 dy/dt=0 时必须与直线模式逐像素一致。
        const double perp_x = -axis_y_;
        const double perp_y =  axis_x_;
        const double local_x = static_cast<double>(base_dx) * axis_x_
                             + static_cast<double>(base_dy) * axis_y_;
        const double local_y = static_cast<double>(base_dx) * perp_x
                             + static_cast<double>(base_dy) * perp_y;
        const double inv_scale = 1.0 / local_scale;
        const double cos_a = inv_scale;
        const double sin_a = dy_dt * inv_scale;
        const double shaped_local_x = local_x * cos_a - local_y * sin_a;
        const double shaped_local_y = local_x * sin_a + local_y * cos_a;
        const double curved_x = shaped_local_x * axis_x_ + shaped_local_y * perp_x;
        const double curved_y = shaped_local_x * axis_y_ + shaped_local_y * perp_y;

        // 在线性 PIDF 向量与完整曲线切线之间插值，再恢复原始幅值。
        // 因此曲线只改变一部分方向，不会降低/放大 PIDF 的追踪力度。
        double step_x = static_cast<double>(base_dx)
                      + (curved_x - static_cast<double>(base_dx)) * influence;
        double step_y = static_cast<double>(base_dy)
                      + (curved_y - static_cast<double>(base_dy)) * influence;
        const double mixed_mag = std::hypot(step_x, step_y);
        if (mixed_mag > 1e-9)
        {
            const double restore = base_mag / mixed_mag;
            step_x *= restore;
            step_y *= restore;
        }

        const double raw_x = step_x + rx_;
        const double raw_y = step_y + ry_;

        out.move_x = raw_x;
        out.move_y = raw_y;
        rx_ = 0;
        ry_ = 0;
        return out;
    }

private:
    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    // ── WindMouse 路径生成 (仿 AimMagic 的 enable_mouse_curve) ──────────────
    //
    // 在【局部坐标系】里跑一遍 WindMouse: 起点 (0,0)、终点 (L,0), L 是本段
    // 锁定时的真实像素误差。跑出来的折线再按"进度 → 侧向偏移 / L"重采样成
    // kWindSamples 段的归一化偏差剖面, 交给下面 curve_y()/curve_derivative()
    // 与 Bezier/Custom 完全一样的整形路径使用。
    //
    // 这样做的两个理由:
    //   ① G0/W0/M0/D0 的量纲是像素(AM 原样), 必须在真实尺度上生成才有意义;
    //   ② 路径只提供【局部切线】, 幅值仍由 PID 决定 —— 不会和控制器打架。
    void build_wind_profile(double length_px, int target_id)
    {
        wind_profile_.assign(kWindSamples, 0.0f);
        std::vector<char> filled(kWindSamples, 0);

        // 把目标 id 混进种子: 每锁定一个新目标都摇出不同的路径 —— 否则每一枪
        // 抖成同一条, 既不自然也容易被看出来。同一串输入(含 id)仍然完全可复现,
        // 回归测试才能断言具体位移。
        rng_ ^= static_cast<std::uint32_t>(target_id) * 0x9E3779B1u;
        rng_ |= 1u;   // xorshift32 的全零状态是吸收态

        const double L = std::max(1.0, length_px);
        const double gravity  = std::clamp(p_.wind_gravity, 0.0, 200.0);
        const double wind     = std::clamp(p_.wind_wind, 0.0, 200.0);
        const double dist0    = std::clamp(p_.wind_distance, 0.1, 200.0);
        double step_max       = std::clamp(p_.wind_step, 0.1, 200.0);

        constexpr double kSqrt3 = 1.7320508075688772;
        constexpr double kSqrt5 = 2.2360679774997896;

        // 固定种子的 xorshift32: 同一段锁定生成同一条路径, 回归测试可复现;
        // 每次起新段推进一次状态, 所以连续几段不会长得一模一样。
        auto rand01 = [this]() {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            return static_cast<double>(rng_) / 4294967296.0;
        };

        auto record = [&](double x, double y) {
            const double t = clamp01(x / L);
            int idx = static_cast<int>(std::lround(t * (kWindSamples - 1)));
            idx = std::clamp(idx, 0, kWindSamples - 1);
            wind_profile_[static_cast<size_t>(idx)] = static_cast<float>(y / L);
            filled[static_cast<size_t>(idx)] = 1;
        };

        double cx = 0.0, cy = 0.0;
        double vx = 0.0, vy = 0.0;
        double wx = 0.0, wy = 0.0;
        double dist = L;
        record(0.0, 0.0);

        for (int guard = 0; guard < kWindMaxSteps && dist >= 1.0; ++guard)
        {
            const double w_mag = std::min(wind, dist);
            if (dist >= dist0)
            {
                wx = wx / kSqrt3 + (2.0 * rand01() - 1.0) * w_mag / kSqrt5;
                wy = wy / kSqrt3 + (2.0 * rand01() - 1.0) * w_mag / kSqrt5;
            }
            else
            {
                wx /= kSqrt3;
                wy /= kSqrt3;
                if (step_max < 3.0)
                    step_max = rand01() * 3.0 + 3.0;
                else
                    step_max /= kSqrt5;
            }

            vx += wx + gravity * (L - cx) / dist;
            vy += wy + gravity * (0.0 - cy) / dist;

            const double v_mag = std::hypot(vx, vy);
            if (v_mag > step_max)
            {
                const double v_clip = step_max / 2.0 + rand01() * step_max / 2.0;
                vx = vx / v_mag * v_clip;
                vy = vy / v_mag * v_clip;
            }

            cx += vx;
            cy += vy;
            dist = std::hypot(L - cx, 0.0 - cy);
            record(cx, cy);
        }

        // 端点必须落在目标上 (和自定义曲线同一个不变式), 否则尾部会留一个
        // 永远收不掉的侧向偏移。
        wind_profile_.front() = 0.0f;
        wind_profile_.back()  = 0.0f;
        filled.front() = 1;
        filled.back()  = 1;

        // 稀疏区间线性补齐: 路径点数与采样数不一定对得上, 空桶会变成
        // 突兀的阶梯, 求导后就是每帧方向跳变。
        int prev = -1;
        for (int i = 0; i < kWindSamples; ++i)
        {
            if (!filled[static_cast<size_t>(i)])
                continue;
            if (prev >= 0 && i - prev > 1)
            {
                const double y0 = wind_profile_[static_cast<size_t>(prev)];
                const double y1 = wind_profile_[static_cast<size_t>(i)];
                for (int k = prev + 1; k < i; ++k)
                {
                    const double f = static_cast<double>(k - prev)
                                   / static_cast<double>(i - prev);
                    wind_profile_[static_cast<size_t>(k)] =
                        static_cast<float>(y0 + (y1 - y0) * f);
                }
            }
            prev = i;
        }
    }

    // Evaluate the deviation curve Y at progress t∈[0,1].
    double curve_y(double t) const
    {
        if (p_.mode == Mode::Bezier)
        {
            // Solve cubic Bezier in X for parameter u s.t. X(u)=t, then
            // return Y(u). Iterative Newton (6 iters is enough for px work).
            double u = t;
            for (int i = 0; i < 6; ++i)
            {
                const double x  = bezier_axis(u, 0.0, p_.cx1, p_.cx2, 1.0);
                const double dx = bezier_deriv(u, 0.0, p_.cx1, p_.cx2, 1.0);
                if (std::abs(dx) < 1e-6) break;
                u -= (x - t) / dx;
                u  = clamp01(u);
            }
            return bezier_axis(u, 0.0, p_.cy1, p_.cy2, 0.0);
        }
        if (p_.mode == Mode::Custom)
        {
            if (p_.neural_enabled)
            {
                const double x = t * 2.0 - 1.0;
                double raw = static_cast<double>(p_.neural_weights[24]);
                for (int h = 0; h < 8; ++h)
                {
                    const double activation = std::tanh(
                        static_cast<double>(p_.neural_weights[h]) * x +
                        static_cast<double>(p_.neural_weights[8 + h]));
                    raw += static_cast<double>(p_.neural_weights[16 + h]) * activation;
                }
                return std::tanh(raw) * 4.0 * t * (1.0 - t);
            }
            if (!p_.custom_samples) return 0.0;
            const auto& samples = *p_.custom_samples;
            const int N = static_cast<int>(samples.size());
            if (N < 2) return 0.0;
            const double pos = t * (N - 1);
            int i0 = static_cast<int>(std::floor(pos));
            int i1 = i0 + 1;
            if (i0 < 0)      { i0 = 0;     i1 = 1; }
            if (i1 > N - 1)  { i1 = N - 1; i0 = i1 - 1; }
            const double f = pos - i0;
            const double y0 = static_cast<double>(samples[i0]);
            const double y1 = static_cast<double>(samples[i1]);
            return y0 + (y1 - y0) * f;
        }
        if (p_.mode == Mode::WindMouse)
        {
            return sample_profile(wind_profile_, t);
        }
        return 0.0;
    }

    // 折线剖面求值 (和自定义曲线同一套线性插值)。
    static double sample_profile(const std::vector<float>& profile, double t)
    {
        const int n = static_cast<int>(profile.size());
        if (n < 2)
            return 0.0;
        const double pos = clamp01(t) * (n - 1);
        int i0 = static_cast<int>(std::floor(pos));
        int i1 = i0 + 1;
        if (i0 < 0)     { i0 = 0;     i1 = 1; }
        if (i1 > n - 1) { i1 = n - 1; i0 = i1 - 1; }
        const double f = pos - i0;
        return static_cast<double>(profile[static_cast<size_t>(i0)]) * (1.0 - f)
             + static_cast<double>(profile[static_cast<size_t>(i1)]) * f;
    }

    double curve_derivative(double t) const
    {
        // 约覆盖 32768 点曲线的 196 个采样，避免手绘局部毛刺被求导放大。
        // WindMouse 的剖面只有 256 段, 同样的 0.003 窗口小于一个采样间隔,
        // 会在桶边界上得到分段常数的切线 —— 放宽到约 4 段, 切线才连续。
        const double eps = (p_.mode == Mode::WindMouse)
                               ? 4.0 / (kWindSamples - 1)
                               : 0.003;
        const double ta = std::max(0.0, t - eps);
        const double tb = std::min(1.0, t + eps);
        return (tb > ta) ? (curve_y(tb) - curve_y(ta)) / (tb - ta) : 0.0;
    }

    static double bezier_axis(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return m*m*m*a + 3.0*m*m*u*b + 3.0*m*u*u*c + u*u*u*d;
    }
    static double bezier_deriv(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return 3.0*m*m*(b - a) + 6.0*m*u*(c - b) + 3.0*u*u*(d - c);
    }

    Params p_{};
    bool engaged_ = false;
    int  last_id_ = -1;
    double progress_ = 0.0;
    double reference_length_ = 1.0;
    double axis_x_ = 1.0, axis_y_ = 0.0;
    double smoothed_slope_ = 0.0;
    double rx_ = 0.0, ry_ = 0.0;
    // WindMouse 每段路径的归一化偏差剖面 + 固定种子的 PRNG 状态。
    std::vector<float> wind_profile_;
    std::uint32_t rng_ = 0x9E3779B9u;
};

} // namespace boss

#endif // MOUSE_AIM_PATH_H
