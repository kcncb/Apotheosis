
#include "depth_mask.h"

#include <algorithm>
#include <vector>

#include "depth_anything_trt.h"

namespace depth_anything
{
    void DepthMaskGenerator::reset()
    {
        std::lock_guard<std::mutex> lk(state_mutex);
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

    cv::Mat DepthMaskGenerator::getDepthNormalized() const
    {
        std::lock_guard<std::mutex> lk(state_mutex);
        return depth_normalized.clone();
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
        if (!options.produce_normalized)
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
    }

    DepthMaskGenerator& GetDepthMaskGenerator()
    {
        static DepthMaskGenerator generator;
        return generator;
    }
}

