// =============================================================================
// runtime/latency_probe.h 自测
// =============================================================================
//
// 探针是纯 header、纯 std 依赖, 因此在任何平台都能编译运行 (不牵扯 Windows /
// CUDA / OpenCV)。这让它可以脱离 Apotheosis 主工程单独回归。
//
//   c++ -std=c++20 -fno-char8_t -I Apotheosis Apotheosis/runtime/tests/latency_probe_test.cpp -o lp_test
//
// 覆盖: 阶段数学 / 丢帧统计 / 无戳消费 / reset / 开关 / 并发冒烟
// =============================================================================

#include "runtime/latency_probe.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("  [FAIL] %s\n", msg); ++g_failures; }          \
        else printf("  [ ok ] %s\n", msg);                                     \
    } while (0)

namespace lat = runtime::latency;

static void spin_us(int us)
{
    const int64_t until = lat::nowNs() + static_cast<int64_t>(us) * 1000;
    while (lat::nowNs() < until) { /* busy wait: 让区间可预测 */ }
}

// 跑一整帧, 三段各插入指定的忙等。
static void runFrame(int capWaitUs, int inferUs, int pubToAimUs)
{
    lat::noteCaptureForStats(lat::markCapture());
    spin_us(capWaitUs);
    const int64_t cap = lat::markSubmit();
    spin_us(inferUs);
    lat::markInferenceDone();
    spin_us(pubToAimUs);
    const int64_t pub = lat::nowNs();
    const auto aim = lat::markAimConsume(cap, pub);
    spin_us(500);
    lat::markMoveSent(cap, aim);
}

int main()
{
    printf(u8"=== latency_probe 自测 ===\n\n");

    // ---------------------------------------------------------------- [1]
    printf(u8"[1] 阶段拆分与总延迟\n");
    {
        lat::reset();
        for (int i = 0; i < 60; ++i) runFrame(0, 0, 0);   // 预热 EMA
        lat::reset();
        for (int i = 0; i < 40; ++i) runFrame(200, 3000, 100);

        const auto s = lat::snapshot();
        CHECK(s.frames_consumed == 40, u8"结算帧数 == 40");
        CHECK(s.stages[lat::kTotal].n == 40, u8"总延迟样本数 == 40");

        // 采集->取帧 应约 0.2ms, 推理 应约 3.0ms
        CHECK(s.stages[lat::kCaptureWait].ema_ms > 0.1 &&
              s.stages[lat::kCaptureWait].ema_ms < 2.0,
              u8"采集->取帧 EMA 落在 [0.1, 2.0] ms");
        CHECK(s.stages[lat::kInference].ema_ms > 2.5 &&
              s.stages[lat::kInference].ema_ms < 6.0,
              u8"推理 EMA 落在 [2.5, 6.0] ms");

        // 总延迟 = 三段之和, 必须 >= 各段
        CHECK(s.stages[lat::kTotal].ema_ms >= s.stages[lat::kInference].ema_ms,
              u8"总延迟 >= 推理段");
        CHECK(s.stages[lat::kEndToEnd].ema_ms > s.stages[lat::kTotal].ema_ms,
              u8"全链路(T0->T4) > 总延迟(T0->T3)");
        CHECK(s.stages[lat::kTotal].max_ms >= s.stages[lat::kTotal].ema_ms,
              u8"峰值 >= 均值");
        printf("      total=%.2f  cap->det=%.2f  infer=%.2f  pub->aim=%.2f  e2e=%.2f (ms)\n",
               s.stages[lat::kTotal].ema_ms, s.stages[lat::kCaptureWait].ema_ms,
               s.stages[lat::kInference].ema_ms, s.stages[lat::kPublishToAim].ema_ms,
               s.stages[lat::kEndToEnd].ema_ms);
    }

    // ---------------------------------------------------------------- [2]
    printf(u8"\n[2] detector 跟不上时统计丢帧\n");
    {
        lat::reset();
        for (int i = 0; i < 5; ++i) runFrame(0, 0, 0);
        CHECK(lat::snapshot().dropped_capture == 0, u8"连续取帧不记账为丢帧");

        // 模拟 detector 卡了 3 帧: 采集继续产帧, 但没人 markSubmit
        for (int i = 0; i < 3; ++i) lat::noteCaptureForStats(lat::markCapture());
        runFrame(0, 0, 0);   // detector 恢复, 一次取走最新的

        const auto s = lat::snapshot();
        CHECK(s.dropped_capture == 3, u8"跳过 3 帧被计入 dropped_capture");
        printf("      dropped=%llu capture=%llu\n",
               (unsigned long long)s.dropped_capture,
               (unsigned long long)s.capture_frames);
    }

    // ---------------------------------------------------------------- [3]
    printf(u8"\n[3] 无采集戳的消费不应污染统计\n");
    {
        lat::reset();
        for (int i = 0; i < 10; ++i) runFrame(0, 0, 0);
        const uint64_t before = lat::snapshot().stages[lat::kTotal].n;

        // frame_stamp_ns == 0 (空检测帧 / 采集不可用)
        lat::markAimConsume(0, lat::nowNs());
        lat::markAimConsume(0, lat::nowNs());

        const auto s = lat::snapshot();
        CHECK(s.stages[lat::kTotal].n == before, u8"无戳消费不进 total 样本");
        CHECK(s.stale_consumes == 2, u8"无戳消费计入 stale_consumes");
    }

    // ---------------------------------------------------------------- [4]
    printf(u8"\n[4] reset 清空\n");
    {
        lat::reset();
        const auto s = lat::snapshot();
        CHECK(s.frames_consumed == 0 && s.dropped_capture == 0, u8"计数器归零");
        CHECK(s.stages[lat::kTotal].n == 0 && s.stages[lat::kTotal].max_ms == 0.0,
              u8"阶段统计归零");
    }

    // ---------------------------------------------------------------- [5]
    printf(u8"\n[5] 开关\n");
    {
        lat::setEnabled(false);
        CHECK(!lat::enabled(), u8"setEnabled(false) 生效");
        CHECK(lat::formatLinesAscii(true).size() == 1, u8"关闭时只输出一行提示");
        lat::setEnabled(true);
        CHECK(lat::enabled(), u8"重新开启");
    }

    // ---------------------------------------------------------------- [6]
    printf(u8"\n[6] 输出格式\n");
    {
        lat::reset();
        for (int i = 0; i < 20; ++i) runFrame(0, 500, 0);

        const auto ascii = lat::formatLinesAscii(true);
        CHECK(!ascii.empty(), u8"ASCII 版非空");
        bool all_ascii = true;
        for (const auto& line : ascii)
            for (unsigned char ch : line)
                if (ch > 0x7E || (ch < 0x20 && ch != 0x09)) all_ascii = false;
        CHECK(all_ascii, u8"ASCII 版不含非 ASCII 字节 (OpenCV putText 只能画 ASCII)");

        const std::string cn = lat::formatLines(true);
        CHECK(cn.find(u8"总延迟") != std::string::npos, u8"中文版含 总延迟 字样");
        printf("      --- ascii ---\n");
        for (const auto& line : ascii) printf("      %s\n", line.c_str());
    }

    // ---------------------------------------------------------------- [7]
    printf(u8"\n[7] 并发冒烟 (采集线程 + 消费线程)\n");
    {
        lat::reset();
        std::atomic<bool> run{true};

        std::thread producer([&] {
            while (run.load()) {
                lat::noteCaptureForStats(lat::markCapture());
                spin_us(200);
                lat::markSubmit();
                lat::markInferenceDone();
            }
        });
        std::thread consumer([&] {
            while (run.load()) {
                const int64_t cap = lat::loadCaptureNs();
                if (cap != 0) lat::markAimConsume(cap, lat::nowNs());
                spin_us(300);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        run = false;
        producer.join();
        consumer.join();

        const auto s = lat::snapshot();
        CHECK(s.frames_consumed > 0, u8"并发下仍结算到帧");
        CHECK(s.stages[lat::kTotal].ema_ms >= 0.0, u8"并发下统计无异常值");
        printf("      frames=%llu  total=%.2f ms\n",
               (unsigned long long)s.frames_consumed, s.stages[lat::kTotal].ema_ms);
    }


    // The same frame stamp must survive backend decoding and overlapping input.
    {
        lat::reset();
        const auto received = lat::nowNs() - 40'000'000;
        lat::noteCaptureForStats(lat::markCapture(received));
        auto first = lat::markSubmitStamp();
        lat::markDetectorConsume(first);
        lat::noteCaptureForStats(lat::markCapture());
        const auto overwritten = lat::markSubmitStamp();
        lat::noteCaptureForStats(lat::markCapture());
        auto newest = lat::markSubmitStamp();
        lat::markInferenceDone(first.submit_ns);
        lat::markAimConsume(first.capture_ns, lat::nowNs());
        CHECK(lat::snapshot().stages[lat::kTotal].last_ms >= 40.0,
              "source decode/queue age survives a newer frame submission");
        CHECK(lat::snapshot().stages[lat::kCaptureWait].last_ms >= 40.0,
              "backend processing is included before detector consumption");
        CHECK(lat::snapshot().dropped_capture == 0, "input submission is not consumption");
        lat::markDetectorConsume(newest);
        CHECK(lat::snapshot().dropped_capture == 1, "overwritten detector input is counted");
        CHECK(first.capture_ns == received && overwritten.capture_ns != received,
              "in-flight frame retains its own timestamp");
        lat::noteDeviceFrameAgeUs(750000);
        lat::noteEngineInferenceMs(3.5);
        lat::reset();
        CHECK(lat::snapshot().device_frame_age_us == -1 &&
              lat::snapshot().engine_inference_ms == -1.0,
              "session reset invalidates device and engine telemetry");
    }


    {
        lat::reset();
        const auto capture = lat::markCapture();
        const auto aim = lat::markAimConsume(capture, lat::nowNs());
        CHECK(lat::snapshot().stages[lat::kEndToEnd].n == 0,
              "no movement cannot reuse a previous command latency");
        lat::markMoveSent(capture, aim, aim + 2'000'000);
        CHECK(lat::snapshot().stages[lat::kAimToMove].last_ms == 2.0,
              "send completion is paired with its own consume timestamp");
        const auto before = lat::snapshot().stages[lat::kEndToEnd].n;
        lat::markMoveSent(0, aim);
        CHECK(lat::snapshot().stages[lat::kEndToEnd].n == before, "unstamped sends are ignored");
        lat::reset();
        lat::markMoveSent(capture, aim);
        CHECK(lat::snapshot().stages[lat::kEndToEnd].n == 0, "old session sends cannot contaminate reset");
    }

    printf("\n=====================================\n");
    if (g_failures) { printf(u8"失败 %d 项\n", g_failures); return 1; }
    printf(u8"结果: 全部通过\n");
    return 0;
}
