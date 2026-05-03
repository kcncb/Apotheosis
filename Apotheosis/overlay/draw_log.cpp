#include "imgui/imgui.h"

#include "overlay/app_log.h"
#include "overlay/ui_sections.h"

void draw_log()
{
    static bool autoScroll = true;

    if (OverlayUI::BeginSection(u8"运行日志", "log_section_runtime"))
    {
        if (ImGui::Button(u8"清空日志"))
            AppLog::Clear();

        ImGui::SameLine();
        if (ImGui::Button(u8"复制全部"))
        {
            // Snapshot once and concatenate so the user gets exactly what
            // they see in the panel — including any [错误]-prefixed lines.
            // ImGui's clipboard handler (imgui_impl_win32) routes to the
            // Win32 system clipboard, so a paste in another app works.
            const std::vector<std::string> snap = AppLog::Snapshot();
            std::string blob;
            size_t total = 0;
            for (const std::string& l : snap) total += l.size() + 1;
            blob.reserve(total);
            for (const std::string& l : snap)
            {
                blob.append(l);
                blob.push_back('\n');
            }
            ImGui::SetClipboardText(blob.c_str());
        }

        ImGui::SameLine();
        ImGui::Checkbox(u8"自动滚动", &autoScroll);

        ImGui::Separator();

        const float logHeight = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##runtime_log_lines", ImVec2(0.0f, logHeight), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        const std::vector<std::string> lines = AppLog::Snapshot();
        if (lines.empty())
        {
            ImGui::TextDisabled(u8"暂无日志");
        }
        else
        {
            for (const std::string& line : lines)
            {
                if (line.rfind(u8"[错误]", 0) == 0)
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 110, 255));

                ImGui::TextUnformatted(line.c_str());

                if (line.rfind(u8"[错误]", 0) == 0)
                    ImGui::PopStyleColor();
            }

            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f)
                ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
        OverlayUI::EndSection();
    }
}
