#include "aim_controller.h"

#include "alpha_beta_filter.h"

#include <cmath>

namespace control {

AimController::AimController()
{
    filter_ = std::make_unique<AlphaBetaFilter>();
}

AimController::~AimController() = default;

void AimController::setFilter(std::unique_ptr<IFilter> filter)
{
    // ★★ 这里是"二选一"的落点：赋值即替换，**不存在"两个同时生效"的状态**。
    //   传 nullptr 恢复默认 α-β。这正是 §3.5.1 禁止串联的实现形态 ——
    //   不是靠运行时检查，而是靠"类型上就只有一个成员"。
    filter_ = filter ? std::move(filter) : std::make_unique<AlphaBetaFilter>();
    if (filter_)
        filter_->reset();
}

void AimController::reset()
{
    selectorState_.reset();
    stabilizerState_.reset();
    if (filter_)
        filter_->reset();
    pid_.reset();
    hasLastBox_ = false;
    lastBox_ = Box{};
}

ControlOutput AimController::update(const ControlInput& in)
{
    ControlOutput out;
    out.cross = in.cross;

    // ── dt 门禁 ────────────────────────────────────────────────────────
    if (!(in.dtSec > 0.0))
    {
        out.idleReason = ControlOutput::IdleReason::BadDt;
        return out;
    }

    // ── 新鲜度门禁（二值，§3.3） ────────────────────────────────────────
    if (cfg_.requireFreshDetection && !in.detectionFresh)
    {
        out.idleReason = ControlOutput::IdleReason::StaleDetection;
        return out;
    }
    if (cfg_.requireFreshCrosshair && !in.crosshairFresh)
    {
        out.idleReason = ControlOutput::IdleReason::StaleCrosshair;
        return out;
    }

    // ── ① 筛选 + 选靶 ──────────────────────────────────────────────────
    const std::vector<size_t> aimIdx = filterAimCandidates(in.candidates, cfg_.buckets);
    if (aimIdx.empty())
    {
        // 没有可瞄目标 ⇒ 锁定失效，下游必须复位（§3.3 第 3 条）。
        selectorState_.reset();
        stabilizerState_.reset();
        if (filter_) filter_->reset();
        pid_.reset();
        hasLastBox_ = false;
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }

    const TargetSelection sel = selectTarget(
        in.candidates, aimIdx, in.cross, cfg_.selector, selectorState_);
    if (!sel.found)
    {
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }

    // ── ② 稳定器（认目标 + 剔除异常框，★ 不滤波） ──────────────────────
    Candidate chosen;
    chosen.box = sel.box;
    chosen.classId = sel.classId;
    chosen.confidence = sel.confidence;

    const StabilizerResult stab = stabilize(chosen, cfg_.stabilizer, stabilizerState_);
    if (!stab.accepted)
    {
        out.idleReason = ControlOutput::IdleReason::RejectedByStabilizer;
        // ★ 本帧不出力，但【不】复位滤波器 —— 一个误检不该把已经稳定的估计丢掉。
        return out;
    }

    // ★★★ 突变 ⇒ 硬重置下游滤波状态（§3.2.1）。
    //   不重置的话，换目标时会出现"从旧位置滑到新位置"的过渡，
    //   而控制器会把这段过渡当成"目标在高速移动"，表现为换目标时冲一下。
    //   ★ 这条与卡尔曼无关 —— 无论开不开卡尔曼都需要。
    if (stab.verdict == StabilizerVerdict::Snap || stab.verdict == StabilizerVerdict::NoHistory)
    {
        if (filter_) filter_->reset();
        pid_.reset();
    }

    // ── ③ 滤波（唯一一处） ─────────────────────────────────────────────
    // ★ 平滑对象是稳定后框的【中心点】（2 个数），不是四个角/宽高。
    const Vec2 obsCenter = stab.box.center();
    filter_->observe(obsCenter, in.dtSec);
    const Vec2 filteredCenter = filter_->position();

    hasLastBox_ = true;
    lastBox_ = stab.box;

    // ── ④ 瞄点 ────────────────────────────────────────────────────────
    // ★ 用【滤波后的中心点】+【稳定器的框尺寸】—— 滤波只有中心点被平滑。
    out.anchor = computeAnchor(filteredCenter, stab.box, cfg_.aimPoint, in.frameIndex);

    // ── ④b 目标框与身份（供自动扳机算命中区 / 判转火）──────────────────
    // ★ 用【稳定后的框】而不是原始 sel.box —— 命中区应当跟着控制实际用的
    //   那个框走, 否则误检的漂移会让扳机跟着抽。
    out.targetBox = stab.box;
    out.hasTarget = true;
    // 身份: 稳定器每帧要么延续同一个目标(Common), 要么判为换目标(Snap/NoHistory)。
    // ★ 只有"换目标"才推进编号 —— 扳机的转火冷却就挂在这个变化上。
    if (stab.verdict == StabilizerVerdict::Snap || stab.verdict == StabilizerVerdict::NoHistory)
        ++targetIdCounter_;
    out.targetId = targetIdCounter_;

    // ── ⑤⑥ PID + 量化 ────────────────────────────────────────────────
    out.error = out.anchor - out.cross;
    out.counts = pid_.update(out.anchor, out.cross, in.dtSec);
    out.engaged = true;
    out.idleReason = ControlOutput::IdleReason::None;
    return out;
}

} // namespace control
