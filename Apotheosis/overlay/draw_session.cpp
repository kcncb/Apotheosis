#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "imgui/imgui.h"

#include "draw_settings.h"
#include "i_detector.h"
#include "include/other_tools.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "runtime/cuda_availability.h"
#include "runtime/inference_session.h"
#include "scr/file_picker.h"
#include "detector/dml_detector.h"
#include "Apotheosis.h"
#include "auth/auth_state.h"

namespace
{
const char* backend_label_for_current()
{
    if (config.backend == "TRT")
        return u8"TensorRT (CUDA)";
    return u8"DirectML (CPU/GPU)";
}

void preload_current_model_for_ui()
{
    std::string error;
    if (!runtime::preload_model_metadata(std::string("models/") + config.ai_model, true, &error) && !error.empty())
        std::cerr << "[SessionUI] Model metadata preload failed: " << error << std::endl;
}

void draw_start_stop_controls(runtime::InferenceSession* session)
{
    static std::atomic<bool> stopInProgress{ false };

    const bool sessionRunning = session && session->running();
    const auto& cudaStatus = runtime::probe_cuda_runtime();
    const bool trtReady = cudaStatus.trt_ready();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 8.0f));
    if (!sessionRunning)
    {
        if (config.auth_require_online && !auth::state().is_authorized())
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(46, 140, 80, 235));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(58, 170, 98, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(38, 120, 70, 255));
        if (ImGui::Button(u8"启动推理", ImVec2(180.0f, 0.0f)))
        {
            if (session)
            {
                if (!trtReady && config.backend == "TRT")
                {
                    config.backend = "DML";
                    OverlayConfig_MarkDirty();
                }
                session->start(config.backend, std::string("models/") + config.ai_model);
            }
        }
        ImGui::PopStyleColor(3);
        if (config.auth_require_online && !auth::state().is_authorized())
            ImGui::EndDisabled();
    }
    else
    {
        if (stopInProgress.load())
            ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(182, 60, 60, 235));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(210, 76, 76, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(160, 50, 50, 255));
        if (ImGui::Button(stopInProgress.load() ? u8"停止中..." : u8"停止推理", ImVec2(180.0f, 0.0f)))
        {
            if (session && !stopInProgress.exchange(true))
            {
                std::thread([session] {
                    session->stop();
                    stopInProgress.store(false);
                }).detach();
            }
        }
        ImGui::PopStyleColor(3);
        if (stopInProgress.load())
            ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 12.0f);
    if (sessionRunning)
    {
        ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.50f, 1.0f), u8"运行中");
        ImGui::SameLine();
        ImGui::Text(u8"(后端=%s, 模型=%s)",
                    session->current_backend().c_str(),
                    session->current_model_path().c_str());
    }
    else
    {
        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.0f), u8"已停止");
        if (session && !session->last_error().empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), u8"[%s]",
                               session->last_error().c_str());
        }
        if (config.auth_require_online && !auth::state().is_authorized())
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), u8"请先在“账号授权”页登录。");
    }
}

void draw_backend_picker(bool sessionRunning)
{
    const auto& cudaStatus = runtime::probe_cuda_runtime();
    const bool trtReady = cudaStatus.trt_ready();

    const std::vector<std::string> backendOptions{ "TRT", "DML" };
    const char* backendItems[] = {
        trtReady ? u8"TensorRT (CUDA)" : u8"TensorRT (CUDA) - 不可用",
        u8"DirectML (CPU/GPU)",
    };

    int currentIdx = (config.backend == "DML") ? 1 : 0;

    if (sessionRunning) ImGui::BeginDisabled();
    if (ImGui::Combo(u8"推理后端##session_backend", &currentIdx, backendItems, IM_ARRAYSIZE(backendItems)))
    {
        std::string next = backendOptions[currentIdx];
        if (!trtReady && next == "TRT")
        {
            next = "DML";
            currentIdx = 1;
        }
        if (config.backend != next)
        {
            config.backend = next;
            OverlayConfig_MarkDirty();
        }
    }
    if (sessionRunning) ImGui::EndDisabled();

    if (!trtReady)
    {
        ImGui::TextWrapped(u8"TensorRT 不可用：%s",
            cudaStatus.failure_reason.empty() ? u8"未检测到运行时" : cudaStatus.failure_reason.c_str());
    }
    else if (sessionRunning)
    {
        ImGui::TextDisabled(u8"停止后才能切换后端");
    }
}

void draw_model_picker(bool sessionRunning)
{
    std::vector<std::string> availableModels = getAvailableModels();

    if (sessionRunning) ImGui::BeginDisabled();

    if (availableModels.empty())
    {
        ImGui::Text(u8"models/ 目录为空，请使用 [浏览] 按钮导入 .onnx 文件。");
    }
    else
    {
        int currentIdx = 0;
        auto it = std::find(availableModels.begin(), availableModels.end(), config.ai_model);
        if (it != availableModels.end())
            currentIdx = static_cast<int>(std::distance(availableModels.begin(), it));

        std::vector<const char*> modelItems;
        modelItems.reserve(availableModels.size());
        for (const auto& m : availableModels)
            modelItems.push_back(m.c_str());

        if (ImGui::Combo(u8"模型##session_model", &currentIdx, modelItems.data(),
                         static_cast<int>(modelItems.size())))
        {
            if (config.ai_model != availableModels[currentIdx])
            {
                config.ai_model = availableModels[currentIdx];
                preload_current_model_for_ui();
                OverlayConfig_MarkDirty();
            }
        }
    }

    if (ImGui::Button(u8"浏览 ONNX...##session_browse"))
    {
        const std::vector<file_picker::FilterSpec> filters{
            {L"Oliver 加密模型 (*.oliver)", L"*.oliver"},
            {L"ONNX 模型 (*.onnx)", L"*.onnx"},
            {L"已编译引�?(*.engine)", L"*.engine"},
            {L"所有文�?(*.*)", L"*.*"},
        };
        const auto picked = file_picker::open_file(L"选择 ONNX 模型", filters);
        if (picked)
        {
            const auto imported = file_picker::import_onnx_into_models_dir(*picked);
            if (imported)
            {
                config.ai_model = *imported;
                preload_current_model_for_ui();
                OverlayConfig_MarkDirty();
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(u8"文件会导入到 models/，编译引擎缓存在 models/engines/");

    if (sessionRunning) ImGui::EndDisabled();
}

void draw_dml_adapter_picker(bool sessionRunning)
{
    const auto adapters = EnumerateDMLAdapters();
    if (adapters.empty())
    {
        ImGui::TextDisabled(u8"未检测到 DirectML 显卡");
        return;
    }

    int currentIdx = 0;
    for (int i = 0; i < static_cast<int>(adapters.size()); ++i)
    {
        if (adapters[i].device_id == config.dml_device_id)
        {
            currentIdx = i;
            break;
        }
    }

    std::vector<std::string> labels;
    std::vector<const char*> items;
    labels.reserve(adapters.size());
    items.reserve(adapters.size());
    for (const auto& adapter : adapters)
    {
        labels.push_back(std::to_string(adapter.device_id) + ": " + adapter.name);
        items.push_back(labels.back().c_str());
    }

    if (sessionRunning) ImGui::BeginDisabled();
    if (ImGui::Combo(u8"DirectML 显卡##session_dml_adapter", &currentIdx,
                     items.data(), static_cast<int>(items.size())))
    {
        const int nextDeviceId = adapters[currentIdx].device_id;
        if (config.dml_device_id != nextDeviceId)
        {
            config.dml_device_id = nextDeviceId;
            OverlayConfig_MarkDirty();
        }
    }
    if (sessionRunning) ImGui::EndDisabled();
}
} // namespace

void draw_session()
{
    runtime::InferenceSession* session = g_inference_session;

    if (OverlayUI::BeginSection(u8"推理会话", "session_section_control"))
    {
        draw_start_stop_controls(session);
        OverlayUI::EndSection();
    }

    const bool sessionRunning = session && session->running();

    if (OverlayUI::BeginSection(u8"推理后端", "session_section_backend"))
    {
        draw_backend_picker(sessionRunning);
        draw_dml_adapter_picker(sessionRunning);
        ImGui::TextDisabled(u8"当前选择：%s", backend_label_for_current());
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"模型", "session_section_model"))
    {
        draw_model_picker(sessionRunning);
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"检测预览", "session_section_preview"))
    {
        draw_detection_preview();
        OverlayUI::EndSection();
    }
}
