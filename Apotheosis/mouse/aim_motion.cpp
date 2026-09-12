#include "aim_motion.h"

#include <algorithm>
#include <cmath>

namespace boss
{
namespace
{

// 历史保留时长与样本上限: 要盖住拟合窗口(用户可调)与"在途计数"查询窗口, 再留余量。
constexpr double kHistorySpanS = 1.2;
constexpr std::size_t kHistoryMaxSamples = 4096;

// 两次有效拟合之间至少隔这么久: 相邻窗口几乎重合, 结果高度相关, 取中位数就没意义了。
constexpr double kFitMinGapS = 0.12;

double median_of(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    const double hi = values[mid];
    if (values.size() % 2 != 0)
        return hi;
    const double lo = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}

// 解 3x3 线性方程组(克拉默法则)。奇异返回 false。
bool solve3(const double m[3][3], const double b[3], double out[3])
{
    const double det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-12 || !std::isfinite(det))
        return false;

    for (int col = 0; col < 3; ++col)
    {
        double tmp[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                tmp[r][c] = (c == col) ? b[r] : m[r][c];
        const double d =
            tmp[0][0] * (tmp[1][1] * tmp[2][2] - tmp[1][2] * tmp[2][1]) -
            tmp[0][1] * (tmp[1][0] * tmp[2][2] - tmp[1][2] * tmp[2][0]) +
            tmp[0][2] * (tmp[1][0] * tmp[2][1] - tmp[1][1] * tmp[2][0]);
        out[col] = d / det;
        if (!std::isfinite(out[col]))
            return false;
    }
    return true;
}

} // namespace

void AnchorMotionEstimator::configure(const AnchorMotionConfig& config)
{
    AnchorMotionConfig c = config;
    c.fit_window_s = std::clamp(c.fit_window_s, 0.05, 1.0);
    c.min_samples = std::clamp(c.min_samples, 4, 2048);
    c.min_counts_span = std::max(0.0, c.min_counts_span);
    c.min_anchor_span_px = std::max(0.0, c.min_anchor_span_px);
    c.max_time_counts_corr = std::clamp(c.max_time_counts_corr, 0.5, 0.9999);
    c.max_fit_rms_px = std::max(0.1, c.max_fit_rms_px);
    c.px_per_count_min = std::max(1e-4, c.px_per_count_min);
    c.px_per_count_max = std::max(c.px_per_count_min * 2.0, c.px_per_count_max);
    c.max_fits = std::clamp(c.max_fits, 1, 32);
    c.min_fits_ready = std::clamp(c.min_fits_ready, 1, c.max_fits);
    c.velocity_window_s = std::clamp(c.velocity_window_s, 0.01, 0.5);
    c.velocity_tau_s = std::clamp(c.velocity_tau_s, 0.001, 1.0);

    config_ = c;
    fits_.assign(static_cast<std::size_t>(c.max_fits), Fit{});
    fit_next_ = 0;
    fit_count_ = 0;
    last_fit_t_ = -1.0;
    px_per_count_ = 0.0;
    resetTargetMotion();
}

void AnchorMotionEstimator::resetTargetMotion()
{
    history_.clear();
    now_ = 0.0;
    total_counts_ = 0.0;
    have_last_ = false;
    last_anchor_ = 0.0;
    velocity_raw_ = 0.0;
    velocity_ = 0.0;
    velocity_ready_ = false;
    velocity_initialized_ = false;
}

void AnchorMotionEstimator::resetCalibration()
{
    std::fill(fits_.begin(), fits_.end(), Fit{});
    fit_count_ = 0;
    fit_next_ = 0;
    last_fit_t_ = -1.0;
    px_per_count_ = 0.0;
    resetTargetMotion();
}

void AnchorMotionEstimator::setManualPxPerCount(double value)
{
    // 只接受物理上讲得通的范围; 其余(0/负数/离谱值)一律当"自动估算"。
    if (!std::isfinite(value) || value <= 0.0 || value > 20.0)
        manual_px_per_count_ = 0.0;
    else
        manual_px_per_count_ = value;
}

bool AnchorMotionEstimator::calibrationReady() const
{
    // 手填了值就算 ready(不用等拟合); 这是刻意的: 用户填的数就是他要的, 不该被
    // "画面太乱所以量不出来"这种事悄悄关掉前馈。
    return pxPerCount() > 0.0;
}

bool AnchorMotionEstimator::velocityReady() const
{
    return velocity_ready_ && calibrationReady();
}

double AnchorMotionEstimator::pxPerCount() const
{
    return manual_px_per_count_ > 0.0 ? manual_px_per_count_ : px_per_count_;
}

double AnchorMotionEstimator::targetVelocityPxPerSec() const
{
    return velocityReady() ? velocity_ : 0.0;
}

const AnchorMotionEstimator::Sample* AnchorMotionEstimator::sample_at_or_before(double t) const
{
    if (history_.empty())
        return nullptr;
    std::size_t lo = 0;
    std::size_t hi = history_.size();
    while (lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (history_[mid].t <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return nullptr;  // 历史覆盖不到 t
    return &history_[lo - 1];
}

void AnchorMotionEstimator::prune()
{
    while (!history_.empty() && now_ - history_.front().t > kHistorySpanS)
        history_.erase(history_.begin());
    if (history_.size() > kHistoryMaxSamples)
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - kHistoryMaxSamples));
}

void AnchorMotionEstimator::push_sample(double anchor, int counts_sent, double dt)
{
    // 先把锚点和'造成它的那批计数'对齐, 再推进时间与累计量。
    double aligned = total_counts_ + static_cast<double>(counts_sent);
    if (!history_.empty())
    {
        const double want = now_ + dt - config_.command_lag_s;
        if (const Sample* past = sample_at_or_before(want))
            aligned = past->counts;
        else
            aligned = history_.front().counts;  // 历史还不够长, 用最老的一拍
    }
    now_ += dt;
    total_counts_ += static_cast<double>(counts_sent);
    history_.push_back(Sample{now_, anchor, total_counts_, aligned});
    prune();
}

void AnchorMotionEstimator::update(double anchor_px, int counts_sent, double dt,
                                   bool observation_valid)
{
    if (!std::isfinite(dt) || dt <= 0.0)
        dt = 1.0 / 120.0;

    if (!std::isfinite(anchor_px))
        observation_valid = false;

    if (observation_valid)
        last_anchor_ = anchor_px;
    else if (!have_last_)
        return;  // 还没有任何真实观测

    // 预测框(tracker 外推出来的锚点)不能拿来标定: 把它压成上一次真实观测值,
    // 只让计数历史继续前进。
    push_sample(last_anchor_, counts_sent, dt);
    have_last_ = true;

    try_fit();
    publish_from_fits(dt);
    update_target_velocity(dt);
}

// 稳态跟枪时窗口拟合会退化(U(t) 几乎是一条直线, k 与 v 不可分), 所以速度不能只靠
// 拟合给 —— 否则一路匀速跟下去, 速度读数会停在很久以前的那次拟合上(实测: 目标从静止
// 突然加速到 300px/s 之后 v̂ 还停在 0, 前馈里"在途自身位移"那一项没人抵消, 反而把稳态
// 误差从 13px 推到 31px)。这里改用已经标定出来的 k̂ 反算:
//     目标自身速度 = 画面里看到的移动 + k̂ * 我们自己的计数速率
// k̂ 一旦可信, 这个式子在稳态和机动时都成立。
void AnchorMotionEstimator::update_target_velocity(double dt)
{
    if (!calibrationReady() || !have_last_ || history_.size() < 2)
    {
        velocity_ready_ = false;
        return;
    }

    const Sample& newest = history_.back();
    const Sample* oldest = sample_at_or_before(newest.t - config_.velocity_window_s);
    if (oldest == nullptr || oldest == &newest)
    {
        velocity_ready_ = false;
        return;
    }
    const double span = newest.t - oldest->t;
    if (span < 0.5 * config_.velocity_window_s)
    {
        velocity_ready_ = false;
        return;
    }

    const double anchor_rate = (newest.anchor - oldest->anchor) / span;
    const double count_rate = (newest.counts_aligned - oldest->counts_aligned) / span;
    const double raw = anchor_rate + px_per_count_ * count_rate;
    if (!std::isfinite(raw))
    {
        velocity_ready_ = false;
        return;
    }

    if (!velocity_initialized_)
    {
        velocity_ = raw;
        velocity_initialized_ = true;
    }
    else
    {
        const double alpha = std::clamp(dt / (config_.velocity_tau_s + dt), 0.0, 1.0);
        velocity_ += (raw - velocity_) * alpha;
    }
    velocity_ready_ = true;
}

void AnchorMotionEstimator::try_fit()
{
    // 防御: 没有可写的拟合槽位就干脆不工作。正常路径下构造函数已经配好了,
    // 但 2026-09-12 那次"首次触发就闪退"就是因为没配好却往下走(长度 0 的数组)。
    if (fits_.empty() || config_.max_fits <= 0)
        return;
    if (history_.size() < static_cast<std::size_t>(config_.min_samples))
        return;

    const Sample& newest = history_.back();
    if (last_fit_t_ >= 0.0 && newest.t - last_fit_t_ < kFitMinGapS)
        return;

    // 收集窗口内的样本。
    const double from = newest.t - config_.fit_window_s;
    std::vector<const Sample*> window;
    window.reserve(history_.size());
    for (const Sample& s : history_)
    {
        if (s.t >= from)
            window.push_back(&s);
    }
    if (static_cast<int>(window.size()) < config_.min_samples)
        return;

    const double counts_span = window.back()->counts_aligned - window.front()->counts_aligned;
    const double anchor_span = window.back()->anchor - window.front()->anchor;
    if (std::abs(counts_span) < config_.min_counts_span)
        return;  // 没有指令激励: 这个窗口分不出 k
    if (std::abs(anchor_span) < config_.min_anchor_span_px)
        return;  // 锚点几乎没动: 没有信号

    // 中心化 + 归一化, 让法方程的数值条件与量纲无关。
    const std::size_t n = window.size();
    double t_mean = 0.0;
    double u_mean = 0.0;
    for (const Sample* s : window)
    {
        t_mean += s->t;
        u_mean += s->counts_aligned;
    }
    t_mean /= static_cast<double>(n);
    u_mean /= static_cast<double>(n);

    double t_var = 0.0;
    double u_var = 0.0;
    for (const Sample* s : window)
    {
        t_var += (s->t - t_mean) * (s->t - t_mean);
        u_var += (s->counts_aligned - u_mean) * (s->counts_aligned - u_mean);
    }
    const double t_scale = std::sqrt(t_var / static_cast<double>(n));
    const double u_scale = std::sqrt(u_var / static_cast<double>(n));
    if (!(t_scale > 1e-9) || !(u_scale > 1e-9))
        return;

    // t 与 U 若高度线性相关, k 与 v 就不可分(稳态跟枪就是这种情况), 直接放弃。
    double corr = 0.0;
    for (const Sample* s : window)
        corr += ((s->t - t_mean) / t_scale) * ((s->counts_aligned - u_mean) / u_scale);
    corr /= static_cast<double>(n);
    if (std::abs(corr) > config_.max_time_counts_corr)
        return;

    // 基函数 phi = [1, t', -U'], 拟合 a = c + v'*t' - k'*U'
    double m[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    double b[3] = {0.0, 0.0, 0.0};
    for (const Sample* s : window)
    {
        const double t_n = (s->t - t_mean) / t_scale;
        const double u_n = (s->counts_aligned - u_mean) / u_scale;
        const double phi[3] = {1.0, t_n, -u_n};
        for (int r = 0; r < 3; ++r)
        {
            for (int c2 = 0; c2 < 3; ++c2)
                m[r][c2] += phi[r] * phi[c2];
            b[r] += phi[r] * s->anchor;
        }
    }

    double x[3] = {0.0, 0.0, 0.0};
    if (!solve3(m, b, x))
        return;

    const double k = x[2] / u_scale;          // 反归一化: U 的单位是计数
    const double v = x[1] / t_scale;          // 反归一化: t 的单位是秒
    const double c = x[0] - x[1] * (t_mean / t_scale) + x[2] * (u_mean / u_scale);
    if (!std::isfinite(k) || !std::isfinite(v))
        return;
    if (k < config_.px_per_count_min || k > config_.px_per_count_max)
        return;

    // 残差: 模型对不上(有跳变、有 tracker 外推、有别的运动)就别用这次拟合。
    double sq = 0.0;
    for (const Sample* s : window)
    {
        const double predicted = c + v * s->t - k * s->counts_aligned;
        const double e = s->anchor - predicted;
        sq += e * e;
    }
    const double rms = std::sqrt(sq / static_cast<double>(n));
    if (!std::isfinite(rms) || rms > config_.max_fit_rms_px)
        return;

    fits_[static_cast<std::size_t>(fit_next_)] = Fit{k, v, rms};
    fit_next_ = (fit_next_ + 1) % config_.max_fits;
    fit_count_ = std::min(fit_count_ + 1, config_.max_fits);
    last_fit_t_ = newest.t;
}

void AnchorMotionEstimator::publish_from_fits(double dt)
{
    if (fit_count_ < config_.min_fits_ready || fits_.empty())
    {
        px_per_count_ = 0.0;
        velocity_ready_ = false;
        return;
    }
    fit_count_ = std::min(fit_count_, static_cast<int>(fits_.size()));

    std::vector<double> ks;
    std::vector<double> vs;
    ks.reserve(static_cast<std::size_t>(fit_count_));
    vs.reserve(static_cast<std::size_t>(fit_count_));
    for (int i = 0; i < fit_count_; ++i)
    {
        ks.push_back(fits_[static_cast<std::size_t>(i)].px_per_count);
        vs.push_back(fits_[static_cast<std::size_t>(i)].velocity);
    }

    const double k_center = median_of(ks);
    // 稳健聚合: 只取中位数附近的内点, 离群的窗口丢掉而不是把整份估计作废。
    double spread = 0.0;
    for (double value : ks)
        spread = std::max(spread, std::abs(value - k_center));
    const double tolerance = std::max(0.35 * k_center, 1e-6);
    std::vector<double> k_in;
    std::vector<double> v_in;
    for (std::size_t i = 0; i < ks.size(); ++i)
    {
        if (std::abs(ks[i] - k_center) <= tolerance)
        {
            k_in.push_back(ks[i]);
            v_in.push_back(vs[i]);
        }
    }
    if (static_cast<int>(k_in.size()) < config_.min_fits_ready)
    {
        px_per_count_ = 0.0;
        velocity_ready_ = false;
        return;
    }

    const double k_final = median_of(k_in);
    if (k_final < config_.px_per_count_min || k_final > config_.px_per_count_max)
    {
        px_per_count_ = 0.0;
        velocity_ready_ = false;
        return;
    }
    px_per_count_ = k_final;

    // 速度不在这里发布: 拟合给出的 v 在稳态窗口里会退化, 统一由
    // update_target_velocity() 用 k̂ 反算(见那里的说明)。
    (void)v_in;
}

double AnchorMotionEstimator::countsDuring(double seconds) const
{
    if (history_.empty() || !(seconds > 0.0))
        return 0.0;
    const Sample& newest = history_.back();
    const Sample* oldest = sample_at_or_before(newest.t - seconds);
    // 历史不够长时用最老的一拍, 相当于"少补一点", 不会过度补偿。
    const double from = oldest != nullptr ? oldest->counts : history_.front().counts;
    return newest.counts - from;
}

} // namespace boss
