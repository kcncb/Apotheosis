#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>

#include "imgui/imgui.h"
#include "Apotheosis.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

namespace
{
float OverlayCompactControlWidth()
{
    return OverlayUI::AdaptiveItemWidth(0.66f);
}
}

void draw_overlay()
{
    constexpr int kMinReadableOpacity = 220;

    if (OverlayUI::BeginSection(u8"外观", "overlay_section_visual"))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"窗口不透明度");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(OverlayCompactControlWidth());
        int prev_opacity = config.overlay_opacity;
        if (ImGui::SliderInt("##overlay_opacity_slider", &config.overlay_opacity, kMinReadableOpacity, 255))
        {
            if (config.overlay_opacity < kMinReadableOpacity) config.overlay_opacity = kMinReadableOpacity;
            if (config.overlay_opacity > 255) config.overlay_opacity = 255;

            Overlay_SetOpacity(config.overlay_opacity);

            if (config.overlay_opacity != prev_opacity)
                OverlayConfig_MarkDirty();
        }

        float ui_scale = std::clamp(config.overlay_ui_scale, 0.85f, 1.35f);
        if (ui_scale != config.overlay_ui_scale)
            config.overlay_ui_scale = ui_scale;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"界面缩放");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(OverlayCompactControlWidth());
        if (ImGui::SliderFloat("##overlay_ui_scale_slider", &ui_scale, 0.85f, 1.35f, "%.2f"))
        {
            config.overlay_ui_scale = ui_scale;
            OverlayConfig_MarkDirty();
        }

        OverlayUI::EndSection();
    }
}
