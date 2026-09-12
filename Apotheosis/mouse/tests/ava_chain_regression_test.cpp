#include "../boss_aim.h"
#include "../aim_path.h"
#include "../ava_exact/mouse_output_exact.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace
{

[[noreturn]] void fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, const char* message)
{
    if (!value)
        fail(message);
}

boss::EngineInput makeInput(const std::vector<cv::Rect2f>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences)
{
    boss::EngineInput input;
    input.boxes = &boxes;
    input.classes = &classes;
    input.confidences = &confidences;
    input.target_slots.push_back({0, 0.5f, 0.5f, 0.1f});
    input.crosshair_x = 160.0;
    input.crosshair_y = 160.0;
    input.fov_radius_x = 150.0;
    input.fov_radius_y = 150.0;
    input.image_size = 320.0;
    input.lost_target_cache_frames = 3;
    input.pidf_params.kp_x = 0.6;
    input.pidf_params.kp_y = 0.6;
    input.pidf_params.kd_x = 0.01;
    input.pidf_params.kd_y = 0.01;
    input.pidf_params.kf_x = 1.0;
    input.pidf_params.kf_y = 1.0;
    return input;
}

void testPredictedAimpointAndReacquire()
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};

    boss::EngineOutput output;
    bool saw_movement = false;
    for (int frame = 0; frame < 12; ++frame)
    {
        boxes[0] = cv::Rect2f(80.0f + frame * 5.0f, 130.0f, 20.0f, 40.0f);
        auto input = makeInput(boxes, classes, confidences);
        output = engine.tick(input, 1.0 / 120.0);
        require(output.have_target, "moving target must remain selected");
        saw_movement = saw_movement || output.dx != 0 || output.dy != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const float raw_center_x = static_cast<float>(boxes[0].x) + boxes[0].width * 0.5f;
    std::cout << "prediction lead: raw=" << raw_center_x
              << " aim=" << output.anchor.x
              << " lead=" << (output.anchor.x - raw_center_x) << '\n';
    require(output.anchor.x > raw_center_x,
            "AVA tracker prediction must feed target_to_aimpoint before PIDF");
    require(saw_movement,
            "canonical internal hotkey must activate the AVA movement pipeline");
    const int old_generation = output.current_track_id;

    boxes.clear();
    classes.clear();
    confidences.clear();
    auto missing = makeInput(boxes, classes, confidences);
    output = engine.tick(missing, 1.0 / 120.0);
    require(output.have_target && output.coasting,
            "short detection loss must use the AVA predicted track");
    require(output.current_track_id == old_generation,
            "coasting must not replace the target generation");

    boxes.emplace_back(145.0f, 130.0f, 20.0f, 40.0f);
    classes.push_back(0);
    confidences.push_back(0.9f);
    auto reacquired = makeInput(boxes, classes, confidences);
    output = engine.tick(reacquired, 1.0 / 120.0);
    require(output.have_target, "reacquired target must be installed");
    require(output.current_track_id != old_generation,
            "AVA reacquire must advance the target generation");
    require(output.motion_suppressed,
            "reacquire frame must preempt pending movement and reset per-target state");
}

void testAimpointLeadsBothDirections()
{
    auto run = [](int start_x, int step_x) {
        boss::AimEngine engine;
        std::vector<cv::Rect2f> boxes(1);
        std::vector<int> classes{0};
        std::vector<float> confidences{0.9f};
        boss::EngineOutput output;
        for (int frame = 0; frame < 12; ++frame)
        {
            boxes[0] = cv::Rect2f(
                static_cast<float>(start_x + frame * step_x),
                130.0f, 20.0f, 40.0f);
            auto input = makeInput(boxes, classes, confidences);
            input.pidf_params.kf_x = 5.0;
            input.pidf_params.lr_x = 0.02;
            output = engine.tick(input, 1.0 / 120.0);
            require(output.have_target, "directional target must remain selected");
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        const float raw_center = static_cast<float>(boxes[0].x)
            + static_cast<float>(boxes[0].width) * 0.5f;
        return std::pair<float, float>{raw_center, output.anchor.x};
    };

    const auto [left_raw, left_aim] = run(200, -5);
    const auto [right_raw, right_aim] = run(80, 5);
    std::cout << "directional aimpoint: left raw/aim="
              << left_raw << '/' << left_aim
              << " right raw/aim=" << right_raw << '/' << right_aim << '\n';
    require(left_aim < left_raw,
            "left-moving target must aim to the left of the current detection center");
    require(right_aim > right_raw,
            "right-moving target must aim to the right of the current detection center");
}

struct HighSpeedTrace
{
    std::vector<float> raw_centers;
    std::vector<float> predicted_aimpoints;
    std::vector<int> moves;
};

HighSpeedTrace runHighSpeedTracking(float start_x, float step_x)
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    HighSpeedTrace trace;
    for (int frame = 0; frame < 22; ++frame)
    {
        boxes[0] = cv::Rect2f(
            start_x + static_cast<float>(frame) * step_x,
            120.0f, 36.0f, 72.0f);
        auto input = makeInput(boxes, classes, confidences);
        input.pidf_params.kp_x = 1.0;
        input.pidf_params.kd_x = 0.01;
        input.pidf_params.kf_x = 3.0;
        input.pidf_params.lr_x = 0.08;
        const auto output = engine.tick(input, 1.0 / 120.0);
        require(output.have_target, "high-speed target must remain selected");
        trace.raw_centers.push_back(boxes[0].x + boxes[0].width * 0.5f);
        trace.predicted_aimpoints.push_back(output.anchor.x);
        trace.moves.push_back(output.dx);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return trace;
}

void testHighSpeedTrackingEffect()
{
    // 10 px / 8 ms 约等于 1250 px/s，用于直观检查高速双向跟踪。
    const auto left = runHighSpeedTracking(242.0f, -10.0f);
    const auto right = runHighSpeedTracking(42.0f, 10.0f);
    const float left_lead = left.predicted_aimpoints.back() - left.raw_centers.back();
    const float right_lead = right.predicted_aimpoints.back() - right.raw_centers.back();
    std::cout << "high-speed tracking (~1250 px/s): left raw/aim/lead="
              << left.raw_centers.back() << '/' << left.predicted_aimpoints.back()
              << '/' << left_lead
              << " right raw/aim/lead="
              << right.raw_centers.back() << '/' << right.predicted_aimpoints.back()
              << '/' << right_lead << '\n';
    std::cout << "high-speed tail (raw->aim):";
    for (std::size_t i = left.raw_centers.size() - 8;
         i < left.raw_centers.size(); ++i)
    {
        std::cout << ' ' << left.raw_centers[i] << "->"
                  << left.predicted_aimpoints[i];
    }
    std::cout << '\n';
    require(left_lead < -7.0f,
            "high-speed left target must keep a visible left-side lead");
    require(right_lead > 7.0f,
            "high-speed right target must keep a visible right-side lead");
}

void testAvaAimpointRangeIsStabilizedPerLock()
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes{cv::Rect2f(110, 100, 100, 100)};
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    auto input = makeInput(boxes, classes, confidences);
    input.target_slots = {{0, 0.70f, 0.80f, 0.1f}};

    auto output = engine.tick(input, 1.0 / 120.0);
    require(output.have_target, "random-Y target must be selected");
    const float first_ratio = (output.anchor.y - output.bbox.y) / output.bbox.height;
    require(std::fabs(first_ratio - 0.30f) < 0.0001f,
            "AVA must clamp the reference Y into the configured ratio range");

    // 同一锁定期间移动检测框，归一化 Y 应保持不变，不能逐帧重抽。
    boxes[0].y += 12;
    input.crosshair_y = 110.0;
    output = engine.tick(input, 1.0 / 120.0);
    require(output.have_target, "moving random-Y target must remain selected");
    const float second_ratio = (output.anchor.y - output.bbox.y) / output.bbox.height;
    require(std::fabs(second_ratio - first_ratio) < 0.0001f,
            "AVA aimpoint ratio must be cached for the lifetime of one lock");
}

void testSubpixelDetectorGeometryReachesAvaTracker()
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    boss::EngineOutput output;
    for (int frame = 0; frame < 16; ++frame)
    {
        boxes[0] = cv::Rect2f(
            100.125f + static_cast<float>(frame) * 0.375f,
            130.25f, 19.75f, 40.5f);
        auto input = makeInput(boxes, classes, confidences);
        output = engine.tick(input, 1.0 / 120.0);
        require(output.have_target, "subpixel target must remain selected");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const float integer_part = std::floor(output.bbox.x);
    require(std::fabs(output.bbox.x - integer_part) > 0.0001f,
            "detector geometry must not be quantized before the AVA tracker");
}

void testLatestOnlySlot()
{
    cvm::recovered::LatestMovementBatchSlot slot;
    const cvm::recovered::RelativeMove old_move{12, 0};
    const cvm::recovered::RelativeMove new_move{-7, 3};
    require(slot.publish(std::span(&old_move, 1)), "first batch publish failed");
    require(slot.publish(std::span(&new_move, 1)), "replacement batch publish failed");

    cvm::recovered::LatestMovementBatchSlot::TakenBatch batch;
    require(slot.wait_take(batch), "latest batch take failed");
    require(batch.moves.size() == 1 && batch.moves[0] == new_move,
            "latest-only slot returned a stale movement");

    const auto generation = batch.generation;
    require(slot.publish(std::span(&old_move, 1)), "third batch publish failed");
    require(slot.newer_than(generation),
            "generation must preempt a worker that holds an older batch");
    slot.stop();
}

void testUserAimPathShapesAvaPidfOutput()
{
    boss::AimPathDriver linear;
    boss::AimPathDriver::Params linear_params;
    linear_params.mode = boss::AimPathDriver::Mode::Linear;
    linear.configure(linear_params);
    const auto passthrough = linear.step(
        260.0, 160.0, 160.0, 160.0, 1.0 / 120.0, 7, 12, 0);
    require(passthrough.move_x == 12 && passthrough.move_y == 0,
            "linear AimPath must preserve the AVA PIDF output exactly");

    boss::AimPathDriver custom;
    boss::AimPathDriver::Params custom_params;
    custom_params.mode = boss::AimPathDriver::Mode::Custom;
    custom_params.custom_samples = std::make_shared<const std::vector<float>>(
        std::initializer_list<float>{0.0f, 0.35f, 0.35f, 0.0f});
    custom_params.strength = 0.25;
    custom.configure(custom_params);
    // 首帧从直线平滑进入；部分影响经过整数 residual 后应逐步可见，
    // 而不是首帧突然完全转向。
    boss::AimPathDriver::Result shaped;
    int max_abs_y = 0;
    int max_frame_jump = 0;
    boss::AimPathDriver::Result previous{12, 0};
    for (int frame = 0; frame < 20; ++frame) {
        shaped = custom.step(
            260.0, 160.0, 160.0, 160.0, 1.0 / 120.0, 7, 12, 0);
        max_abs_y = std::max(max_abs_y, std::abs(shaped.move_y));
        max_frame_jump = std::max(max_frame_jump,
            std::abs(shaped.move_x - previous.move_x)
            + std::abs(shaped.move_y - previous.move_y));
        previous = shaped;
    }
    require(max_abs_y != 0,
            "custom AimPath must shape the post-PIDF direction");
    require(std::abs(shaped.move_x) > std::abs(shaped.move_y),
            "partial AimPath influence must not take over the PIDF direction");
    require(max_frame_jump <= 5,
            "partial AimPath influence must not create frame-to-frame direction stalls");
    std::cout << "AimPath partial influence: max-y=" << max_abs_y
              << " max-jump=" << max_frame_jump << '\n';
    require(std::abs(std::hypot(static_cast<double>(shaped.move_x),
                                static_cast<double>(shaped.move_y)) - 12.0) < 2.0,
            "AimPath must preserve the PIDF movement magnitude approximately");
}

void inspectKfDifferential()
{
    boss::AimEngine no_kf;
    boss::AimEngine with_kf;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    long long sum_no_kf = 0;
    long long sum_with_kf = 0;
    std::cout << "KF differential:";
    for (int frame = 0; frame < 20; ++frame)
    {
        boxes[0] = cv::Rect2f(60.0f + frame * 5.0f, 130.0f, 20.0f, 40.0f);
        auto a = makeInput(boxes, classes, confidences);
        auto b = makeInput(boxes, classes, confidences);
        a.pidf_params.kf_x = 0.0;
        b.pidf_params.kf_x = 3.0;
        a.pidf_params.lr_x = 0.1;
        b.pidf_params.lr_x = 0.1;
        const auto oa = no_kf.tick(a, 1.0 / 120.0);
        const auto ob = with_kf.tick(b, 1.0 / 120.0);
        sum_no_kf += oa.dx;
        sum_with_kf += ob.dx;
        std::cout << ' ' << oa.dx << '/' << ob.dx;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << " sums=" << sum_no_kf << '/' << sum_with_kf << '\n';
    require(sum_no_kf != sum_with_kf,
            "PIDF Kf must change the moving-target output history");
}

void inspectLrDifferential()
{
    boss::AimEngine frozen_lr;
    boss::AimEngine active_lr;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    long long sum_frozen = 0;
    long long sum_active = 0;
    std::cout << "LR differential:";
    for (int frame = 0; frame < 20; ++frame)
    {
        boxes[0] = cv::Rect2f(60.0f + frame * 5.0f, 130.0f, 20.0f, 40.0f);
        auto a = makeInput(boxes, classes, confidences);
        auto b = makeInput(boxes, classes, confidences);
        // LR 只有在 Kf 非零时才参与前馈；保持 Kf 相同，单独验证
        // “预测速度”从 0 改为 1 确实进入运行中的 PIDF 状态。
        a.pidf_params.kf_x = 3.0;
        b.pidf_params.kf_x = 3.0;
        a.pidf_params.lr_x = 0.0;
        b.pidf_params.lr_x = 1.0;
        const auto oa = frozen_lr.tick(a, 1.0 / 120.0);
        const auto ob = active_lr.tick(b, 1.0 / 120.0);
        sum_frozen += oa.dx;
        sum_active += ob.dx;
        std::cout << ' ' << oa.dx << '/' << ob.dx;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << " sums=" << sum_frozen << '/' << sum_active << '\n';
    require(sum_frozen != sum_active,
            "PIDF LR=0 and LR=1 must not produce the same moving-target history when Kf is nonzero");
}

void testPidfDeadzonePreservesStableAimY()
{
    boss::AimEngine no_deadzone;
    boss::AimEngine with_deadzone;
    std::vector<cv::Rect2f> boxes{cv::Rect2f(154.0f, 140.0f, 20.0f, 40.0f)};
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    long long open_sum = 0;
    long long deadzone_sum = 0;
    float stable_y = 0.0f;

    for (int frame = 0; frame < 10; ++frame)
    {
        auto open = makeInput(boxes, classes, confidences);
        auto dead = makeInput(boxes, classes, confidences);
        // 使用纵向范围模式，验证死区不会改变或重抽稳定 Y 瞄点。
        open.target_slots[0].y_offset_min = 0.2f;
        open.target_slots[0].y_offset_max = 0.8f;
        dead.target_slots = open.target_slots;
        open.pidf_params.kp_x = dead.pidf_params.kp_x = 1.0;
        open.pidf_params.kd_x = dead.pidf_params.kd_x = 0.0;
        open.pidf_params.kf_x = dead.pidf_params.kf_x = 0.0;
        open.pidf_params.lr_x = dead.pidf_params.lr_x = 0.0;
        dead.pidf_params.deadzone_x = 8;

        const auto a = no_deadzone.tick(open, 1.0 / 120.0);
        const auto b = with_deadzone.tick(dead, 1.0 / 120.0);
        require(a.have_target && b.have_target,
                "deadzone comparison must keep the target selected");
        require(std::fabs(a.anchor.y - b.anchor.y) < 0.0001f,
                "PIDF deadzone must not change the stable Y aimpoint");
        if (frame == 0)
            stable_y = b.anchor.y;
        require(std::fabs(b.anchor.y - stable_y) < 0.0001f,
                "PIDF deadzone must not redraw the Y aimpoint");
        open_sum += std::abs(a.dx);
        deadzone_sum += std::abs(b.dx);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "deadzone/stable-Y: open=" << open_sum
              << " deadzone=" << deadzone_sum
              << " aim-y=" << stable_y << '\n';
    require(open_sum > 0, "baseline micro-movement must be observable");
    require(deadzone_sum == 0,
            "AVA PIDF deadzone must suppress only the final micro-movement");
}

double runClosedLoop(double target_velocity, double lock_strength,
                     double prediction_speed)
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    double error = target_velocity < 0.0 ? 60.0 : -60.0;
    for (int frame = 0; frame < 80; ++frame)
    {
        const int center = static_cast<int>(std::lround(160.0 + error));
        boxes[0] = cv::Rect2f(
            static_cast<float>(center - 10), 130.0f, 20.0f, 40.0f);
        auto input = makeInput(boxes, classes, confidences);
        input.pidf_params.kp_x = 1.0;
        // AVA 可见参数：锁定强度 -> Kf，预测速度 -> LR。
        input.pidf_params.kf_x = lock_strength;
        input.pidf_params.lr_x = prediction_speed;
        const auto output = engine.tick(input, 1.0 / 120.0);
        // Positive mouse motion rotates the view right, making the target move
        // left by the same number of screen pixels.
        error += target_velocity - static_cast<double>(output.dx);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return error;
}

void testClosedLoopLeadUsesAvaVisibleMapping()
{
    const double left_without_prediction = runClosedLoop(-3.0, 0.0, 0.0);
    const double left_with_prediction = runClosedLoop(-3.0, 5.0, 0.02);
    const double right_with_prediction = runClosedLoop(3.0, 5.0, 0.02);
    std::cout << "closed-loop mapping: left-off=" << left_without_prediction
              << " left-on=" << left_with_prediction
              << " right-on=" << right_with_prediction << '\n';
    require(left_without_prediction < 0.0,
            "without AVA prediction a left-moving target should expose lag");
    require(left_with_prediction > 0.0,
            "AVA lock/prediction mapping must lead a left-moving target");
    require(right_with_prediction < 0.0,
            "AVA lock/prediction mapping must lead a right-moving target");
}

struct NoisyRunMetrics
{
    double final_error{};
    double tail_mean_error{};
    int max_abs_move{};
    int max_move_jump{};
    long long total_variation{};
    int zero_frames{};
    std::vector<int> tail_moves;
};

NoisyRunMetrics runNoisyCadenceClosedLoop(double lock_strength,
                                           double prediction_speed)
{
    boss::AimEngine engine;
    std::vector<cv::Rect2f> boxes(1);
    std::vector<int> classes{0};
    std::vector<float> confidences{0.9f};
    constexpr int cadence_ms[] = {7, 8, 15, 7, 20, 9, 7, 13};
    constexpr int center_noise[] = {0, 1, -1, 0, 1, 0, -1, 1, 0, -1};
    constexpr int size_noise[] = {0, 1, 0, -1, 1, -1, 0, 0};
    constexpr double target_velocity_px_s = -240.0;
    double true_error = 70.0;
    double tail_sum = 0.0;
    int tail_count = 0;
    int previous_move = 0;
    NoisyRunMetrics metrics;
    for (int frame = 0; frame < 120; ++frame)
    {
        const int sleep_ms = cadence_ms[frame % std::size(cadence_ms)];
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        true_error += target_velocity_px_s * (static_cast<double>(sleep_ms) * 0.001);

        const float measured_center = static_cast<float>(
            160.0 + true_error
            + 0.35 * center_noise[frame % std::size(center_noise)]);
        const float width = 20.0f
            + 0.35f * size_noise[frame % std::size(size_noise)];
        const float height = 40.0f
            + 0.35f * size_noise[(frame + 3) % std::size(size_noise)];
        boxes[0] = cv::Rect2f(measured_center - width * 0.5f,
                              160.0f - height * 0.5f, width, height);
        auto input = makeInput(boxes, classes, confidences);
        input.pidf_params.kp_x = 1.0;
        input.pidf_params.kd_x = 0.01;
        input.pidf_params.kf_x = lock_strength;
        input.pidf_params.lr_x = prediction_speed;
        const auto output = engine.tick(input, static_cast<double>(sleep_ms) * 0.001);
        true_error -= static_cast<double>(output.dx);

        metrics.max_abs_move = std::max(metrics.max_abs_move, std::abs(output.dx));
        const int jump = std::abs(output.dx - previous_move);
        metrics.max_move_jump = std::max(metrics.max_move_jump, jump);
        metrics.total_variation += jump;
        metrics.zero_frames += output.dx == 0;
        previous_move = output.dx;
        if (frame >= 90) {
            tail_sum += true_error;
            ++tail_count;
            metrics.tail_moves.push_back(output.dx);
        }
    }
    metrics.final_error = true_error;
    metrics.tail_mean_error = tail_count > 0 ? tail_sum / tail_count : true_error;
    return metrics;
}

void inspectNoisyCadenceTradeoff()
{
    const auto high_slow = runNoisyCadenceClosedLoop(5.0, 0.01);
    const auto high_fast = runNoisyCadenceClosedLoop(5.0, 1.0);
    const auto low_fast = runNoisyCadenceClosedLoop(0.8, 1.0);
    const auto print = [](const char* name, const NoisyRunMetrics& value) {
        std::cout << name
                  << ": final=" << value.final_error
                  << " tail-mean=" << value.tail_mean_error
                  << " max|dx|=" << value.max_abs_move
                  << " max-jump=" << value.max_move_jump
                  << " variation=" << value.total_variation
                  << " zero-frames=" << value.zero_frames
                  << " tail=";
        for (const int move : value.tail_moves)
            std::cout << move << ',';
        std::cout << '\n';
    };
    std::cout << "noisy cadence tradeoff:\n";
    print("  high-Kf/low-LR", high_slow);
    print("  high-Kf/high-LR", high_fast);
    print("  low-Kf/high-LR", low_fast);
}

} // namespace

int main()
{
    testPredictedAimpointAndReacquire();
    testAimpointLeadsBothDirections();
    testHighSpeedTrackingEffect();
    testAvaAimpointRangeIsStabilizedPerLock();
    testSubpixelDetectorGeometryReachesAvaTracker();
    inspectKfDifferential();
    inspectLrDifferential();
    testPidfDeadzonePreservesStableAimY();
    testClosedLoopLeadUsesAvaVisibleMapping();
    inspectNoisyCadenceTradeoff();
    testUserAimPathShapesAvaPidfOutput();
    testLatestOnlySlot();
    std::cout << "AVA chain regression: ALL PASS\n";
    return 0;
}
