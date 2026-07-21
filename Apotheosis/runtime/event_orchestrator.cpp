#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "runtime/event_orchestrator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace event_orch
{

namespace
{

struct Pending
{
    std::chrono::steady_clock::time_point fire_at;
    Action action;
    size_t rule_index = 0;
    unsigned long long generation = 0;
    unsigned long long order = 0;
    bool cycle_marker = false;
};

struct RunState
{
    unsigned long long generation = 0;
    bool active = false;
    int remaining_cycles = 0; // -1 = WhileActive
};

struct State
{
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<Rule> rules;
    std::vector<Pending> pending;   // 按 fire_at 升序
    std::vector<std::chrono::steady_clock::time_point> rule_last_fire;
    std::vector<RunState> runs;
    unsigned long long next_order = 1;
    std::atomic<bool> running{false};
    std::thread worker;
};
State g;

void send_key(int vk, bool down)
{
    INPUT ip{};
    ip.type = INPUT_KEYBOARD;
    ip.ki.wVk = static_cast<WORD>(vk);
    ip.ki.dwFlags = down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP);
    SendInput(1, &ip, sizeof(ip));
}

void send_mouse(int button, bool down)
{
    INPUT ip{};
    ip.type = INPUT_MOUSE;
    switch (button)
    {
    case 0: ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN
                                 : MOUSEEVENTF_LEFTUP;   break;
    case 1: ip.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN
                                 : MOUSEEVENTF_RIGHTUP;  break;
    case 2: ip.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN
                                 : MOUSEEVENTF_MIDDLEUP; break;
    case 3:
    case 4:
        ip.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        ip.mi.mouseData = (button == 3) ? XBUTTON1 : XBUTTON2;
        break;
    default: return;
    }
    SendInput(1, &ip, sizeof(ip));
}

bool pending_less(const Pending& x, const Pending& y)
{
    if (x.fire_at != y.fire_at) return x.fire_at < y.fire_at;
    return x.order < y.order;
}

void sort_pending_locked()
{
    std::sort(g.pending.begin(), g.pending.end(), pending_less);
}

EventType complementary_event(EventType e)
{
    switch (e)
    {
    case EventType::TargetLocked:   return EventType::TargetLost;
    case EventType::TargetLost:     return EventType::TargetLocked;
    case EventType::FirePressed:    return EventType::FireReleased;
    case EventType::FireReleased:   return EventType::FirePressed;
    case EventType::FlashlightHit:  return EventType::FlashlightLost;
    case EventType::FlashlightLost: return EventType::FlashlightHit;
    case EventType::TargetSwitched:
    case EventType::Count:          return EventType::Count;
    }
    return EventType::Count;
}

void push_pending_locked(std::chrono::steady_clock::time_point ts,
                         const Action& action, size_t rule_index,
                         unsigned long long generation, bool marker = false)
{
    Pending p;
    p.fire_at = ts;
    p.action = action;
    p.rule_index = rule_index;
    p.generation = generation;
    p.order = g.next_order++;
    p.cycle_marker = marker;
    g.pending.push_back(p);
}

void enqueue_cycle_locked(size_t rule_index,
                          unsigned long long generation,
                          std::chrono::steady_clock::time_point start)
{
    if (rule_index >= g.rules.size()) return;
    const Rule& r = g.rules[rule_index];
    auto ts = start;

    for (const auto& a : r.actions)
    {
        if (a.type == ActionType::Delay)
        {
            ts += std::chrono::milliseconds(std::max(0, a.a));
            continue;
        }

        // Tap 展开成两个定时动作，唯一 worker 不再 sleep/阻塞其它规则。
        if (a.type == ActionType::KeyTap || a.type == ActionType::MouseTap)
        {
            Action down = a;
            Action up = a;
            if (a.type == ActionType::KeyTap)
            {
                down.type = ActionType::KeyDown;
                up.type = ActionType::KeyUp;
            }
            else
            {
                down.type = ActionType::MouseDown;
                up.type = ActionType::MouseUp;
            }
            push_pending_locked(ts, down, rule_index, generation);
            ts += std::chrono::milliseconds(std::max(1, a.b));
            push_pending_locked(ts, up, rule_index, generation);
            continue;
        }

        push_pending_locked(ts, a, rule_index, generation);
    }

    if (r.execution_mode != ExecutionMode::Once)
    {
        ts += std::chrono::milliseconds(std::max(1, r.repeat_interval_ms));
        push_pending_locked(ts, Action{}, rule_index, generation, true);
    }
    sort_pending_locked();
}

// 取消规则时兜底释放它可能按下的输入，避免持续模式停止后卡键。
void release_rule_inputs(const Rule& r)
{
    for (const auto& a : r.actions)
    {
        if (a.type == ActionType::KeyDown || a.type == ActionType::KeyTap)
            send_key(a.a, false);
        else if (a.type == ActionType::MouseDown || a.type == ActionType::MouseTap)
            send_mouse(a.a, false);
    }
}

// Tap 已在排队阶段展开，此处所有动作都是立即动作。
void execute(const Action& a)
{
    switch (a.type)
    {
    case ActionType::Delay:
        // 不应到达这里(Delay 由 publish 排队时展平掉)。
        break;
    case ActionType::KeyTap:  break;
    case ActionType::KeyDown:  send_key(a.a, true);  break;
    case ActionType::KeyUp:    send_key(a.a, false); break;
    case ActionType::MouseTap: break;
    case ActionType::MouseDown: send_mouse(a.a, true);  break;
    case ActionType::MouseUp:   send_mouse(a.a, false); break;
    case ActionType::Count:     break;
    }
}

void worker_thread()
{
    while (g.running.load())
    {
        std::unique_lock<std::mutex> lk(g.mtx);
        if (g.pending.empty())
        {
            g.cv.wait_for(lk, std::chrono::milliseconds(200),
                          [] { return !g.pending.empty() || !g.running.load(); });
            if (!g.running.load()) return;
            continue;
        }
        // 等到最早 fire_at；新插入更早动作时 notify 后重新比较。
        const auto next = g.pending.front().fire_at;
        g.cv.wait_until(lk, next, [next] {
            return !g.running.load() || g.pending.empty()
                || g.pending.front().fire_at < next;
        });
        if (!g.running.load()) return;
        if (g.pending.empty() || g.pending.front().fire_at > std::chrono::steady_clock::now())
            continue;

        Pending p = g.pending.front();
        g.pending.erase(g.pending.begin());
        if (p.rule_index >= g.runs.size())
            continue;
        RunState& run = g.runs[p.rule_index];
        if (!run.active || run.generation != p.generation)
            continue;

        if (p.cycle_marker)
        {
            if (run.remaining_cycles == 0)
            {
                run.active = false;
                const Rule finished_rule = g.rules[p.rule_index];
                lk.unlock();
                release_rule_inputs(finished_rule);
                continue;
            }
            if (run.remaining_cycles > 0)
                --run.remaining_cycles;
            enqueue_cycle_locked(p.rule_index, p.generation, p.fire_at);
            g.cv.notify_all();
            continue;
        }
        lk.unlock();
        execute(p.action);
    }
}

// UI 显示名(UTF-8 中文)。EventType / ActionType 顺序与 enum 一致。
constexpr const char* kEventNames[] = {
    u8"锁定目标",
    u8"目标丢失",
    u8"切换目标",
    u8"开火按下",
    u8"开火松开",
    u8"寻光命中",
    u8"寻光结束",
};
constexpr const char* kActionNames[] = {
    u8"延迟",
    u8"按键敲击",
    u8"按键按下",
    u8"按键松开",
    u8"鼠标点击",
    u8"鼠标按下",
    u8"鼠标松开",
};
constexpr const char* kExecutionModeNames[] = {
    u8"单次",
    u8"固定循环",
    u8"持续期间循环",
};

std::string sanitize(std::string s)
{
    for (auto& c : s)
        if (c == '|' || c == ';' || c == '\r' || c == '\n') c = '_';
    return s;
}

} // namespace

// ─── Lifecycle ──────────────────────────────────────────────────────────────
void start()
{
    if (g.running.exchange(true)) return;
    g.worker = std::thread(worker_thread);
}
void stop()
{
    if (!g.running.exchange(false)) return;
    g.cv.notify_all();
    if (g.worker.joinable()) g.worker.join();
    std::vector<Rule> old_rules;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        old_rules = g.rules;
        g.pending.clear();
        for (auto& run : g.runs)
        {
            run.active = false;
            ++run.generation;
        }
    }
    for (const auto& r : old_rules)
        release_rule_inputs(r);
}

// ─── Publish ────────────────────────────────────────────────────────────────
void publish(EventType e)
{
    if (!g.running.load()) return;
    const auto now = std::chrono::steady_clock::now();

    std::vector<Rule> release_after_unlock;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(g.mtx);

        if (g.rule_last_fire.size() != g.rules.size())
            g.rule_last_fire.assign(
                g.rules.size(),
                std::chrono::steady_clock::time_point::min());
        if (g.runs.size() != g.rules.size())
            g.runs.resize(g.rules.size());

        // WhileActive 收到互补结束边沿时立即取消尚未执行的动作/下一轮。
        for (size_t i = 0; i < g.rules.size(); ++i)
        {
            const Rule& r = g.rules[i];
            if (r.execution_mode != ExecutionMode::WhileActive ||
                complementary_event(r.event) != e || !g.runs[i].active)
                continue;

            g.runs[i].active = false;
            ++g.runs[i].generation;
            g.pending.erase(
                std::remove_if(g.pending.begin(), g.pending.end(),
                    [i](const Pending& p) { return p.rule_index == i; }),
                g.pending.end());
            release_after_unlock.push_back(r);
        }

        for (size_t i = 0; i < g.rules.size(); ++i)
        {
            const auto& r = g.rules[i];
            if (!r.enabled || r.event != e) continue;

            if (r.cooldown_ms > 0)
            {
                const double elapsed = std::chrono::duration<double, std::milli>(
                    now - g.rule_last_fire[i]).count();
                if (elapsed < static_cast<double>(r.cooldown_ms)) continue;
            }
            g.rule_last_fire[i] = now;

            RunState& run = g.runs[i];
            if (run.active)
            {
                release_after_unlock.push_back(r);
                g.pending.erase(
                    std::remove_if(g.pending.begin(), g.pending.end(),
                        [i](const Pending& p) { return p.rule_index == i; }),
                    g.pending.end());
            }
            ++run.generation;
            run.active = true;
            if (r.execution_mode == ExecutionMode::RepeatCount)
                run.remaining_cycles = std::max(1, r.repeat_count) - 1;
            else if (r.execution_mode == ExecutionMode::WhileActive &&
                     complementary_event(r.event) != EventType::Count)
                run.remaining_cycles = -1;
            else
                run.remaining_cycles = 0;

            enqueue_cycle_locked(i, run.generation, now);
            queued = true;
        }
    }
    for (const auto& r : release_after_unlock)
        release_rule_inputs(r);
    if (queued) g.cv.notify_all();
}

// ─── Rules CRUD ─────────────────────────────────────────────────────────────
std::vector<Rule> get_rules()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.rules;
}
void set_rules(std::vector<Rule> rules)
{
    for (auto& r : rules)
    {
        const int mode = std::clamp(static_cast<int>(r.execution_mode), 0,
                                    static_cast<int>(ExecutionMode::Count) - 1);
        r.execution_mode = static_cast<ExecutionMode>(mode);
        r.repeat_count = std::clamp(r.repeat_count, 1, 1000);
        r.repeat_interval_ms = std::clamp(r.repeat_interval_ms, 1, 600000);
        r.cooldown_ms = std::clamp(r.cooldown_ms, 0, 600000);
    }

    std::vector<Rule> old_rules;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        old_rules = g.rules;
        g.pending.clear();
        g.rules = std::move(rules);
        g.rule_last_fire.assign(
            g.rules.size(),
            std::chrono::steady_clock::time_point::min());
        g.runs.assign(g.rules.size(), RunState{});
    }
    for (const auto& r : old_rules)
        release_rule_inputs(r);
    g.cv.notify_all();
}

// ─── Serialization ──────────────────────────────────────────────────────────
std::string serialize_rule(const Rule& r)
{
    std::ostringstream oss;
    oss << "v2|" << sanitize(r.name) << '|'
        << (r.enabled ? 1 : 0) << '|'
        << static_cast<int>(r.event) << '|'
        << static_cast<int>(r.execution_mode) << '|'
        << r.repeat_count << '|'
        << r.repeat_interval_ms << '|'
        << r.cooldown_ms;
    for (const auto& a : r.actions)
    {
        oss << '|' << static_cast<int>(a.type) << ',' << a.a << ',' << a.b;
    }
    return oss.str();
}

Rule deserialize_rule(const std::string& s)
{
    Rule r;
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : s)
        {
            if (c == '|') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
    }
    if (parts.size() < 4) return r;

    size_t action_start = 4;
    if (parts[0] == "v2" && parts.size() >= 8)
    {
        r.name = parts[1];
        r.enabled = (parts[2] != "0");
        try { r.event = static_cast<EventType>(std::stoi(parts[3])); } catch (...) {}
        try { r.execution_mode = static_cast<ExecutionMode>(std::stoi(parts[4])); } catch (...) {}
        try { r.repeat_count = std::stoi(parts[5]); } catch (...) {}
        try { r.repeat_interval_ms = std::stoi(parts[6]); } catch (...) {}
        try { r.cooldown_ms = std::stoi(parts[7]); } catch (...) {}
        action_start = 8;
    }
    else
    {
        // v1兼容:name|enabled|event|cooldown|actions...，默认单次。
        r.name = parts[0];
        r.enabled = (parts[1] != "0");
        try { r.event = static_cast<EventType>(std::stoi(parts[2])); } catch (...) {}
        try { r.cooldown_ms = std::stoi(parts[3]); } catch (...) {}
        r.execution_mode = ExecutionMode::Once;
    }

    const int event_i = std::clamp(static_cast<int>(r.event), 0,
                                   static_cast<int>(EventType::Count) - 1);
    r.event = static_cast<EventType>(event_i);
    const int mode_i = std::clamp(static_cast<int>(r.execution_mode), 0,
                                  static_cast<int>(ExecutionMode::Count) - 1);
    r.execution_mode = static_cast<ExecutionMode>(mode_i);
    r.repeat_count = std::clamp(r.repeat_count, 1, 1000);
    r.repeat_interval_ms = std::clamp(r.repeat_interval_ms, 1, 600000);
    r.cooldown_ms = std::clamp(r.cooldown_ms, 0, 600000);

    for (size_t i = action_start; i < parts.size(); ++i)
    {
        // "type,a,b"
        int t = 0, av = 0, bv = 50;
        size_t p1 = parts[i].find(',');
        if (p1 == std::string::npos) continue;
        size_t p2 = parts[i].find(',', p1 + 1);
        try { t  = std::stoi(parts[i].substr(0, p1)); } catch (...) { continue; }
        try {
            av = std::stoi(parts[i].substr(
                p1 + 1,
                (p2 == std::string::npos) ? std::string::npos : (p2 - p1 - 1)));
        } catch (...) {}
        if (p2 != std::string::npos)
        {
            try { bv = std::stoi(parts[i].substr(p2 + 1)); } catch (...) {}
        }

        Action a;
        if (t < 0 || t >= static_cast<int>(ActionType::Count)) t = 0;
        a.type = static_cast<ActionType>(t);
        a.a = av;
        a.b = bv;
        r.actions.push_back(a);
    }
    return r;
}

const char* event_name(EventType e)
{
    const int i = static_cast<int>(e);
    if (i < 0 || i >= static_cast<int>(EventType::Count)) return "?";
    return kEventNames[i];
}
const char* action_name(ActionType a)
{
    const int i = static_cast<int>(a);
    if (i < 0 || i >= static_cast<int>(ActionType::Count)) return "?";
    return kActionNames[i];
}
const char* execution_mode_name(ExecutionMode mode)
{
    const int i = static_cast<int>(mode);
    if (i < 0 || i >= static_cast<int>(ExecutionMode::Count)) return "?";
    return kExecutionModeNames[i];
}

} // namespace event_orch
