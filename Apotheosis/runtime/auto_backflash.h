#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace auto_backflash
{

enum class Phase
{
    Idle = 0,
    Turning,
    Holding,
    Returning,
    Settling,
    Cooldown,
};

struct Params
{
    bool enabled = false;
    std::vector<int> classes;
    int confirm_frames = 2;
    int turn_amount = 4000;
    int turn_speed = 75;
    int return_delay_ms = 800;
    int return_speed = 100;
    int cooldown_ms = 1500;
};

// UI 只发布测试请求，真正的移动始终由 mouse thread 串行执行。
void request_test();
Phase published_phase();
int published_remaining();

class Controller
{
public:
    // 返回 true 表示正在接管鼠标；调用方本轮不得再发送瞄准移动。
    bool tick(const Params& params,
              bool has_new_detection,
              const std::vector<int>& classes,
              const std::vector<float>& confidences,
              const std::vector<float>& centers_x,
              float frame_center_x,
              std::chrono::steady_clock::time_point now,
              const std::function<bool(int)>& send_horizontal);

    // 会话停止时用于立即偿还尚未返回的水平位移。
    int emergency_return_delta();
    bool active() const;

private:
    void begin_turn(const Params& params,
                    std::chrono::steady_clock::time_point now,
                    bool testing,
                    int direction);
    void begin_return(const Params& params,
                      std::chrono::steady_clock::time_point now);
    bool advance_motion(int speed,
                        std::chrono::steady_clock::time_point now,
                        const std::function<bool(int)>& send_horizontal);
    void publish() const;

    Phase phase_ = Phase::Idle;
    int confirm_count_ = 0;
    int direction_ = 1;
    int remaining_ = 0;
    int displacement_ = 0;
    double fractional_move_ = 0.0;
    bool testing_ = false;
    int pending_cooldown_ms_ = 0;
    unsigned long long seen_test_generation_ = 0;
    std::chrono::steady_clock::time_point last_motion_time_{};
    std::chrono::steady_clock::time_point deadline_{};
};

} // namespace auto_backflash
