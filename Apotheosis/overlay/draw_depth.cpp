#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <commdlg.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <algorithm>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>

#include "imgui/imgui.h"
#include "Apotheosis.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "capture.h"
#include "draw_settings.h"
#include "include/other_tools.h"
#include "overlay/ui_sections.h"

#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#include "tensorrt/trt_monitor.h"

static const char* kDepthColormapNames[] = {
    "Autumn",
    "Bone",
    "Jet",
    "Winter",
    "Rainbow",
    "Ocean",
    "Summer",
    "Spring",
    "Cool",
    "HSV",
    "Pink",
    "Hot",
    "Parula",
    "Magma",
    "Inferno",
    "Plasma",
    "Viridis",
    "Cividis",
    "Twilight",
    "Twilight Shifted",
    "Turbo",
    "Deepgreen"
};

namespace
{
    bool HasExtensionCaseInsensitive(const std::string& path, const char* ext)
    {
        if (!ext || !*ext)
        {
            return false;
        }

        std::filesystem::path p(path);
        std::string current = p.extension().u8string();
        std::string expected = ext;
        std::transform(current.begin(), current.end(), current.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(expected.begin(), expected.end(), expected.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return current == expected;
    }
}

void draw_depth()
{
    static std::string depthStatus = u8"深度运行时由系统自动管理。";
    static std::atomic<bool> depthExportRunning{ false };
    static std::thread depthExportThread;
    static std::mutex depthExportMutex;
    static std::string depthExportResult;

    if (depthExportThread.joinable() &&
        depthExportThread.get_id() != std::this_thread::get_id() &&
        !depthExportRunning.load())
    {
        depthExportThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(depthExportMutex);
        if (!depthExportResult.empty())
        {
            depthStatus = depthExportResult;
            depthExportResult.clear();
        }
    }

    std::vector<std::string> availableDepthModels = getAvailableDepthModels();
    std::string selectedModel;
    bool hasModels = !availableDepthModels.empty();

    if (OverlayUI::BeginSection(u8"深度推理", "depth_section_inference"))
    {
        if (ImGui::Checkbox(u8"启用深度推理", &config.depth_inference_enabled))
        {
            OverlayConfig_MarkDirty();
            if (!config.depth_inference_enabled)
                depthStatus = u8"深度推理已禁用。";
        }

        if (!hasModels)
        {
            ImGui::Text(u8"models/depth 目录下没有可用的深度模型。");
        }
        else
        {
            int currentModelIndex = 0;
            auto it = std::find(availableDepthModels.begin(), availableDepthModels.end(), config.depth_model_path);
            if (it == availableDepthModels.end())
            {
                std::string configFile = std::filesystem::u8path(config.depth_model_path).filename().u8string();
                it = std::find(availableDepthModels.begin(), availableDepthModels.end(), configFile);
            }
            if (it != availableDepthModels.end())
            {
                currentModelIndex = static_cast<int>(std::distance(availableDepthModels.begin(), it));
            }

            std::vector<const char*> modelItems;
            modelItems.reserve(availableDepthModels.size());
            for (const auto& modelName : availableDepthModels)
            {
                modelItems.push_back(modelName.c_str());
            }

            if (ImGui::Combo(u8"深度模型", &currentModelIndex, modelItems.data(), static_cast<int>(modelItems.size())))
            {
                if (config.depth_model_path != availableDepthModels[currentModelIndex])
                {
                    config.depth_model_path = availableDepthModels[currentModelIndex];
                    OverlayConfig_MarkDirty();
                }
            }

            selectedModel = availableDepthModels[currentModelIndex];
        }

        const bool selectedIsOnnx = hasModels && HasExtensionCaseInsensitive(selectedModel, ".onnx");
        const bool exportBusy = depthExportRunning.load();
        if (!hasModels || selectedIsOnnx || exportBusy)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(u8"加载深度模型"))
        {
            if (config.depth_model_path != selectedModel)
            {
                config.depth_model_path = selectedModel;
                OverlayConfig_MarkDirty();
                depthStatus = u8"已应用深度模型路径，运行时加载器会自动更新。";
            }
            else
            {
                depthStatus = u8"深度模型路径已是当前选择。";
            }
        }
        if (!hasModels || selectedIsOnnx || exportBusy)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        if (!hasModels || !selectedIsOnnx || exportBusy)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(u8"导出深度引擎"))
        {
            if (!depthExportRunning.load())
            {
                if (config.depth_model_path != selectedModel)
                {
                    config.depth_model_path = selectedModel;
                    OverlayConfig_MarkDirty();
                }

                std::string exportPath = selectedModel;
                if (exportPath.empty())
                {
                    depthStatus = u8"请先设置深度 ONNX 路径再导出。";
                }
                else if (!HasExtensionCaseInsensitive(exportPath, ".onnx"))
                {
                    depthStatus = u8"导出需要 .onnx 深度模型路径。";
                }
                else
                {
                    depthExportRunning.store(true);
                    depthExportThread = std::thread([exportPath] {
                        depth_anything::DepthAnythingTrt exporter;
                        std::string result;
                        if (exporter.initialize(exportPath, gLogger))
                        {
                            result = u8"深度引擎已导出到 ONNX 同目录。";
                        }
                        else
                        {
                            if (gTrtExportCancelRequested.load())
                            {
                                result = u8"深度导出已取消。";
                            }
                            else
                            {
                                result = exporter.lastError();
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(depthExportMutex);
                            depthExportResult = result;
                        }
                        depthExportRunning.store(false);
                    });
                }
            }
        }
        if (!hasModels || !selectedIsOnnx || exportBusy)
        {
            ImGui::EndDisabled();
        }

        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"深度运行时", "depth_section_runtime"))
    {
        if (ImGui::SliderInt(u8"深度 FPS", &config.depth_fps, 0, 120))
        {
            OverlayConfig_MarkDirty();
        }

        if (ImGui::SliderInt(u8"深度遮罩 FPS", &config.depth_mask_fps, 1, 30))
        {
            OverlayConfig_MarkDirty();
        }
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"深度遮罩", "depth_section_mask"))
    {
        if (ImGui::Checkbox(u8"启用深度遮罩", &config.depth_mask_enabled))
        {
            OverlayConfig_MarkDirty();
        }

        if (ImGui::SliderInt(u8"深度遮罩近端 %", &config.depth_mask_near_percent, 1, 100))
        {
            OverlayConfig_MarkDirty();
        }

        if (ImGui::SliderInt(u8"深度遮罩扩展 (px)", &config.depth_mask_expand, 0, 128))
        {
            OverlayConfig_MarkDirty();
        }

        if (ImGui::SliderInt(u8"深度遮罩保持帧数", &config.depth_mask_hold_frames, 0, 120))
        {
            OverlayConfig_MarkDirty();
        }

        if (ImGui::SliderFloat(u8"遮罩抑制占比阈值", &config.depth_mask_suppression_ratio, 0.0f, 1.0f, "%.2f"))
        {
            OverlayConfig_MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(u8"bbox 内落在遮罩上的像素比例超过这个值才丢弃该检测。0 表示任意 1 个像素命中即丢弃；0.30 是较稳妥默认；1.0 表示永不丢弃。");
        }

        if (ImGui::SliderInt(u8"深度遮罩透明度", &config.depth_mask_alpha, 0, 255))
        {
            OverlayConfig_MarkDirty();
        }
        if (config.depth_mask_enabled && config.depth_mask_alpha == 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), u8"深度遮罩不可见：透明度为 0。");
        }

        if (ImGui::Checkbox(u8"反转深度遮罩", &config.depth_mask_invert))
        {
            OverlayConfig_MarkDirty();
        }

        int colormapIndex = config.depth_colormap;
        if (ImGui::Combo(u8"深度色图", &colormapIndex, kDepthColormapNames, IM_ARRAYSIZE(kDepthColormapNames)))
        {
            config.depth_colormap = colormapIndex;
            OverlayConfig_MarkDirty();
        }

        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"深度状态", "depth_section_status"))
    {
        ImGui::Text(u8"状态：%s", depthStatus.c_str());

        if (config.depth_inference_enabled && config.depth_mask_enabled)
        {
            auto& depthMask = depth_anything::GetDepthMaskGenerator();
            const auto state = depthMask.debugState();
            const auto lastErr = depthMask.lastError();
            const auto frameSize = depthMask.lastFrameSize();

            ImGui::Separator();
            ImGui::Text(u8"遮罩运行时：%s", state.model_ready ? u8"就绪" : u8"未就绪");
            ImGui::Text(u8"遮罩模型路径：%s",
                state.last_model_path.empty() ? u8"(无)" : state.last_model_path.c_str());
            if (frameSize.first > 0 && frameSize.second > 0)
                ImGui::Text(u8"最新遮罩帧：%dx%d", frameSize.first, frameSize.second);

            if (!lastErr.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), u8"遮罩错误：%s", lastErr.c_str());
        }
        else if (config.depth_inference_enabled)
        {
            ImGui::Separator();
            ImGui::TextUnformatted(u8"深度遮罩已禁用。");
        }
        else
        {
            ImGui::Separator();
            ImGui::TextUnformatted(u8"深度推理已禁用。");
        }

        OverlayUI::EndSection();
    }
}


