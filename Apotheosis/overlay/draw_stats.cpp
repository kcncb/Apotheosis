#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <string>

#include "imgui/imgui.h"
#include "Apotheosis.h"
#include "overlay.h"
#include "capture.h"
#include "draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

void draw_stats()
{
    static float preprocess_times[120] = {};
    static float inference_times[120] = {};
    static float copy_times[120] = {};
    static float postprocess_times[120] = {};
    static float nms_times[120] = {};
    static int index_inf = 0;

    static float capture_fps_vals[120] = {};
    static int index_fps = 0;

    static float avg_preprocess_cached = 0.0f;
    static float avg_inference_cached = 0.0f;
    static float avg_copy_cached = 0.0f;
    static float avg_post_cached = 0.0f;
    static float avg_nms_cached = 0.0f;
    static float avg_fps_cached = 0.0f;
    static double last_avg_update_time = 0.0;

    float current_preprocess = 0.0f;
    float current_inference = 0.0f;
    float current_copy = 0.0f;
    float current_post = 0.0f;
    float current_nms = 0.0f;

    if (g_detector)
    {
        current_preprocess = static_cast<float>(g_detector->lastPreprocessTime().count());
        current_inference = static_cast<float>(g_detector->lastInferenceTime().count());
        current_copy = static_cast<float>(g_detector->lastCopyTime().count());
        current_post = static_cast<float>(g_detector->lastPostprocessTime().count());
        current_nms = static_cast<float>(g_detector->lastNmsTime().count());
    }

    preprocess_times[index_inf] = current_preprocess;
    inference_times[index_inf] = current_inference;
    copy_times[index_inf] = current_copy;
    postprocess_times[index_inf] = current_post;
    nms_times[index_inf] = current_nms;
    index_inf = (index_inf + 1) % IM_ARRAYSIZE(inference_times);

    float current_fps = static_cast<float>(captureFps.load());
    capture_fps_vals[index_fps] = current_fps;
    index_fps = (index_fps + 1) % IM_ARRAYSIZE(capture_fps_vals);

    auto avg = [](const float* arr, int n) -> float {
        float sum = 0.0f; int cnt = 0;
        for (int i = 0; i < n; ++i)
            if (arr[i] > 0.0f) { sum += arr[i]; ++cnt; }
        return cnt ? (sum / cnt) : 0.0f;
        };

    const double now = ImGui::GetTime();
    if (last_avg_update_time == 0.0 || (now - last_avg_update_time) >= 1.0)
    {
        avg_preprocess_cached = avg(preprocess_times, IM_ARRAYSIZE(preprocess_times));
        avg_inference_cached = avg(inference_times, IM_ARRAYSIZE(inference_times));
        avg_copy_cached = avg(copy_times, IM_ARRAYSIZE(copy_times));
        avg_post_cached = avg(postprocess_times, IM_ARRAYSIZE(postprocess_times));
        avg_nms_cached = avg(nms_times, IM_ARRAYSIZE(nms_times));
        avg_fps_cached = avg(capture_fps_vals, IM_ARRAYSIZE(capture_fps_vals));

        last_avg_update_time = now;
    }

    if (OverlayUI::BeginSection(u8"耗时分解", "stats_section_time_breakdown"))
    {
        ImGui::PlotLines(u8"预处理", preprocess_times, IM_ARRAYSIZE(preprocess_times), index_inf, nullptr, 0.0f, 20.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text(u8"%.2f | 平均: %.2f", current_preprocess, avg_preprocess_cached);

        ImGui::PlotLines(u8"推理", inference_times, IM_ARRAYSIZE(inference_times), index_inf, nullptr, 0.0f, 20.0f, ImVec2(0, 40));
        ImGui::SameLine();

        ImGui::Text(u8"%.2f | 平均:", current_inference);
        ImGui::SameLine();

        const bool inf_slow = (avg_inference_cached > 20.0f);
        if (inf_slow)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));

        ImGui::Text("%.2f", avg_inference_cached);

        if (inf_slow)
            ImGui::PopStyleColor();

        ImGui::PlotLines(u8"拷贝", copy_times, IM_ARRAYSIZE(copy_times), index_inf, nullptr, 0.0f, 10.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text(u8"%.2f | 平均: %.2f", current_copy, avg_copy_cached);

        ImGui::PlotLines(u8"后处理", postprocess_times, IM_ARRAYSIZE(postprocess_times), index_inf, nullptr, 0.0f, 10.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text(u8"%.2f | 平均: %.2f", current_post, avg_post_cached);

        ImGui::PlotLines("NMS", nms_times, IM_ARRAYSIZE(nms_times), index_inf, nullptr, 0.0f, 5.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text(u8"%.2f | 平均: %.2f", current_nms, avg_nms_cached);

        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"采集 FPS", "stats_section_capture_fps"))
    {
        ImGui::PlotLines("##fps_plot", capture_fps_vals, IM_ARRAYSIZE(capture_fps_vals), index_fps, nullptr, 0.0f, 144.0f, ImVec2(0, 60));
        ImGui::SameLine();
        ImGui::Text(u8"当前: %.1f | 平均: %.1f", current_fps, avg_fps_cached);
        OverlayUI::EndSection();
    }

    int latestWidth = 0;
    int latestHeight = 0;
    size_t queueDepth = 0;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
        {
            latestWidth = latestFrame.cols;
            latestHeight = latestFrame.rows;
        }
        queueDepth = frameQueue.size();
    }

    const int captureFpsLimit = std::max(0, config.capture_fps);
    const float currentFrameTimeMs = (current_fps > 0.01f) ? (1000.0f / current_fps) : 0.0f;
    const float avgFrameTimeMs = (avg_fps_cached > 0.01f) ? (1000.0f / avg_fps_cached) : 0.0f;

    std::string captureSource = u8"未知";
    if (config.capture_method == "udp_capture")
    {
        captureSource = "UDP " + config.udp_ip + ":" + std::to_string(config.udp_port);
    }
    else if (config.capture_method == "tcp_capture")
    {
        captureSource = "TCP " + config.tcp_ip + ":" + std::to_string(config.tcp_port);
    }
    else if (config.capture_method == "opencv_capture")
    {
        if (!config.opencv_capture_url.empty())
            captureSource = "OpenCV " + config.opencv_capture_api + " " + config.opencv_capture_url;
        else
            captureSource = "OpenCV " + config.opencv_capture_api + " #" +
                            std::to_string(config.opencv_capture_index);
    }

    if (OverlayUI::BeginSection(u8"采集详情", "stats_section_capture_details"))
    {
        ImGui::Text(u8"采集方式: %s", config.capture_method.c_str());
        ImGui::Text(u8"后端: %s", config.backend.c_str());
        ImGui::TextWrapped(u8"数据源: %s", captureSource.c_str());

        if (screenWidth > 0 && screenHeight > 0)
            ImGui::Text(u8"桌面尺寸: %dx%d", screenWidth, screenHeight);
        else
            ImGui::TextDisabled(u8"桌面尺寸: 未知");

        if (latestWidth > 0 && latestHeight > 0)
            ImGui::Text(u8"最新帧: %dx%d", latestWidth, latestHeight);
        else
            ImGui::TextDisabled(u8"最新帧: 未知");

        ImGui::Text(u8"检测分辨率: %d", config.detection_resolution);
        if (captureFpsLimit > 0)
            ImGui::Text(u8"采集 FPS 上限: %d", captureFpsLimit);
        else
            ImGui::Text(u8"采集 FPS 上限: 无限制");

        if (currentFrameTimeMs > 0.0f || avgFrameTimeMs > 0.0f)
            ImGui::Text(u8"帧时间: 当前 %.2f ms | 平均 %.2f ms", currentFrameTimeMs, avgFrameTimeMs);
        else
            ImGui::TextDisabled(u8"帧时间: 未知");

        ImGui::Text(u8"帧队列深度: %d", static_cast<int>(queueDepth));
        ImGui::Text(u8"圆形遮罩: %s", config.circle_mask ? u8"开" : u8"关");

        if (config.backend == "TRT")
        {
            const bool depthMaskEnabled = config.depth_inference_enabled && config.depth_mask_enabled;
            ImGui::Separator();
            ImGui::Text(u8"深度遮罩: %s", depthMaskEnabled ? u8"开" : u8"关");
        }

        OverlayUI::EndSection();
    }
}
