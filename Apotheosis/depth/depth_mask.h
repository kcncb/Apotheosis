#pragma once

#include <opencv2/opencv.hpp>
#include <chrono>
#include <mutex>
#include <string>

namespace nvinfer1
{
    class ILogger;
}

namespace depth_anything
{
    class DepthAnythingTrt;

    struct DepthMaskOptions
    {
        bool enabled = false;
        int fps = 5;
        int near_percent = 20;
        int expand = 0;
        bool invert = false;
        bool produce_colormap = false;
        // 仅需要 normalized depth(用于 bbox 距离标注等),不需要 mask 也
        // 不需要 colormap 时,把这个置 true。
        bool produce_normalized = false;
        int colormap_type = 18;
        int opt_input_size = 224;
        float heatmap_gamma = 1.0f;
        float norm_low_pct = 0.0f;
        float norm_high_pct = 100.0f;
    };

    struct DepthMaskDebugState
    {
        bool initialized = false;
        bool has_model = false;
        bool model_ready = false;
        std::string last_model_path;
    };

    class DepthMaskGenerator
    {
    public:
        void update(const cv::Mat& frame, const DepthMaskOptions& options,
            const std::string& modelPath, nvinfer1::ILogger& logger);
        cv::Mat getMask() const;
        cv::Mat getColormap() const;
        bool hasColormap() const;
        // Normalized depth (CV_8UC1, frame-sized). 255 = closest in frame,
        // 0 = farthest. Per-frame MIN-MAX normalization, so values are
        // RELATIVE not absolute.
        cv::Mat getDepthNormalized() const;
        // Diagnostic counters: number of times update() entered, number of
        // times predictDepth() returned a non-empty depth map, number of
        // times cv::applyColorMap() actually ran. Helps localize where the
        // heatmap pipeline is dropping frames.
        int colormapProducedCount() const;
        int updateEnteredCount() const;
        int depthSucceededCount() const;
        bool ready() const;
        std::string lastError() const;
        std::chrono::steady_clock::time_point lastAttemptTime() const;
        std::pair<int, int> lastFrameSize() const;
        DepthMaskDebugState debugState() const;
        void reset();

    private:
        mutable std::mutex state_mutex;
        cv::Mat mask_binary;
        cv::Mat colormap_bgr;
        cv::Mat depth_normalized;
        int colormap_produced_count = 0;
        int update_entered_count = 0;
        int depth_succeeded_count = 0;
        // Default-constructed (epoch) instead of time_point::min() — the
        // latter overflows when subtracted from a steady_clock::now() in
        // the throttle check, leaving (now - last_update) negative and
        // making "< interval" always true → predictDepth never runs.
        std::chrono::steady_clock::time_point last_update{};
        std::chrono::steady_clock::time_point last_attempt{};
        int last_frame_w = 0;
        int last_frame_h = 0;
        std::string last_model_path;
        std::string last_error;
        bool initialized = false;

        class DepthAnythingTrt* model = nullptr;
    };

    DepthMaskGenerator& GetDepthMaskGenerator();
}

