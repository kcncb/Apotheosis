#pragma once

// α-β 滤波（默认实现）
//
// ★ 平滑强度是**编译期常数**，不做成用户旋钮（control-architecture.md:73-75）：
//   "平滑强度与 Kp 耦合，两个一起调极容易调乱"。
//
// ★ 速度只是内部状态，不外传（见 filter.h 的说明）。

#include "filter.h"

namespace control {

// 30ms —— ★ 历史实测值（control-architecture.md），不要随手改。
// 它的来历：α-β 的等效时间常数是靠 G1/G2 阶跃表实测定下来的，
// 不是照搬教科书。
inline constexpr double kAnchorFilterTauMs = 30.0;

struct AlphaBetaParams
{
    double tauMs = kAnchorFilterTauMs;
};

class AlphaBetaFilter : public IFilter
{
public:
    explicit AlphaBetaFilter(const AlphaBetaParams& params = {});

    void observe(const Vec2& center, double dtSeconds) override;
    Vec2 position() const override;
    bool initialized() const override { return initialized_; }
    void reset() override;

    // α-β 的两条更新律（位置 + 速度），导出以便单测直接钉住它们。
    //   α = 位置修正比例, β = 速度修正比例
    // ★ 对外仍然只有 observe() 一条路径 —— 这两个函数是给测试用的。
    static double alphaForTau(double tauSeconds, double dtSeconds);
    static double betaForTau(double tauSeconds, double dtSeconds);

private:
    AlphaBetaParams params_;
    Vec2 estimate_;      // 滤波后的位置
    Vec2 velocity_;      // ★ 内部状态，不外传
    bool initialized_ = false;
};

} // namespace control
