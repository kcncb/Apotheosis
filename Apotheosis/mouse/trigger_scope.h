#ifndef MOUSE_TRIGGER_SCOPE_H
#define MOUSE_TRIGGER_SCOPE_H

#include <cstdint>

namespace boss
{

// 自动开镜 —— 仿 AimMagic 的「开火方式」(fireModes: None / Click Right / Hold Right)。
//
//   mode 0 = 关闭
//   mode 1 = 点按右键（一次性）: **每次接敌开始时点一下右键**(按下 → 下一拍抬起),
//            之后就不再碰它了 —— 不自动收镜, 开镜状态由用户自己负责。
//   mode 2 = 长按右键（按住开镜）: 命中区里一直按住, 离开时松开。
//
// 这个状态机只做两件事: ①决定"该不该点/按住右键"; ②回答"镜开好了没有" ——
// 后者用来闸住左键, 保证第一颗子弹是开着镜打出去的。
//
// ★ 五条必须守住的性质 (都有回归断言):
//   ① 点按模式的一次"点"跨两拍完成(按下 → 下一拍抬起)。同一拍里连发 down/up
//      在多数游戏里会被合并或直接丢掉, 而本循环的节拍就是检测帧率(≈8ms),
//      跨一拍正好是"机械鼠标能给到的最小可靠点击"。
//   ② **只点一下, 不收镜**(2026-09-12 实机反馈后改): 收镜时再点一下, 在"切换
//      开镜"的游戏里会和用户自己的操作打架 —— 用户明确要求开镜状态由他自己处理。
//   ③ **右键绝不允许卡在按下**。欠下的抬指必须被补发: 接敌途中的复位(丢目标/
//      滑行/检测流断)走 flushUp() 只补抬指; 会话或接敌结束走 forceRelease()。
//   ④ 接敌途中的复位【不重新武装】点按 —— 否则每次重新锁到目标都会又点一下,
//      在切换开镜的游戏里把镜来回切。只有热键松开/会话结束(=新的接敌)才重新武装。
//   ⑤ 只有 mode > 0 且 allowed 时才介入; allowed=false(热键自己就绑了右键)时
//      一律不碰右键。ready() 在不适用时恒为 true, 绝不能把扳机整体卡死。
class ScopeController
{
public:
    struct Action
    {
        bool press_right = false;
        bool release_right = false;
    };

    // 每拍调用一次。
    //   in_zone    准星是否在命中区
    //   allowed    本热键是否允许自动开镜 (false = 热键自己绑了右键)
    //   mode       0/1/2
    //   delay_ms   开镜后等多久才允许开火 (0 = 同一拍)
    //   now_ms     单调毫秒时钟
    Action tick(bool in_zone, bool allowed, int mode, int delay_ms, int64_t now_ms)
    {
        Action action;
        // 上一次"点"的抬指要落在下一拍, 否则 down/up 会挤在同一拍里。
        if (tap_release_pending_)
        {
            action.release_right = true;
            tap_release_pending_ = false;
        }

        // 运行中改了模式: 先把旧模式下的按键状态还回去, 再按新模式重新起一次。
        // (长按态直接改成"关闭/点按"而不松开, 右键就会永远卡在按下。)
        if (engaged_ && mode != mode_)
        {
            if (mode_ >= 2)
                action.release_right = true;
            engaged_ = false;
        }
        mode_ = mode;

        const bool want = in_zone && allowed && mode > 0;
        if (!want)
        {
            // 长按模式: 离开命中区就松开, 下次进区重新按住。
            if (engaged_ && mode >= 2)
            {
                engaged_ = false;
                action.release_right = true;
            }
            // 点按模式: 【什么都不做】。我们只负责"开枪前点一下", 之后开镜状态
            // 由用户自己处理; engaged_ 一直保持到本次接敌结束(热键松开/会话停止
            // 时由 forceRelease 重新武装)。所以离开命中区再回来也不会再点第二下。
            return action;
        }

        if (!engaged_)
        {
            engaged_ = true;
            opened_ms_ = now_ms;
            action.press_right = true;
            // 点按模式只点一下: 抬指排在下一拍, 之后不再碰右键。
            if (mode == 1)
                tap_release_pending_ = true;
        }
        return action;
    }

    // 镜开好了没有 —— begin_fire() 拿它当闸门。
    // 自动开镜没开/不适用时必须返回 true, 否则会把扳机整体卡死。
    bool ready(bool allowed, int mode, int delay_ms, int64_t now_ms) const
    {
        if (!allowed || mode <= 0)
            return true;
        if (!engaged_)
            return false;
        if (delay_ms <= 0)
            return true;
        return (now_ms - opened_ms_) >= delay_ms;
    }

    // 只把欠下的抬指补上, 【不动】接敌状态。
    // 接敌途中复位(丢目标/滑行)用它: 保证右键是抬起的, 又不至于重新武装点按。
    Action flushUp()
    {
        Action action;
        if (tap_release_pending_)
        {
            action.release_right = true;
            tap_release_pending_ = false;
        }
        return action;
    }

    // 接敌/会话结束: 保证右键抬起, 并且【不】再补点一下。
    // 返回之后控制器回到"未介入"态, 下一次接敌会重新点一下。
    Action forceRelease()
    {
        Action action = flushUp();
        if (engaged_)
        {
            engaged_ = false;
            // 长按模式必须松开; 点按模式早就抬过指了, 什么都不做。
            if (mode_ >= 2)
                action.release_right = true;
        }
        return action;
    }

    // 本次接敌是否已经动过右键(点按模式 = 已经点过; 长按模式 = 正按着)。
    // 点按模式下它在离开命中区后仍然为 true —— 因为在同一次接敌里我们不收镜、
    // 也不会再点第二下, 直到接敌结束(forceRelease)才重新武装。
    bool engaged() const { return engaged_; }
    int  mode() const { return mode_; }

private:
    bool    engaged_ = false;
    bool    tap_release_pending_ = false;
    int     mode_ = 0;
    int64_t opened_ms_ = 0;
};

} // namespace boss

#endif // MOUSE_TRIGGER_SCOPE_H
