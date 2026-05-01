#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "draw_settings.h"
#include "Apotheosis.h"
#include "runtime/active_hotkey.h"

namespace
{

const char* bucket_label(ClassBucket b)
{
    switch (b)
    {
    case ClassBucket::Delete: return u8"删除";
    case ClassBucket::Filter: return u8"过滤";
    case ClassBucket::Aim:    return u8"瞄准";
    }
    return u8"删除";
}

ImU32 bucket_color(ClassBucket b)
{
    switch (b)
    {
    case ClassBucket::Delete: return IM_COL32(210, 90, 90, 200);
    case ClassBucket::Filter: return IM_COL32(210, 190, 90, 210);
    case ClassBucket::Aim:    return IM_COL32(90, 200, 120, 220);
    }
    return IM_COL32(210, 90, 90, 200);
}

bool radio_bucket(const char* id, ClassBucket& bucket, ClassBucket value)
{
    bool changed = false;
    const bool selected = (bucket == value);
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, bucket_color(value));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bucket_color(value));
    }
    if (ImGui::Button(id))
    {
        if (!selected)
        {
            bucket = value;
            changed = true;
        }
    }
    if (selected)
        ImGui::PopStyleColor(2);
    return changed;
}

} // namespace

void draw_target()
{
    detector::ModelMetadata metadata;
    {
        std::lock_guard<std::mutex> lock(runtime::g_model_metadata_mutex);
        metadata = runtime::g_model_metadata;
    }

    int classCount = metadata.class_count;
    {
        std::lock_guard<std::recursive_mutex> cfg(configMutex);
        if (classCount <= 0 && !config.class_filters.empty())
            classCount = static_cast<int>(config.class_filters.size());
    }

    if (OverlayUI::BeginSection(u8"流程说明", "target_section_pipeline"))
    {
        ImGui::TextWrapped(u8"YOLO 输出会按顺序流经：删除 -> 过滤 -> 瞄准。");
        ImGui::TextWrapped(u8"删除：该类别的检测框会直接丢弃，不再进入任何后续处理。");
        ImGui::TextWrapped(u8"过滤：保留检测框（调试视图与深度遮罩可见），但不会作为瞄准候选。");
        ImGui::TextWrapped(u8"瞄准：作为瞄准候选；具体优先级由“瞄准热键”面板的类别顺序决定。");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"目标类别", "target_section_classes"))
    {
        if (classCount <= 0)
        {
            ImGui::TextWrapped(u8"尚未加载模型。启动推理会话后，类别列表会自动填充。");
        }
        else
        {
            ImGui::Text(u8"模型类别数量：%d", classCount);
            ImGui::TextDisabled(u8"提示：新类别默认置于“删除”。");
            ImGui::Separator();

            if (ImGui::BeginTable("##class_filter_table", 5,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
            {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn(u8"类别", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(u8"删除", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn(u8"过滤", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn(u8"瞄准", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();

                bool changed = false;
                std::lock_guard<std::recursive_mutex> cfg(configMutex);
                for (auto& cf : config.class_filters)
                {
                    ImGui::PushID(cf.class_id);
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", cf.class_id);

                    ImGui::TableNextColumn();
                    const std::string name = cf.class_name.empty()
                        ? ("class_" + std::to_string(cf.class_id))
                        : cf.class_name;
                    ImGui::TextUnformatted(name.c_str());

                    ImGui::TableNextColumn();
                    changed |= radio_bucket(u8"删除##del", cf.bucket, ClassBucket::Delete);

                    ImGui::TableNextColumn();
                    changed |= radio_bucket(u8"过滤##flt", cf.bucket, ClassBucket::Filter);

                    ImGui::TableNextColumn();
                    changed |= radio_bucket(u8"瞄准##aim", cf.bucket, ClassBucket::Aim);

                    ImGui::PopID();
                }
                ImGui::EndTable();

                if (changed)
                    OverlayConfig_MarkDirty();
            }
        }

        OverlayUI::EndSection();
    }
}
