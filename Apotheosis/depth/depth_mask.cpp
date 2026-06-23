
#include "depth_mask.h"

#include <algorithm>
#include <vector>

#include "depth_anything_trt.h"

namespace depth_anything
{
    void DepthMaskGenerator::reset()
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        mask_binary.release();
        colormap_bgr.release();
        depth_normalized.release();
        last_error.clear();
        last_model_path.clear();
        initialized = false;
        last_update = std::chrono::steady_clock::time_point{};
        last_attempt = std::chrono::steady_clock::time_point{};
        last_frame_w = 0;
        last_frame_h = 0;
        if (model)
        {
            delete model;
            model = nullptr;
        }
    }

    bool DepthMaskGenerator::ready() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return initialized && model && model->ready();
    }

    std::string DepthMaskGenerator::lastError() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return last_error;
    }

    std::chrono::steady_clock::time_point DepthMaskGenerator::lastAttemptTime() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return last_attempt;
    }

    std::pair<int, int> DepthMaskGenerator::lastFrameSize() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return { last_frame_w, last_frame_h };
    }

    DepthMaskDebugState DepthMaskGenerator::debugState() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        DepthMaskDebugState state;
        state.initialized = initialized;
        state.has_model = (model != nullptr);
        state.model_ready = (model != nullptr) ? model->ready() : false;
        state.last_model_path = last_model_path;
        return state;
    }

    cv::Mat DepthMaskGenerator::getMask() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return mask_binary.clone();
    }

    cv::Mat DepthMaskGenerator::getColormap() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return colormap_bgr.clone();
    }

    bool DepthMaskGenerator::hasColormap() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return !colormap_bgr.empty();
    }

    cv::Mat DepthMaskGenerator::getDepthNormalized() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return depth_normalized.clone();
    }

    int DepthMaskGenerator::colormapProducedCount() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return colormap_produced_count;
    }

    int DepthMaskGenerator::updateEnteredCount() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return update_entered_count;
    }

    int DepthMaskGenerator::depthSucceededCount() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return depth_succeeded_count;
    }

    void DepthMaskGenerator::update(const cv::Mat& frame, const DepthMaskOptions& options,
        const std::string& modelPath, nvinfer1::ILogger& logger)
    {
        if (!options.enabled && !options.produce_colormap && !options.produce_normalized)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (frame.empty())
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            last_error = "Depth mask frame is empty.";
            last_attempt = now;
            last_frame_w = 0;
            last_frame_h = 0;
            return;
        }

        std::lock_guard<std::mutex> lk(state_mutex);
        last_attempt = now;
        last_frame_w = frame.cols;
        last_frame_h = frame.rows;
        ++update_entered_count;

        if (!model)
            model = new DepthAnythingTrt();

        if (modelPath.empty())
        {
            last_error = "Depth mask model path is empty.";
            return;
        }

        // Push the configured OPT size before init so that if a fresh build
        // is triggered (modelPath is .onnx with no .engine yet) it gets the
        // user's tuned profile instead of the kOptInputSize default.
        model->setOptInputSize(options.opt_input_size);
        // Push percentile-clip settings every frame so the user can tune
        // them live without restarting.
        model->setNormPercentiles(options.norm_low_pct, options.norm_high_pct);

        if (!initialized || modelPath != last_model_path || !model->ready())
        {
            if (!model->initialize(modelPath, logger))
            {
                last_error = model->lastError();
                initialized = false;
                return;
            }
            last_model_path = modelPath;
            initialized = true;
            last_error.clear();
        }

        const int fps = options.fps > 0 ? options.fps : 5;
        const auto interval = std::chrono::milliseconds(1000 / fps);
        if (now - last_update < interval)
            return;

        last_update = now;

        cv::Mat depth_norm = model->predictDepth(frame);
        if (depth_norm.empty())
        {
            last_error = model->lastError();
            if (last_error.empty())
                last_error = "Depth mask inference returned empty output.";
            return;
        }
        ++depth_succeeded_count;
        depth_normalized = depth_norm.clone();

        if (options.produce_colormap)
        {
            // Gamma 曲线只作用于热力图显示,不动 depth_norm 本身(后面的
            // 直方图阈值是百分位的,对单调变换不变,但保持显示和遮罩解耦更
            // 干净)。gamma < 1 把暗端拉亮,gamma > 1 反之。
            const float gamma = std::clamp(options.heatmap_gamma, 0.1f, 5.0f);
            cv::Mat depth_for_color;
            if (std::abs(gamma - 1.0f) < 1e-3f)
            {
                depth_for_color = depth_norm;
            }
            else
            {
                cv::Mat depth_f;
                depth_norm.convertTo(depth_f, CV_32F, 1.0 / 255.0);
                cv::pow(depth_f, gamma, depth_f);
                depth_f.convertTo(depth_for_color, CV_8U, 255.0);
            }
            cv::applyColorMap(depth_for_color, colormap_bgr, options.colormap_type);
            ++colormap_produced_count;
        }

        if (!options.enabled)
            return;

        int near_percent = std::clamp(options.near_percent, 1, 100);

        const int total = depth_norm.rows * depth_norm.cols;
        if (total <= 0)
        {
            return;
        }

        const int target = std::max(1, (total * near_percent) / 100);
        int hist[256] = {};
        for (int y = 0; y < depth_norm.rows; ++y)
        {
            const uint8_t* row = depth_norm.ptr<uint8_t>(y);
            for (int x = 0; x < depth_norm.cols; ++x)
            {
                hist[row[x]]++;
            }
        }

        int threshold = 0;
        if (!options.invert)
        {
            int count = 0;
            for (int i = 0; i < 256; ++i)
            {
                count += hist[i];
                if (count >= target)
                {
                    threshold = i;
                    break;
                }
            }
        }
        else
        {
            int count = 0;
            for (int i = 255; i >= 0; --i)
            {
                count += hist[i];
                if (count >= target)
                {
                    threshold = i;
                    break;
                }
            }
        }

        cv::Mat mask;
        if (!options.invert)
            cv::compare(depth_norm, threshold, mask, cv::CMP_LE);
        else
            cv::compare(depth_norm, threshold, mask, cv::CMP_GE);

        const int expand = std::clamp(options.expand, 0, 128);
        if (expand > 0)
        {
            const int kernelSize = 2 * expand + 1;
            cv::Mat kernel = cv::getStructuringElement(
                cv::MORPH_ELLIPSE, cv::Size(kernelSize, kernelSize));
            cv::dilate(mask, mask, kernel);
        }

        const int nonZero = cv::countNonZero(mask);
        const bool hasPreviousMask = !mask_binary.empty();
        if (total > 0)
        {
            if (hasPreviousMask && near_percent < 100 && nonZero >= total)
            {
                last_error = "Depth mask became full-frame; keeping previous mask.";
                return;
            }
            if (hasPreviousMask && near_percent > 1 && nonZero <= 0)
            {
                last_error = "Depth mask became empty; keeping previous mask.";
                return;
            }
        }

        mask_binary = std::move(mask);
    }

    DepthMaskGenerator& GetDepthMaskGenerator()
    {
        static DepthMaskGenerator generator;
        return generator;
    }
}

