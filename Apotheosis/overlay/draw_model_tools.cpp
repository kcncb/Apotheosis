#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <array>
#include <filesystem>
#include <string>

#include "imgui/imgui.h"
#include "auth/auth_state.h"
#include "config.h"
#include "model_crypto/model_crypto.h"
#include "overlay/config_dirty.h"
#include "scr/file_picker.h"

extern Config config;

namespace
{
std::string default_oliver_output(const std::string& input_path)
{
    std::filesystem::path p(std::filesystem::u8path(input_path));
    p.replace_extension(".oliver");
    return p.u8string();
}
} // namespace

void draw_model_tools()
{
    static std::string selected_input;
    static std::string output_path;
    static std::string last_model_id;
    static std::array<char, 64> grant_user{};
    static std::string status;

    ImGui::TextUnformatted(u8"模型加密");
    ImGui::Separator();

    if (!auth::state().is_authorized())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), u8"请先登录。");
    }
    else
    {
        if (ImGui::Button(u8"选择模型", ImVec2(150.0f, 0.0f)))
        {
            const std::vector<file_picker::FilterSpec> filters{
                {L"模型文件 (*.onnx;*.engine)", L"*.onnx;*.engine"},
                {L"ONNX 模型 (*.onnx)", L"*.onnx"},
                {L"TensorRT 引擎 (*.engine)", L"*.engine"},
                {L"所有文件 (*.*)", L"*.*"},
            };
            const auto picked = file_picker::open_file(L"选择需要加密的模型", filters);
            if (picked)
            {
                selected_input = *picked;
                output_path = default_oliver_output(selected_input);
                status.clear();
            }
        }

        if (!selected_input.empty())
            ImGui::TextWrapped(u8"输入：%s", selected_input.c_str());
        if (!output_path.empty())
            ImGui::TextWrapped(u8"输出：%s", output_path.c_str());

        if (selected_input.empty())
            ImGui::BeginDisabled();
        if (ImGui::Button(u8"加密并登记模型", ImVec2(180.0f, 0.0f)))
        {
            std::filesystem::path input_path(std::filesystem::u8path(selected_input));
            std::string model_id;
            std::vector<uint8_t> key;
            if (!auth::state().create_model_key(input_path.filename().u8string(), model_id, key))
            {
                status = auth::state().last_error();
            }
            else
            {
                oliver::set_runtime_key(key);
                std::string error;
                const oliver::PayloadType type = oliver::payload_type_from_extension(selected_input);
                if (oliver::encrypt_file(selected_input, output_path, type, model_id, error))
                {
                    last_model_id = model_id;
                    status = u8"加密完成，模型编号：" + model_id;
                }
                else
                {
                    status = error;
                }
                oliver::clear_runtime_key();
            }
        }
        if (selected_input.empty())
            ImGui::EndDisabled();
    }

    if (!status.empty())
        ImGui::TextWrapped("%s", status.c_str());

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextUnformatted(u8"模型授权");
    ImGui::Separator();

    std::string current_model_id;
    std::string id_error;
    if (oliver::is_oliver_path(std::string("models/") + config.ai_model))
        oliver::read_model_id_from_file(std::string("models/") + config.ai_model, current_model_id, id_error);
    if (current_model_id.empty())
        current_model_id = last_model_id;

    if (current_model_id.empty())
    {
        ImGui::TextDisabled(u8"请选择或加密一个 .oliver 模型后再授权。");
    }
    else
    {
        ImGui::Text(u8"模型编号：%s", current_model_id.c_str());
        ImGui::InputText(u8"授权账号", grant_user.data(), grant_user.size());
        if (!auth::state().is_authorized() || grant_user[0] == '\0')
            ImGui::BeginDisabled();
        if (ImGui::Button(u8"授权给该账号", ImVec2(180.0f, 0.0f)))
        {
            if (auth::state().grant_model(current_model_id, grant_user.data()))
                status = u8"模型授权成功。";
            else
                status = auth::state().last_error();
        }
        if (!auth::state().is_authorized() || grant_user[0] == '\0')
            ImGui::EndDisabled();
    }
}
