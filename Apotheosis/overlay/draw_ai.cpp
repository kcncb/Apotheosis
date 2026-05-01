#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "imgui/imgui.h"

#include "Apotheosis.h"
#include "include/other_tools.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "draw_settings.h"
#include "overlay/ui_sections.h"
#include "runtime/cuda_availability.h"
#include "scr/file_picker.h"
#include "trt_monitor.h"

std::string prev_backend = config.backend;
float prev_confidence_threshold = config.confidence_threshold;
float prev_nms_threshold = config.nms_threshold;
int prev_max_detections = config.max_detections;

static bool wasExporting = false;
static bool ai_state_initialized = false;

void draw_ai()
{
    if (!ai_state_initialized)
    {
        prev_backend = config.backend;
        prev_confidence_threshold = config.confidence_threshold;
        prev_nms_threshold = config.nms_threshold;
        prev_max_detections = config.max_detections;
        ai_state_initialized = true;
    }

    if (gIsTrtExporting)
    {
        ImGui::OpenPopup(u8"TensorRT 导出进度");
    }

    if (ImGui::BeginPopupModal(u8"TensorRT 导出进度", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        bool hasPhases = false;
        {
            std::lock_guard<std::mutex> lock(gProgressMutex);
            hasPhases = !gProgressPhases.empty();
            if (hasPhases)
            {
                for (auto& [name, phase] : gProgressPhases)
                {
                    float percent = phase.max > 0 ? phase.current / float(phase.max) : 0.0f;
                    ImGui::Text("%s: %d/%d", name.c_str(), phase.current, phase.max);
                    ImGui::ProgressBar(percent, ImVec2(300, 0));
                }
            }
        }
        if (!hasPhases)
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::Text(u8"引擎导出中，请稍候...");
        long long lastUpdate = gTrtExportLastUpdateMs.load();
        if (lastUpdate > 0)
        {
            double secondsSince = (TrtNowMs() - lastUpdate) / 1000.0;
            ImGui::Text(u8"上次进度更新: %.1f 秒前", secondsSince);
        }
        bool cancelRequested = gTrtExportCancelRequested.load();
        if (cancelRequested)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(u8"取消导出"))
        {
            gTrtExportCancelRequested = true;
        }
        if (cancelRequested)
        {
            ImGui::EndDisabled();
            ImGui::Text(u8"已请求取消...");
        }
        ImGui::EndPopup();
    }
    std::vector<std::string> availableModels = getAvailableModels();
    if (OverlayUI::BeginSection(u8"模型", "ai_section_model"))
    {
        if (availableModels.empty())
        {
            ImGui::Text(u8"models/ 目录下没有模型文件。");
        }
        else
        {
            int currentModelIndex = 0;
            auto it = std::find(availableModels.begin(), availableModels.end(), config.ai_model);

            if (it != availableModels.end())
            {
                currentModelIndex = static_cast<int>(std::distance(availableModels.begin(), it));
            }

            std::vector<const char*> modelsItems;
            modelsItems.reserve(availableModels.size());

            for (const auto& modelName : availableModels)
            {
                modelsItems.push_back(modelName.c_str());
            }

            if (ImGui::Combo(u8"模型文件", &currentModelIndex, modelsItems.data(), static_cast<int>(modelsItems.size())))
            {
                if (config.ai_model != availableModels[currentModelIndex])
                {
                    config.ai_model = availableModels[currentModelIndex];
                    OverlayConfig_MarkDirty();
                    detector_model_changed.store(true);
                }
            }
            ImGui::SameLine();
            ImGui::Text(u8"固定输入尺寸：%s", config.fixed_input_size ? u8"开启" : u8"关闭");
        }

        if (ImGui::Button(u8"浏览 ONNX..."))
        {
            const std::vector<file_picker::FilterSpec> filters{
                {L"Oliver 加密模型 (*.oliver)", L"*.oliver"},
                {L"ONNX 模型 (*.onnx)", L"*.onnx"},
                {L"已编译引擎 (*.engine)", L"*.engine"},
                {L"所有文件 (*.*)", L"*.*"},
            };
            const auto picked = file_picker::open_file(L"选择 ONNX 模型", filters);
            if (picked)
            {
                const auto imported = file_picker::import_onnx_into_models_dir(*picked);
                if (imported)
                {
                    config.ai_model = *imported;
                    OverlayConfig_MarkDirty();
                    detector_model_changed.store(true);
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(u8"(导入到 models/)");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"推理后端", "ai_section_backend"))
    {
        const auto& cudaStatus = runtime::probe_cuda_runtime();
        const bool trtReady = cudaStatus.trt_ready();

        std::vector<std::string> backendOptions = { "TRT", "DML" };
        std::vector<const char*> backendItems = {
            trtReady ? u8"TensorRT (CUDA)" : u8"TensorRT (CUDA) - 不可用",
            u8"DirectML (CPU/GPU)"
        };

        int currentBackendIndex = config.backend == "DML" ? 1 : 0;

        if (ImGui::Combo(u8"后端", &currentBackendIndex, backendItems.data(), static_cast<int>(backendItems.size())))
        {
            std::string newBackend = backendOptions[currentBackendIndex];
            if (!trtReady && newBackend == "TRT")
            {
                newBackend = "DML";
                currentBackendIndex = 1;
            }
            if (config.backend != newBackend)
            {
                config.backend = newBackend;
                OverlayConfig_MarkDirty();
                detector_model_changed.store(true);
            }
        }

        if (!trtReady)
        {
            ImGui::TextWrapped(u8"TensorRT 后端已禁用：%s",
                cudaStatus.failure_reason.empty() ? u8"未检测到运行时" : cudaStatus.failure_reason.c_str());
        }
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"检测参数", "ai_section_detection"))
    {
        ImGui::SliderFloat(u8"置信度阈值", &config.confidence_threshold, 0.01f, 1.00f, "%.2f");
        ImGui::SliderFloat(u8"NMS 阈值", &config.nms_threshold, 0.00f, 1.00f, "%.2f");
        ImGui::SliderInt(u8"最大检测数", &config.max_detections, 1, 100);
        OverlayUI::EndSection();
    }

    draw_depth();
        
    if (prev_confidence_threshold != config.confidence_threshold ||
        prev_nms_threshold != config.nms_threshold ||
        prev_max_detections != config.max_detections)
    {
        prev_nms_threshold = config.nms_threshold;
        prev_confidence_threshold = config.confidence_threshold;
        prev_max_detections = config.max_detections;
        OverlayConfig_MarkDirty();
    }

    if (prev_backend != config.backend)
    {
        prev_backend = config.backend;
        detector_model_changed.store(true);
        OverlayConfig_MarkDirty();
    }
}
