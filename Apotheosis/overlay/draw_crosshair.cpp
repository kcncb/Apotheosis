#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "draw_settings.h"
#include "Apotheosis.h"

namespace
{

struct ColorPreset
{
    const char* label;
    CrosshairColorProfileConfig bandA;
    bool has_secondary;
    CrosshairColorProfileConfig bandB;
};

const ColorPreset& red_preset()
{
    static ColorPreset p = [] {
        ColorPreset x;
        x.label = u8"红色（双区间）";
        x.bandA.name = u8"红色-低H";  x.bandA.h_low = 0;   x.bandA.h_high = 10;
        x.has_secondary = true;
        x.bandB.name = u8"红色-高H"; x.bandB.h_low = 160; x.bandB.h_high = 179;
        return x;
    }();
    return p;
}

const ColorPreset& green_preset()
{
    static ColorPreset p = [] {
        ColorPreset x;
        x.label = u8"绿色";
        x.bandA.name = u8"绿色"; x.bandA.h_low = 40; x.bandA.h_high = 85;
        x.has_secondary = false;
        return x;
    }();
    return p;
}

const ColorPreset& cyan_preset()
{
    static ColorPreset p = [] {
        ColorPreset x;
        x.label = u8"青色";
        x.bandA.name = u8"青色"; x.bandA.h_low = 85; x.bandA.h_high = 100;
        x.has_secondary = false;
        return x;
    }();
    return p;
}

const ColorPreset& purple_preset()
{
    static ColorPreset p = [] {
        ColorPreset x;
        x.label = u8"紫色";
        x.bandA.name = u8"紫色"; x.bandA.h_low = 125; x.bandA.h_high = 155;
        x.has_secondary = false;
        return x;
    }();
    return p;
}

const ColorPreset& yellow_preset()
{
    static ColorPreset p = [] {
        ColorPreset x;
        x.label = u8"黄色";
        x.bandA.name = u8"黄色"; x.bandA.h_low = 20; x.bandA.h_high = 35;
        x.has_secondary = false;
        return x;
    }();
    return p;
}

const std::vector<const ColorPreset*>& presets()
{
    static std::vector<const ColorPreset*> v = {
        &red_preset(), &green_preset(), &cyan_preset(), &purple_preset(), &yellow_preset(),
    };
    return v;
}

bool draw_color_band_row(CrosshairColorProfileConfig& c, bool& want_delete)
{
    bool changed = false;

    changed |= ImGui::Checkbox(u8"启用##color_enabled", &c.enabled);
    ImGui::SameLine();

    char name_buf[64];
    std::snprintf(name_buf, sizeof(name_buf), "%s", c.name.c_str());
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText(u8"名称##color_name", name_buf, sizeof(name_buf)))
    {
        c.name = name_buf;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 60, 60, 255));
    if (ImGui::Button(u8"删除##color_delete"))
        want_delete = true;
    ImGui::PopStyleColor();

    changed |= ImGui::SliderInt(u8"H 最小",  &c.h_low,  0, 179);
    changed |= ImGui::SliderInt(u8"H 最大", &c.h_high, 0, 179);
    changed |= ImGui::SliderInt(u8"S 最小",  &c.s_min,  0, 255);
    changed |= ImGui::SliderInt(u8"S 最大",  &c.s_max,  0, 255);
    changed |= ImGui::SliderInt(u8"V 最小",  &c.v_min,  0, 255);
    changed |= ImGui::SliderInt(u8"V 最大",  &c.v_max,  0, 255);

    return changed;
}

} // namespace

void draw_crosshair()
{
    bool changed = false;

    std::lock_guard<std::recursive_mutex> cfg(configMutex);

    if (OverlayUI::BeginSection(u8"说明", "crosshair_section_intro"))
    {
        ImGui::TextWrapped(u8"准星找色会把画面中心取样区域从 BGR 转为 HSV，并匹配下面配置的颜色区间。");
        ImGui::TextWrapped(u8"每个瞄准热键可单独控制是否启用；本页用于配置取样区域、颜色和面积过滤。 ");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"取样区域", "crosshair_section_rect"))
    {
        changed |= ImGui::SliderInt(u8"宽度（像素）", &config.crosshair_rect_w, 4, 256);
        changed |= ImGui::SliderInt(u8"高度（像素）", &config.crosshair_rect_h, 4, 256);
        ImGui::TextDisabled(u8"矩形会自动居中到检测图像，并在越界时裁剪。 ");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"颜色区间", "crosshair_section_colors"))
    {
        ImGui::TextDisabled(u8"OpenCV HSV 范围：H [0,179]，S/V [0,255]。红色通常需要两个区间：H 0-10 和 H 160-179。 ");

        for (size_t i = 0; i < config.crosshair_colors.size();)
        {
            ImGui::PushID(static_cast<int>(i));
            std::string title = u8"颜色 #" + std::to_string(i + 1);
            if (!config.crosshair_colors[i].name.empty())
                title += " - " + config.crosshair_colors[i].name;

            bool want_delete = false;
            if (OverlayUI::BeginSubsection(title.c_str()))
            {
                if (draw_color_band_row(config.crosshair_colors[i], want_delete))
                    changed = true;
                OverlayUI::EndSubsection();
            }
            ImGui::PopID();

            if (want_delete)
            {
                config.crosshair_colors.erase(config.crosshair_colors.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                continue;
            }
            ++i;
        }

        ImGui::Separator();
        if (ImGui::Button(u8"新增空颜色"))
        {
            CrosshairColorProfileConfig c;
            c.name = u8"颜色 " + std::to_string(config.crosshair_colors.size() + 1);
            config.crosshair_colors.push_back(std::move(c));
            changed = true;
        }
        ImGui::SameLine();

        static int s_preset_pick = 0;
        const auto& ps = presets();
        std::vector<const char*> labels;
        labels.reserve(ps.size());
        for (const auto* p : ps)
            labels.push_back(p->label);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("##crosshair_preset", &s_preset_pick, labels.data(), static_cast<int>(labels.size()));
        ImGui::SameLine();
        if (ImGui::Button(u8"添加预设"))
        {
            const ColorPreset* p = ps[static_cast<size_t>(s_preset_pick)];
            config.crosshair_colors.push_back(p->bandA);
            if (p->has_secondary)
                config.crosshair_colors.push_back(p->bandB);
            changed = true;
        }

        if (config.crosshair_colors.empty())
            ImGui::TextDisabled(u8"当前没有配置颜色，检测会一直失败。 ");

        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"形状容差", "crosshair_section_shape"))
    {
        changed |= ImGui::SliderInt(u8"最少红像素数", &config.crosshair_min_pixel_count, 1, 200);
        ImGui::TextDisabled(u8"ROI 内累计的红色像素低于此阈值时认为没看到准星。 ");

        changed |= ImGui::SliderInt(u8"形状闭合半径（px）", &config.crosshair_close_radius, 0, 7);
        ImGui::TextDisabled(u8"0 = 不闭合（适合纯实心准星）。1~3 适合白心红点 / 渐变红点 / 十字。\n"
                            u8"过大会把附近红色噪点（血量条、击中反馈）粘进准星 blob。");
        OverlayUI::EndSection();
    }

    if (changed)
        OverlayConfig_MarkDirty();
}
