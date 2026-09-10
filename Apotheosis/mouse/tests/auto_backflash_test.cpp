#include "runtime/auto_backflash.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

auto_backflash::Params params()
{
    auto_backflash::Params p;
    p.enabled = true;
    p.classes = {7, 9};
    p.confirm_frames = 2;
    p.turn_amount = 1000;
    p.turn_speed = 100;
    p.return_delay_ms = 10;
    p.return_speed = 100;
    p.cooldown_ms = 0;
    return p;
}
}

int main()
{
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{} + std::chrono::seconds(1);
    auto_backflash::Controller controller;
    auto p = params();
    int position = 0;
    int max_distance = 0;
    int min_position = 0;
    int max_position = 0;
    int rejected_requests = 3;
    const auto send = [&](int dx) {
        if (rejected_requests > 0)
        {
            --rejected_requests;
            return false;
        }
        position += dx;
        max_distance = std::max(max_distance, std::abs(position));
        min_position = std::min(min_position, position);
        max_position = std::max(max_position, position);
        return true;
    };

    require(!controller.tick(p, true, {7}, {0.9f}, {80.0f}, 160.0f, now, send),
            "one detection frame must not trigger");
    now += std::chrono::milliseconds(1);
    require(controller.tick(p, true, {7}, {0.9f}, {80.0f}, 160.0f, now, send),
            "second consecutive selected-class frame must trigger");

    for (int i = 0; i < 1000 && controller.active(); ++i)
    {
        now += std::chrono::milliseconds(1);
        controller.tick(p, false, {}, {}, {}, 160.0f, now, send);
    }
    require(!controller.active(), "turn/hold/return sequence must finish");
    require(rejected_requests == 0,
            "failed transport requests must be retried without bookkeeping");
    require(max_distance == p.turn_amount,
            "turn must reach the configured total amount");
    require(max_position == p.turn_amount && min_position == 0,
            "left-side detection must turn right before returning");
    require(position == 0,
            "return must exactly repay the accumulated turn amount");

    // 中途关闭功能也必须立即归还已经发送的位移。
    position = 0;
    max_distance = 0;
    min_position = 0;
    max_position = 0;
    now += std::chrono::milliseconds(1);
    controller.tick(p, true, {9}, {0.8f}, {240.0f}, 160.0f, now, send);
    now += std::chrono::milliseconds(1);
    controller.tick(p, true, {9}, {0.8f}, {240.0f}, 160.0f, now, send);
    for (int i = 0; i < 5; ++i)
    {
        now += std::chrono::milliseconds(1);
        controller.tick(p, false, {}, {}, {}, 160.0f, now, send);
    }
    p.enabled = false;
    for (int i = 0; i < 1000 && controller.active(); ++i)
    {
        now += std::chrono::milliseconds(1);
        controller.tick(p, false, {}, {}, {}, 160.0f, now, send);
    }
    require(position == 0, "disabling during a turn must restore the view");
    require(min_position < 0 && max_position == 0,
            "right-side detection must turn left before returning");

    std::cout << "Auto backflash regression: ALL PASS\n";
    return 0;
}
