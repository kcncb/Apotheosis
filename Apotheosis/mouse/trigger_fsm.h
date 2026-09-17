#ifndef MOUSE_TRIGGER_FSM_H
#define MOUSE_TRIGGER_FSM_H

// ★★ 自动扳机的状态机 (2026-09-17 重建)。
//
// 背景: 原实现住在 runtime/mouse_thread_loop.cpp 里(1536 行, 含链路日志/下发/
// 滤波/扳机/开镜/急停)。那条链在 "只留采集+推理" 那轮被整条删除, 扳机也随之
// 消失。现按用户要求重建 —— 但【只搬扳机本身】, 不再是那个巨文件的一部分:
// 本头文件零依赖(不引 OpenCV / Windows / Qt), 所以能在任何平台上单测。
//
// 与旧实现的语义【逐条对齐】(依据: build 里的旧源码 mouse_thread_loop.cpp
// 第 289-370 / 1222-1446 行):
//
//   命中区几何 (旧 1222-1234): 以【框】为基准的一个区间, 与瞄点解耦 ——
//       half_x = bbox.w * s/2,  half_y = bbox.h * s/2   (s = trigger_y_percent/100)
//       in_zone = |judge_x - box_cx| <= half_x && |judge_y - box_cy| <= half_y
//     ★ 判定输入用【原始】准星, 不用瞄点 —— 否则"准星是否真在框里"就变成
//       对一个滞后量的判断(旧注释 1219-1221 记了这个坑)。
//
//   五个相位: Idle → Delay → Pressed → Cooldown, 外加 SwitchCooldown。
//     Idle:    in_zone 且 delay<=0 ⇒ 立即击发; 否则记 in_zone_since_ms 起算。
//     Delay:   离开命中区就回 Idle; 到时就击发。
//     Pressed: 长按模式一直按到离开命中区; 否则按住 phase_target_ms 后松开。
//     Cooldown: 冷却结束时若仍在区内且 delay<=0 ⇒ 无缝续发。
//     SwitchCooldown: 换目标身份时的转火冷却。
//
//   ★ 三条必须守住的性质(与旧实现一致):
//     ① 零延迟直通: trigger_fire_delay=0 时进区那一拍就开火(机械级瞬发)。
//     ② 抖动按 phase 只摇一次(phase_target_ms), 不能每帧重摇 —— 否则门槛漂移。
//     ③ 换目标(last_fire_track_id 变化)才进 SwitchCooldown, 且只在
//        trigger_switch_cooldown_ms > 0 且【当前不在命中区】时 ——
//        转火后新目标立刻在准星上时应当接力爆发, 不该卡一下。

#include <algorithm>
#include <cstdint>
#include <random>

namespace boss
{

enum class TriggerPhase { Idle, Delay, Pressed, Cooldown, SwitchCooldown };

class TriggerFsm
{
public:
    // 一次 tick 的判定输入。
    struct Input
    {
        bool   in_zone = false;        // 准星是否落在命中区内
        int    track_id = -1;          // 当前锁定轨迹的身份(用于转火判定)
        int64_t now_ms = 0;            // 单调毫秒时钟
    };

    // 一次 tick 的输出 = 要对驱动做的动作。★ 顺序敏感: 先抬后按。
    struct Action
    {
        bool press_left = false;
        bool release_left = false;
        bool fired = false;   // 本拍是否真的按下了左键(供自动急停用)
    };

    // 每拍调用一次。
    //   hold_mode  : true = 长按(trigger_fire_duration 不作为上限)
    //   fire_delay : 进区后延迟 ms 才开火 (0 = 立即)
    //   duration   : 单次按住时长 (连点模式)
    //   interval   : 冷却间隔
    //   switch_cd  : 转火冷却
    Action tick(const Input& in, bool hold_mode,
                int fire_delay, int duration, int interval, int switch_cd,
                int delay_jitter, int duration_jitter, int interval_jitter)
    {
        Action act;

        // 转火判定: 身份变了、且不是第一次锁定、配了冷却、且当前【不在】命中区。
        // ★ "不在命中区"这个条件很重要 —— 转火后新目标立刻就在准星上时,
        //   应当立即接力开火, 不该为了冷却而卡一下。
        if (in.track_id != last_fire_track_id_ &&
            last_fire_track_id_ != -1 &&
            switch_cd > 0 &&
            phase_ != TriggerPhase::SwitchCooldown &&
            !in.in_zone)
        {
            if (phase_ == TriggerPhase::Pressed)
                act.release_left = true;
            phase_ = TriggerPhase::SwitchCooldown;
            phase_time_ms_ = in.now_ms;
            phase_target_ms_ = jitter(switch_cd, delay_jitter);
            in_zone_since_ms_ = -1;
        }
        last_fire_track_id_ = in.track_id;

        switch (phase_)
        {
        case TriggerPhase::Idle:
            if (in.in_zone)
            {
                const int target_delay = jitter(fire_delay, delay_jitter);
                if (target_delay <= 0)
                {
                    beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
                }
                else
                {
                    if (in_zone_since_ms_ < 0)
                    {
                        in_zone_since_ms_ = in.now_ms;
                        phase_target_ms_ = target_delay;
                    }
                    if (in.now_ms - in_zone_since_ms_ >= phase_target_ms_)
                        beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
                    else
                    {
                        phase_ = TriggerPhase::Delay;
                        phase_time_ms_ = in_zone_since_ms_;
                    }
                }
            }
            else
            {
                in_zone_since_ms_ = -1;
            }
            break;

        case TriggerPhase::Delay:
            if (!in.in_zone)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                break;
            }
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
                beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
            break;

        case TriggerPhase::Pressed:
            if (hold_mode)
            {
                // 长按: 只要还在命中区就一直按着。离开才松手, 并走一次冷却
                // —— 避免在判定边缘"踩空"变成连点。
                if (!in.in_zone)
                {
                    act.release_left = true;
                    phase_ = TriggerPhase::Cooldown;
                    phase_time_ms_ = in.now_ms;
                    phase_target_ms_ = jitter(interval, interval_jitter);
                }
                break;
            }
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                act.release_left = true;
                phase_ = TriggerPhase::Cooldown;
                phase_time_ms_ = in.now_ms;
                phase_target_ms_ = jitter(interval, interval_jitter);
            }
            break;

        case TriggerPhase::Cooldown:
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                // 冷却结束瞬间若仍在区内且零延迟 ⇒ 无缝衔接下一轮爆发。
                if (in.in_zone && fire_delay <= 0)
                    beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
            }
            break;

        case TriggerPhase::SwitchCooldown:
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
            }
            break;
        }

        return act;
    }

    // 会话/热键结束时调用: 把按住的左键还回去, 状态清空。
    // ★ 必须在松开热键时调用, 否则左键会卡在按下。
    bool reset()
    {
        const bool was_pressed = (phase_ == TriggerPhase::Pressed);
        phase_ = TriggerPhase::Idle;
        phase_time_ms_ = 0;
        phase_target_ms_ = 0;
        in_zone_since_ms_ = -1;
        last_fire_track_id_ = -1;
        return was_pressed;   // 调用方据此决定要不要 releaseLeftButton()
    }

    TriggerPhase phase() const { return phase_; }
    bool pressed() const { return phase_ == TriggerPhase::Pressed; }

    // 命中区判定 —— 与旧实现同一公式(旧 1222-1234)。
    // 返回是否在区内; half_x/half_y 是供界面显示判定框用的半宽/半高。
    static bool inHitZone(double judge_x, double judge_y,
                          double box_x, double box_y, double box_w, double box_h,
                          int y_percent,
                          double* half_x_out = nullptr, double* half_y_out = nullptr)
    {
        const double s = std::max(0.1, y_percent / 100.0);
        const double half_x = box_w * s * 0.5;
        const double half_y = box_h * s * 0.5;
        const double box_cx = box_x + box_w * 0.5;
        const double box_cy = box_y + box_h * 0.5;
        if (half_x_out) *half_x_out = half_x;
        if (half_y_out) *half_y_out = half_y;
        return std::abs(judge_x - box_cx) <= half_x &&
               std::abs(judge_y - box_cy) <= half_y;
    }

private:
    void beginFire(Action& act, int64_t now_ms, bool hold_mode,
                   int duration, int duration_jitter)
    {
        act.press_left = true;
        act.fired = true;
        phase_ = TriggerPhase::Pressed;
        phase_time_ms_ = now_ms;
        in_zone_since_ms_ = -1;
        // 长按模式不设按住上限; 连点模式才用 duration。
        phase_target_ms_ = hold_mode ? 0 : jitter(duration, duration_jitter);
    }

    // 对基础时长加 ±jitter, 结果不小于 0。
    static int jitter(int base, int j)
    {
        if (j <= 0) return std::max(0, base);
        thread_local std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> d(-j, j);
        return std::max(0, base + d(rng));
    }

    TriggerPhase phase_ = TriggerPhase::Idle;
    int64_t phase_time_ms_ = 0;
    // in_zone 起点; -1 = 尚未起算。用 (now - in_zone_since_ms) >= delay 判定,
    // 这样进区那一拍不会空转。
    int64_t in_zone_since_ms_ = -1;
    int last_fire_track_id_ = -1;
    // 本 phase 的目标时长(已含抖动) —— ★ 只摇一次, 否则门槛每帧漂移。
    int phase_target_ms_ = 0;
};

} // namespace boss

#endif // MOUSE_TRIGGER_FSM_H
