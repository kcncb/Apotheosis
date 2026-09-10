#include "runtime/auto_backflash.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace auto_backflash
{
namespace
{
std::atomic<unsigned long long> g_test_generation{0};
std::atomic<int> g_phase{static_cast<int>(Phase::Idle)};
std::atomic<int> g_remaining{0};

int units_per_second(int speed)
{
    // 1% 仍能明显移动，100% 对应快速转身。输出按实际 dt 累积，
    // 不依赖推理帧率，也不向 UI 暴露内部拆包步数。
    const int clamped = std::clamp(speed, 1, 100);
    return 2000 + (clamped - 1) * 580;
}
}

void request_test()
{
    g_test_generation.fetch_add(1, std::memory_order_acq_rel);
}

Phase published_phase()
{
    return static_cast<Phase>(g_phase.load(std::memory_order_acquire));
}

int published_remaining()
{
    return g_remaining.load(std::memory_order_acquire);
}

bool Controller::active() const
{
    return phase_ == Phase::Turning
        || phase_ == Phase::Holding
        || phase_ == Phase::Returning
        || phase_ == Phase::Settling;
}

void Controller::publish() const
{
    g_phase.store(static_cast<int>(phase_), std::memory_order_release);
    g_remaining.store(remaining_, std::memory_order_release);
}

void Controller::begin_turn(const Params& params,
                            std::chrono::steady_clock::time_point now,
                            bool testing,
                            int direction)
{
    direction_ = direction < 0 ? -1 : 1;
    remaining_ = std::clamp(params.turn_amount, 100, 30000);
    displacement_ = 0;
    fractional_move_ = 0.0;
    last_motion_time_ = now;
    confirm_count_ = 0;
    testing_ = testing;
    pending_cooldown_ms_ = 0;
    phase_ = Phase::Turning;
    publish();
}

void Controller::begin_return(const Params&,
                              std::chrono::steady_clock::time_point now)
{
    remaining_ = std::abs(displacement_);
    direction_ = displacement_ > 0 ? -1 : 1;
    fractional_move_ = 0.0;
    last_motion_time_ = now;
    phase_ = remaining_ > 0 ? Phase::Returning : Phase::Cooldown;
    publish();
}

bool Controller::advance_motion(
    int speed,
    std::chrono::steady_clock::time_point now,
    const std::function<bool(int)>& send_horizontal)
{
    const double elapsed = std::clamp(
        std::chrono::duration<double>(now - last_motion_time_).count(),
        0.0, 0.050);
    last_motion_time_ = now;
    fractional_move_ += elapsed * static_cast<double>(units_per_second(speed));
    int amount = std::min(remaining_, static_cast<int>(std::floor(fractional_move_)));
    if (amount <= 0)
        return remaining_ == 0;

    if (!send_horizontal(direction_ * amount))
        return false;

    fractional_move_ -= amount;
    remaining_ -= amount;
    displacement_ += direction_ * amount;
    publish();
    return remaining_ == 0;
}

bool Controller::tick(
    const Params& params,
    bool has_new_detection,
    const std::vector<int>& classes,
    const std::vector<float>& confidences,
    const std::vector<float>& centers_x,
    float frame_center_x,
    std::chrono::steady_clock::time_point now,
    const std::function<bool(int)>& send_horizontal)
{
    const auto requested = g_test_generation.load(std::memory_order_acquire);
    if (requested != seen_test_generation_)
    {
        seen_test_generation_ = requested;
        if (phase_ == Phase::Idle || phase_ == Phase::Cooldown)
            begin_turn(params, now, true, 1);
    }

    // 运行中关闭开关时不能把视角留在背身位置：停止继续转身并立即归还
    // 已经发送的位移。测试请求不受总开关影响。
    if (!params.enabled && !testing_
        && (phase_ == Phase::Turning || phase_ == Phase::Holding))
        begin_return(params, now);

    switch (phase_)
    {
    case Phase::Idle:
        if (params.enabled && has_new_detection && !params.classes.empty())
        {
            const std::unordered_set<int> selected(
                params.classes.begin(), params.classes.end());
            bool matched = false;
            float best_confidence = -1.0f;
            float matched_center_x = frame_center_x;
            const size_t count = std::min(
                {classes.size(), confidences.size(), centers_x.size()});
            for (size_t i = 0; i < count; ++i)
            {
                if (selected.count(classes[i]) > 0
                    && confidences[i] > 0.0f
                    && confidences[i] > best_confidence)
                {
                    matched = true;
                    best_confidence = confidences[i];
                    matched_center_x = centers_x[i];
                }
            }
            confirm_count_ = matched ? confirm_count_ + 1 : 0;
            if (confirm_count_ >= std::clamp(params.confirm_frames, 1, 8))
            {
                // 鼠标正 X 使视角向右：左侧闪光向右背，右侧（含中线）
                // 向左背。多目标时使用置信度最高的所选检测框。
                const int away_direction = matched_center_x < frame_center_x
                    ? 1 : -1;
                begin_turn(params, now, false, away_direction);
            }
        }
        else if (!params.enabled || has_new_detection)
        {
            confirm_count_ = 0;
        }
        break;

    case Phase::Turning:
        if (advance_motion(params.turn_speed, now, send_horizontal))
        {
            phase_ = Phase::Holding;
            remaining_ = 0;
            deadline_ = now + std::chrono::milliseconds(
                std::clamp(params.return_delay_ms, 0, 5000));
            publish();
        }
        break;

    case Phase::Holding:
        if (now >= deadline_)
            begin_return(params, now);
        break;

    case Phase::Returning:
        if (advance_motion(params.return_speed, now, send_horizontal))
        {
            displacement_ = 0;
            remaining_ = 0;
            // 可靠通道已确认最后一段被 USB 端点接受。再短暂保持独占，
            // 避免普通瞄准在视角刚回正时立即输出，导致看起来像未回原位。
            pending_cooldown_ms_ = testing_
                ? 0 : std::clamp(params.cooldown_ms, 0, 10000);
            deadline_ = now + std::chrono::milliseconds(20);
            testing_ = false;
            phase_ = Phase::Settling;
            publish();
        }
        break;

    case Phase::Settling:
        if (now >= deadline_)
        {
            deadline_ = now + std::chrono::milliseconds(pending_cooldown_ms_);
            phase_ = pending_cooldown_ms_ > 0 ? Phase::Cooldown : Phase::Idle;
            pending_cooldown_ms_ = 0;
            publish();
        }
        break;

    case Phase::Cooldown:
        confirm_count_ = 0;
        if (now >= deadline_)
        {
            phase_ = Phase::Idle;
            publish();
        }
        break;
    }

    return active();
}

int Controller::emergency_return_delta()
{
    const int delta = -displacement_;
    phase_ = Phase::Idle;
    confirm_count_ = 0;
    remaining_ = 0;
    displacement_ = 0;
    fractional_move_ = 0.0;
    testing_ = false;
    pending_cooldown_ms_ = 0;
    publish();
    return delta;
}

} // namespace auto_backflash
