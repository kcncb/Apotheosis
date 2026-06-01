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

using OverlayUI::Tooltip;

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
    Tooltip(u8"勾上才参与匹配。临时关掉某个颜色不必删除条目。");
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
    Tooltip(u8"色相最低值(OpenCV 范围 0-179)。红色因色相在 0/180 处环绕,通常需要两条:0-10 与 160-179。");
    changed |= ImGui::SliderInt(u8"H 最大", &c.h_high, 0, 179);
    Tooltip(u8"色相最高值,与最小值组成色相通过区间。");
    changed |= ImGui::SliderInt(u8"S 最小",  &c.s_min,  0, 255);
    Tooltip(u8"饱和度最低值。值高 = 只接受很'纯'的颜色,排除灰白;低 = 也接受偏淡的颜色。");
    changed |= ImGui::SliderInt(u8"S 最大",  &c.s_max,  0, 255);
    Tooltip(u8"饱和度最高值,一般保持 255。除非要排除超饱和荧光色。");
    changed |= ImGui::SliderInt(u8"V 最小",  &c.v_min,  0, 255);
    Tooltip(u8"亮度最低值。值高 = 排除偏暗的同色像素(防误识别阴影中的红色);低 = 暗色也认。");
    changed |= ImGui::SliderInt(u8"V 最大",  &c.v_max,  0, 255);
    Tooltip(u8"亮度最高值,一般 255。除非要排除高光像素。");

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
        Tooltip(u8"画面中心取样矩形的宽度(检测像素)。值小=只看准星正中,精准但镜抬高时可能漏;\n"
                u8"值大=覆盖更广区域,容忍准星偏移但可能误识别其它红色像素。");
        changed |= ImGui::SliderInt(u8"高度（像素）", &config.crosshair_rect_h, 4, 256);
        Tooltip(u8"取样矩形的高度(检测像素)。和宽度独立调,适合椭圆形/长方形取样。");
        ImGui::TextDisabled(u8"矩形居中偏上(贴合抬枪后的准星),越界自动裁剪。镭射取样在左侧「镭射找色」页单独配置。 ");
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
        Tooltip(u8"ROI 内累计的目标颜色像素低于此阈值时认为'没看到准星'。\n"
                u8"值越大越严格,误识少但小准星可能漏检;值越小越宽松。");

        changed |= ImGui::SliderInt(u8"形状闭合半径（px）", &config.crosshair_close_radius, 0, 7);
        Tooltip(u8"形态学闭运算半径(像素),用于把镂空准星(白心红点 / 十字 / 渐变)的小缝合并成一个 blob。\n"
                u8"0 = 不闭合(适合实心准星)。1-3 通用。过大会把血量条/击中反馈等附近红色粘进准星里。");

        changed |= ImGui::SliderFloat(u8"抗抖动平滑", &config.crosshair_smooth, 0.0f, 1.0f, "%.2f");
        Tooltip(u8"对准星点做自适应低通(One-Euro):静止/微抖时强平滑消抖,快速移动时几乎不平滑、\n"
                u8"不增加延迟,所以不会拖慢跟枪/压枪。0=关(原始);抖就往上调,觉得发黏/迟钝就调小。常用 0.3~0.7。");
        OverlayUI::EndSection();
    }

    if (changed)
        OverlayConfig_MarkDirty();
}

// ====================== 镭射找色(独立左侧模块)======================
void draw_laser()
{
    bool changed = false;

    std::lock_guard<std::recursive_mutex> cfg(configMutex);

    if (OverlayUI::BeginSection(u8"说明", "laser_section_intro"))
    {
        ImGui::TextWrapped(u8"镭射找色与准星找色相互独立、可同时开启;两者都命中时'准星找色'优先,"
                           u8"镭射末端仅作为准星没找到时的补充。每个热键的开关在'瞄准热键'页。");
        ImGui::TextWrapped(u8"原理:把镭射当作一条线,拟合主轴取'瞄准端'(靠中心那端)作命中点,可虚拟外推补齐发淡末端。"
                           u8"靠'细长线 + 枪口端在下/末端靠中心'的几何特征筛选,背景随机(天空/红墙)也不易误识别。");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"取样区域与参数", "laser_section_params"))
    {
        const int res = std::max(64, config.detection_resolution);

        ImGui::TextDisabled(u8"① 识别框(黄):框住镭射可识别的线身。 ");
        changed |= ImGui::SliderInt(u8"识别框宽度（px）", &config.laser_rect_w, 4, res);
        Tooltip(u8"镭射识别矩形宽度(检测像素)。横向需罩住整条斜线。");
        changed |= ImGui::SliderInt(u8"识别框高度（px）", &config.laser_rect_h, 4, res);
        Tooltip(u8"镭射识别矩形高度(检测像素)。纵向需罩住前段清晰的线身,主轴拟合才稳。");
        changed |= ImGui::SliderInt(u8"识别框中心 X（px）", &config.laser_center_x, 0, res);
        Tooltip(u8"识别框中心横坐标(检测像素,0=最左)。画面中心约为 检测分辨率/2。");
        changed |= ImGui::SliderInt(u8"识别框中心 Y（px）", &config.laser_center_y, 0, res);
        Tooltip(u8"识别框中心纵坐标(检测像素,0=最上)。镭射线身通常在中心下方,可把 Y 调大一些。");

        ImGui::Separator();
        ImGui::TextDisabled(u8"② 终点框(棕):静态中心附近、真正命中点所在的小区域。把识别到的线\n"
                            u8"沿前段方向投影进这个框来估计终点 —— 取代'延伸像素',末端再糊也不乱跳。 ");
        changed |= ImGui::SliderInt(u8"终点框中心 X（px）", &config.laser_target_center_x, 0, res);
        Tooltip(u8"终点框中心横坐标。一般贴着画面静态中心(准星处)。");
        changed |= ImGui::SliderInt(u8"终点框中心 Y（px）", &config.laser_target_center_y, 0, res);
        Tooltip(u8"终点框中心纵坐标。一般贴着画面静态中心(准星处)。");
        changed |= ImGui::SliderInt(u8"终点框宽度（px）", &config.laser_target_rect_w, 4, res);
        Tooltip(u8"终点框宽度。终点会被夹在这个框内 → 框越小约束越强(防延伸过头),太小则线没穿过框时回退到可见末端。");
        changed |= ImGui::SliderInt(u8"终点框高度（px）", &config.laser_target_rect_h, 4, res);
        Tooltip(u8"终点框高度。同上。终点取'线上最靠近本框中心、且落在框内'的点。");

        ImGui::Separator();
        changed |= ImGui::SliderFloat(u8"细长度门限", &config.laser_min_elongation, 1.0f, 15.0f, "%.1f");
        Tooltip(u8"判定'是不是一条线'的最小长短轴比。越大越严格,越能挡掉背景里块状红色。\n"
                u8"漏检(镭射较短)就调小,误识别就调大。常用 2.5~5。");

        changed |= ImGui::SliderInt(u8"最少像素数", &config.laser_min_pixel_count, 1, 500);
        Tooltip(u8"一条镭射连通域至少要这么多像素才考虑。太大漏掉细/远镭射,太小易受噪声。");

        changed |= ImGui::SliderInt(u8"形状闭合半径（px）", &config.laser_close_radius, 0, 9);
        Tooltip(u8"形态学闭运算半径,把断续/渐变的镭射桥接成一整条连通线。0=关。镭射发虚时调大(2~4)。");

        changed |= ImGui::SliderFloat(u8"抗抖动平滑", &config.laser_smooth, 0.0f, 1.0f, "%.2f");
        Tooltip(u8"对镭射末端点做自适应低通(One-Euro):静止/微抖时强平滑消抖,快速移动时几乎不平滑、\n"
                u8"不增加延迟。0=关;末端抖就往上调,觉得发黏就调小。常用 0.3~0.7。");
        OverlayUI::EndSection();
    }

    if (OverlayUI::BeginSection(u8"镭射颜色区间", "laser_section_colors"))
    {
        ImGui::TextDisabled(u8"独立于准星颜色。镭射多为红色(需 H 0-10 与 160-179 两段)。\n"
                            u8"末端发淡 → 适当调低 S/V 下限以吃住更长的线身。 ");

        for (size_t i = 0; i < config.laser_colors.size();)
        {
            ImGui::PushID(static_cast<int>(1000 + i));
            std::string title = u8"镭射颜色 #" + std::to_string(i + 1);
            if (!config.laser_colors[i].name.empty())
                title += " - " + config.laser_colors[i].name;

            bool want_delete = false;
            if (OverlayUI::BeginSubsection(title.c_str()))
            {
                if (draw_color_band_row(config.laser_colors[i], want_delete))
                    changed = true;
                OverlayUI::EndSubsection();
            }
            ImGui::PopID();

            if (want_delete)
            {
                config.laser_colors.erase(config.laser_colors.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                continue;
            }
            ++i;
        }

        ImGui::Separator();
        if (ImGui::Button(u8"新增空颜色##laser"))
        {
            CrosshairColorProfileConfig c;
            c.name = u8"镭射颜色 " + std::to_string(config.laser_colors.size() + 1);
            c.s_min = 45; c.v_min = 50;
            config.laser_colors.push_back(std::move(c));
            changed = true;
        }
        ImGui::SameLine();

        static int s_laser_preset_pick = 0;
        const auto& ps = presets();
        std::vector<const char*> labels;
        labels.reserve(ps.size());
        for (const auto* p : ps)
            labels.push_back(p->label);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("##laser_preset", &s_laser_preset_pick, labels.data(), static_cast<int>(labels.size()));
        ImGui::SameLine();
        if (ImGui::Button(u8"添加预设##laser"))
        {
            const ColorPreset* p = ps[static_cast<size_t>(s_laser_preset_pick)];
            config.laser_colors.push_back(p->bandA);
            if (p->has_secondary)
                config.laser_colors.push_back(p->bandB);
            changed = true;
        }

        if (config.laser_colors.empty())
            ImGui::TextDisabled(u8"当前没有配置镭射颜色，镭射找色会一直失败。 ");

        OverlayUI::EndSection();
    }

    if (changed)
        OverlayConfig_MarkDirty();
}
