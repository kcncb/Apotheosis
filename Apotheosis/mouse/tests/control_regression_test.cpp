#include "../aim_path.h"
#include "../boss_aim.h"
#include "../movers.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* name)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++failures;
}
}

int main()
{
    constexpr double dt = 1.0 / 240.0;
    constexpr double cross = 160.0;

    // Curves must advance in the fixed-crosshair coordinate model, retain the
    // base controller's magnitude, and stop when the base controller stops.
    {
        boss::AimPathDriver path;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Bezier;
        p.cx1 = 0.30; p.cy1 = 0.25;
        p.cx2 = 0.70; p.cy2 = -0.15;
        path.configure(p);

        double cursor_x = 0.0;
        double cursor_y = 0.0;
        bool curved = false;
        bool bounded = true;
        for (int frame = 0; frame < 14; ++frame)
        {
            const double ex = 100.0 - cursor_x;
            const double ey = -cursor_y;
            const int base_x = static_cast<int>(std::lround(ex * 0.20));
            const int base_y = static_cast<int>(std::lround(ey * 0.20));
            const double base_mag = std::hypot(static_cast<double>(base_x),
                                               static_cast<double>(base_y));
            const auto out = path.step(cross + ex, cross + ey, cross, cross,
                                       1.0 / 120.0, 11, base_x, base_y);
            const double shaped_mag = std::hypot(static_cast<double>(out.move_x),
                                                 static_cast<double>(out.move_y));
            bounded = bounded && shaped_mag <= std::max(1.0, base_mag * 1.25) + 1.5;
            curved = curved || out.move_y != 0;
            path.applyMove(out.move_x, out.move_y);
            cursor_x += out.move_x;
            cursor_y += out.move_y;
        }
        check(curved, "Bezier produces a real curved trajectory");
        check(bounded, "Bezier never amplifies a base step into a large jump");
        check(cursor_x > 60.0, "Bezier virtual progress advances across frames");

        const auto stopped = path.step(cross + (100.0 - cursor_x),
                                       cross - cursor_y, cross, cross,
                                       1.0 / 120.0, 11, 0, 0);
        check(stopped.move_x == 0 && stopped.move_y == 0,
              "Curve respects controller deadzone output");
    }

    // A flat curve is the neutral preset. Enabling it must not change an
    // already tuned controller's per-axis output, including unequal X/Y gains.
    {
        boss::AimPathDriver path;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Bezier;
        p.cx1 = 0.30; p.cy1 = 0.0;
        p.cx2 = 0.70; p.cy2 = 0.0;
        path.configure(p);

        bool identical = true;
        const std::vector<std::pair<int, int>> base_moves{
            {13, 2}, {9, -3}, {5, 1}, {2, -1}
        };
        for (const auto& move : base_moves)
        {
            const auto out = path.step(cross + 100.0, cross + 35.0,
                                       cross, cross, dt, 21,
                                       move.first, move.second);
            identical = identical && out.move_x == move.first && out.move_y == move.second;
        }
        check(identical, "Flat curve is bit-exact neutral for tuned controller output");
    }

    // A moving target can outlive the first start-to-goal segment. Once that
    // segment completes, the same lock must start another curved segment
    // instead of remaining forever on the flat endpoint tangent.
    {
        boss::AimPathDriver path;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Bezier;
        p.cx1 = 0.25; p.cy1 = 0.30;
        p.cx2 = 0.75; p.cy2 = 0.0; // endpoint tangent is deliberately flat
        path.configure(p);

        bool first_segment_curved = false;
        bool later_segment_curved = false;
        for (int frame = 0; frame < 20; ++frame)
        {
            const auto out = path.step(cross + 40.0, cross, cross, cross,
                                       dt, 22, 10, 0);
            if (frame < 8) first_segment_curved |= out.move_y != 0;
            if (frame >= 8) later_segment_curved |= out.move_y != 0;
        }
        check(first_segment_curved && later_segment_curved,
              "Curve restarts for later movement on the same target lock");
    }

    // Long noisy trace: shared high-resolution assets must remain bounded over
    // variable frame rates and repeated segments without resetting each frame.
    {
        auto samples = std::make_shared<std::vector<float>>(
            boss::AimPathDriver::kCustomSamples, 0.0f);
        for (int i = 0; i < boss::AimPathDriver::kCustomSamples; ++i) {
            const double t = static_cast<double>(i) /
                           (boss::AimPathDriver::kCustomSamples - 1);
            (*samples)[static_cast<size_t>(i)] = static_cast<float>(
                0.12 * std::sin(2.0 * 3.141592653589793 * t));
        }
        samples->front() = samples->back() = 0.0f;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Custom;
        p.custom_samples = samples;
        boss::AimPathDriver path;
        path.configure(p);
        bool bounded = true;
        bool remains_curved = false;
        for (int frame = 0; frame < 600; ++frame) {
            const double noise = static_cast<double>((frame % 5) - 2);
            const int base_x = (frame % 90 < 70) ? 7 : -5;
            const int base_y = (frame % 11 == 0) ? 1 : 0;
            const double ex = base_x > 0 ? 80.0 + noise : -60.0 + noise;
            const auto out = path.step(cross + ex, cross, cross, cross,
                                       (frame % 3 == 0) ? 1.0 / 60.0 : 1.0 / 240.0,
                                       23, base_x, base_y);
            const double inputMag = std::hypot(base_x, base_y);
            const double outputMag = std::hypot(out.move_x, out.move_y);
            bounded &= outputMag <= inputMag + 2.0;
            if (frame > 300 && out.move_y != base_y) remains_curved = true;
            path.configure(p); // same shared asset must not reset the segment
        }
        check(bounded && remains_curved,
              "High-resolution curve stays bounded under noise and variable FPS");
    }

    // Neural custom mode evaluates the compact 25-weight MLP directly; no
    // 32768-point table is required on the runtime control path.
    {
        boss::AimPathDriver path;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Custom;
        p.neural_enabled = true;
        p.neural_weights.fill(0.0f);
        p.neural_weights[0] = 0.8f;
        p.neural_weights[8] = 0.2f;
        p.neural_weights[16] = 0.7f;
        path.configure(p);
        bool curved = false;
        bool bounded = true;
        for (int frame = 0; frame < 80; ++frame) {
            const auto out = path.step(cross + 90.0, cross, cross, cross,
                                       (frame & 1) ? 1.0 / 60.0 : 1.0 / 240.0,
                                       24, 8, 0);
            curved |= out.move_y != 0;
            bounded &= std::hypot(out.move_x, out.move_y) <= 10.0;
        }
        check(curved && bounded, "Compact neural weights drive a bounded continuous curve");

        auto changed = p;
        changed.neural_weights[8] = -0.4f;
        path.configure(changed);
        const auto switched = path.step(cross + 90.0, cross, cross, cross,
                                        dt, 24, 8, 0);
        check(std::hypot(switched.move_x, switched.move_y) <= 10.0,
              "Neural curve hot-switch resets safely without a jump");
    }

    // Engine-level contract: random Y is sampled once per lock, coasting keeps
    // identity without moving/firing, and the configured frame count expires.
    {
        boss::AimEngine engine;
        std::vector<cv::Rect> boxes{cv::Rect(120, 80, 80, 100)};
        std::vector<int> classes{0};
        std::vector<float> confidences{0.9f};
        boss::EngineInput in;
        in.boxes = &boxes;
        in.classes = &classes;
        in.confidences = &confidences;
        in.crosshair_x = 160.0;
        in.crosshair_y = 160.0;
        in.fov_radius_x = 500.0;
        in.fov_radius_y = 500.0;
        in.image_size = 320.0;
        in.lost_target_cache_frames = 2;
        boss::TargetSlot slot;
        slot.class_id = 0;
        slot.y_offset_min = 0.10f;
        slot.y_offset_max = 0.30f;
        in.target_slots.push_back(slot);

        const auto first = engine.tick(in, 1.0 / 120.0);
        const double sampled = 1.0 - (first.anchor.y - boxes[0].y) / boxes[0].height;
        const auto second = engine.tick(in, 1.0 / 120.0);
        check(first.have_target && sampled >= 0.10 && sampled <= 0.30,
              "Random lock point stays inside configured range");
        check(std::fabs(first.anchor.y - second.anchor.y) < 1e-5,
              "Random lock point is stable for the same track");

        boxes.clear(); classes.clear(); confidences.clear();
        const auto miss1 = engine.tick(in, 1.0 / 120.0);
        const auto miss2 = engine.tick(in, 1.0 / 120.0);
        const auto miss3 = engine.tick(in, 1.0 / 120.0);
        check(miss1.have_target && miss1.coasting && miss1.motion_suppressed &&
              miss2.have_target && miss2.coasting,
              "Lost-target cache holds identity but suppresses motion");
        check(!miss3.have_target,
              "Lost-target cache expires at configured frame count");
    }

    // Shared deadzone uses bbox half-size semantics and requests queue clearing.
    {
        boss::AimEngine engine;
        std::vector<cv::Rect> boxes{cv::Rect(110, 110, 100, 100)};
        std::vector<int> classes{0};
        std::vector<float> confidences{0.9f};
        boss::EngineInput in;
        in.boxes = &boxes;
        in.classes = &classes;
        in.confidences = &confidences;
        in.target_slots.push_back(boss::TargetSlot{0, 0.5f, 0.5f, 0.0f});
        in.crosshair_x = in.crosshair_y = 160.0;
        in.fov_radius_x = in.fov_radius_y = 500.0;
        in.image_size = 320.0;
        in.deadzone_enabled = true;
        in.deadzone_percent = 10.0f;
        const auto out = engine.tick(in, 1.0 / 120.0);
        check(out.have_target && out.dx == 0 && out.dy == 0 &&
              out.motion_suppressed,
              "Shared deadzone suppresses output and stale queued moves");
    }

    // 摇光必须在 60..240 FPS 连续范围内保持同方向且不跨过静止目标。
    {
        boss::AimPathDriver path;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::Bezier;
        p.cx1 = 0.25; p.cy1 = 0.8;
        p.cx2 = 0.75; p.cy2 = -0.8;
        path.configure(p);
        const auto near = path.step(168.0, 160.0, 160.0, 160.0,
                                    1.0 / 120.0, 9, 4, 0, 10.0);
        check(near.move_x == 4 && near.move_y == 0,
              "Curve settles to raw PID near target instead of orbiting");
    }

    for (double fps : {60.0, 90.0, 120.0, 165.0, 200.0, 240.0})
    {
        mover::YaoguangMover m;
        mover::YaoguangParams p;
        m.configure(p);
        const auto out = m.step(260.0, 160.0, 160.0, 160.0,
                                40.0, 80.0, 320.0, 1.0 / fps, 1);
        check(out.dx >= 0 && out.dx <= 100 && out.dy == 0,
              "Yaoguang static capture stays bounded across variable FPS");
    }

    // 摇光预测只属于控制器自身，与扳机开关无关。
    {
        mover::YaoguangMover tracking;
        mover::YaoguangMover firing;
        mover::YaoguangParams p;
        p.prediction_ms = 0.0;
        tracking.configure(p);
        p.prediction_ms = 50.0;
        firing.configure(p);
        mover::Move center{}, lead{};
        double target_x = 180.0;
        for (int i = 0; i < 50; ++i)
        {
            target_x += 240.0 / 120.0;
            center = tracking.step(target_x, 160.0, 160.0, 160.0,
                                   40.0, 80.0, 320.0, 1.0 / 120.0, 7);
            lead = firing.step(target_x, 160.0, 160.0, 160.0,
                               40.0, 80.0, 320.0, 1.0 / 120.0, 7);
        }
        check(lead.aim_x > center.aim_x,
              "Yaoguang prediction leads stable lateral motion independently");
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILED") << '\n';
    return failures == 0 ? 0 : 1;
}
