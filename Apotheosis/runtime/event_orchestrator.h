#ifndef RUNTIME_EVENT_ORCHESTRATOR_H
#define RUNTIME_EVENT_ORCHESTRATOR_H

// =============================================================================
// 事件编排 (event orchestrator)
//
// 主循环里的关键时刻(锁到目标、丢目标、切目标、扳机按下/松开、寻光命中)
// publish() 出去,后台工作线程按规则表把匹配的动作序列排队执行。
// 动作序列自带累计 delay:Delay 只是把后续动作往后推,不阻塞后台线程。
//
// 全线程安全:publish 无阻塞,主循环随便调。规则表 CRUD 走内部 mutex。
// =============================================================================

#include <string>
#include <vector>

namespace event_orch
{

enum class EventType : int
{
    TargetLocked   = 0,   // 从无锁定到锁定一个目标(current_track_id: -1 → n)
    TargetLost     = 1,   // 从锁定到丢失(n → -1)
    TargetSwitched = 2,   // 锁定的 track_id 从 a 变到 b
    FirePressed    = 3,   // 扳机 FSM 按下鼠标左键
    FireReleased   = 4,   // 扳机 FSM 松开鼠标左键
    FlashlightHit  = 5,   // 寻光命中(valid 且 spots 非空)
    FlashlightLost = 6,   // 寻光命中结束(有 → 无)
    Count
};

enum class ExecutionMode : int
{
    Once        = 0,   // 每次事件触发执行一轮
    RepeatCount = 1,   // 每次事件触发固定循环 N 轮
    WhileActive = 2,   // 从事件开始持续循环，到互补结束事件为止
    Count
};

enum class ActionType : int
{
    Delay      = 0,   // 延迟 a ms(只推后续动作,不阻塞后台线程)
    KeyTap     = 1,   // 键盘 vk=a 按下,b ms 后松开
    KeyDown    = 2,   // 键盘 vk=a 按下(不松开)
    KeyUp      = 3,   // 键盘 vk=a 松开
    MouseTap   = 4,   // 鼠标 button=a (0=L 1=R 2=M 3=X1 4=X2) 按下 b ms 后松开
    MouseDown  = 5,   // 鼠标 button=a 按下
    MouseUp    = 6,   // 鼠标 button=a 松开
    Count
};

struct Action
{
    ActionType type = ActionType::Delay;
    int a = 0;   // Delay: ms;  Key/Mouse: vk 或 button 号
    int b = 50;  // KeyTap / MouseTap: hold_ms
};

struct Rule
{
    std::string name = u8"新规则";
    bool enabled = true;
    EventType   event = EventType::TargetLocked;
    ExecutionMode execution_mode = ExecutionMode::Once;
    int repeat_count = 2;         // RepeatCount:2..1000
    int repeat_interval_ms = 100; // 两轮之间的额外间隔
    int cooldown_ms = 0;   // 两次触发的最小间隔(0=不限)
    std::vector<Action> actions;
};

// 后台工作线程 lifecycle。start() 幂等;stop() 会 join。
void start();
void stop();

// 主循环调用,无阻塞。触发匹配规则的动作序列排队执行。
void publish(EventType e);

// 规则表 CRUD(内部加锁)
std::vector<Rule> get_rules();
void set_rules(std::vector<Rule> rules);

// 序列化:一行文本,给 config.ini / QSettings 用。
// v2|name|enabled|event|mode|count|interval|cooldown|type,a,b|...
// 旧的 name|enabled|event|cooldown|... 自动按 Once 读取。
// name 不能包含 '|' 或 ';';超出会被清洗成 '_'。
std::string serialize_rule(const Rule& r);
Rule        deserialize_rule(const std::string& s);

// 名字表(给 UI 显示 + 提示)
const char* event_name(EventType e);
const char* action_name(ActionType a);
const char* execution_mode_name(ExecutionMode mode);

} // namespace event_orch

#endif // RUNTIME_EVENT_ORCHESTRATOR_H
