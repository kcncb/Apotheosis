#include <array>
#include <string>

#include "imgui/imgui.h"
#include "auth/auth_state.h"

void draw_auth()
{
    static std::array<char, 64> user{};
    static std::array<char, 96> password{};
    static std::array<char, 96> invite{};
    static bool register_mode = false;

    ImGui::TextUnformatted(u8"账号授权");
    ImGui::Separator();

    ImGui::Text(u8"登录状态：%s", auth::state().status_text().c_str());

    if (auth::state().is_authorized())
    {
        if (ImGui::Button(u8"退出登录", ImVec2(150.0f, 0.0f)))
            auth::state().logout();
    }
    else
    {
        ImGui::InputText(u8"账号", user.data(), user.size());
        ImGui::InputText(u8"密码", password.data(), password.size(), ImGuiInputTextFlags_Password);
        if (register_mode)
            ImGui::InputText(u8"邀请码", invite.data(), invite.size());

        if (ImGui::Button(register_mode ? u8"提交注册" : u8"登录", ImVec2(150.0f, 0.0f)))
        {
            if (register_mode)
                auth::state().register_user(user.data(), password.data(), invite.data());
            else
                auth::state().login(user.data(), password.data());
        }

        ImGui::SameLine();
        if (ImGui::Button(register_mode ? u8"返回登录" : u8"注册账号", ImVec2(150.0f, 0.0f)))
            register_mode = !register_mode;
    }

    const std::string error = auth::state().last_error();
    if (!error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", error.c_str());
}
