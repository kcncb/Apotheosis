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
