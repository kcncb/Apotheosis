#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include <imgui/imgui.h>
#include "imgui/imgui_internal.h"

#include "config.h"
#include "Apotheosis.h"
#include "capture.h"
#include "capture_card.h"
#include "other_tools.h"
#include "draw_settings.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

static char udp_ip_buf[64] = "";
static int udp_port_buf = 1234;
static bool udp_settings_init = false;

static char tcp_ip_buf[64] = "";
static int tcp_port_buf = 1235;
static bool tcp_settings_init = false;

static int card_index_buf = 0;
static int card_format_idx = 0;
static int card_w_buf = 0;
static int card_h_buf = 0;
static int card_fps_buf = 0;
static int card_crop_w_buf = 0;
static int card_crop_h_buf = 0;
static bool card_settings_init = false;
static std::vector<CaptureCardDeviceInfo> card_devices;
static std::vector<std::string> card_device_labels;
static std::vector<const char*> card_device_items;

static void RefreshCaptureCardDeviceList()
{
    card_devices = CaptureCard::EnumerateDevices();
    card_device_labels.clear();
    card_device_items.clear();

    if (card_devices.empty())
    {
        card_device_labels.push_back(u8"未检测到可用设备");
    }
    else
    {
        for (const auto& device : card_devices)
            card_device_labels.push_back(device.name);
    }

    for (const auto& label : card_device_labels)
        card_device_items.push_back(label.c_str());
}

static float CaptureCompactComboWidth()
{
    return OverlayUI::AdaptiveItemWidth(0.66f);
}

void draw_capture_settings()
{
    if (OverlayUI::BeginSection(u8"通用采集", "capture_section_general"))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"检测分辨率");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(CaptureCompactComboWidth());
        if (ImGui::InputInt("##capture_detection_resolution", &config.detection_resolution, 16, 64))
        {
            config.detection_resolution = std::clamp(config.detection_resolution, 32, 2048);
            detection_resolution_changed.store(true);
            detector_model_changed.store(true);
            // mouse_thread_loop picks up the new resolution via the
            // detection_resolution_changed flag and re-applies params.
            OverlayConfig_MarkDirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"手动输入检测图像边长，建议使用 32 的倍数。TensorRT 动态模型会按该值重建/更新输入尺寸。");

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"采集 FPS");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(CaptureCompactComboWidth());
        if (ImGui::SliderInt("##capture_fps_slider", &config.capture_fps, 0, 240))
        {
            capture_fps_changed.store(true);
            OverlayConfig_MarkDirty();
        }

        if (config.capture_fps == 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(255, 0, 0, 255), u8"→ 已禁用");
        }

        if (config.capture_fps == 0 || config.capture_fps >= 61)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0f);
            ImGui::TextColored(ImVec4(255, 255, 0, 255), u8"警告：FPS 过高会影响性能。");
            ImGui::PopTextWrapPos();
        }

        if (ImGui::Checkbox(u8"圆形遮罩", &config.circle_mask))
        {
            OverlayConfig_MarkDirty();
        }

        std::vector<std::string> captureMethodOptions = {
            "udp_capture",
            "tcp_capture",
            "capture_card"
        };
        std::vector<std::string> captureMethodLabels = {
            u8"UDP",
            u8"TCP",
            u8"采集卡（直采）"
        };
        std::vector<const char*> captureMethodItems;
        for (const auto& label : captureMethodLabels)
            captureMethodItems.push_back(label.c_str());

        int currentcaptureMethodIndex = 0;
        for (size_t i = 0; i < captureMethodOptions.size(); ++i)
        {
            if (captureMethodOptions[i] == config.capture_method)
            {
                currentcaptureMethodIndex = static_cast<int>(i);
                break;
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"采集方式");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(CaptureCompactComboWidth());
        if (ImGui::Combo("##capture_method", &currentcaptureMethodIndex, captureMethodItems.data(), static_cast<int>(captureMethodItems.size())))
        {
            config.capture_method = captureMethodOptions[currentcaptureMethodIndex];
            OverlayConfig_MarkDirty();
            capture_method_changed.store(true);
        }

        OverlayUI::EndSection();
    }

    if (config.capture_method == "udp_capture")
    {
        if (OverlayUI::BeginSection(u8"UDP 采集", "capture_section_udp"))
        {
            if (!udp_settings_init)
            {
                memset(udp_ip_buf, 0, sizeof(udp_ip_buf));
                std::string ip = config.udp_ip;
                if (ip.size() >= sizeof(udp_ip_buf))
                    ip = ip.substr(0, sizeof(udp_ip_buf) - 1);
                memcpy(udp_ip_buf, ip.c_str(), ip.size());
                udp_port_buf = config.udp_port;
                udp_settings_init = true;
            }

            ImGui::InputText(u8"UDP 地址", udp_ip_buf, IM_ARRAYSIZE(udp_ip_buf));
            ImGui::InputInt(u8"UDP 端口", &udp_port_buf);
            if (ImGui::Button(u8"应用 UDP 设置"))
            {
                udp_port_buf = std::clamp(udp_port_buf, 1, 65535);
                config.udp_ip = udp_ip_buf;
                config.udp_port = udp_port_buf;
                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }

            OverlayUI::EndSection();
        }
    }

    if (config.capture_method == "tcp_capture")
    {
        if (OverlayUI::BeginSection(u8"TCP 采集", "capture_section_tcp"))
        {
            if (!tcp_settings_init)
            {
                memset(tcp_ip_buf, 0, sizeof(tcp_ip_buf));
                std::string ip = config.tcp_ip;
                if (ip.size() >= sizeof(tcp_ip_buf))
                    ip = ip.substr(0, sizeof(tcp_ip_buf) - 1);
                memcpy(tcp_ip_buf, ip.c_str(), ip.size());
                tcp_port_buf = config.tcp_port;
                tcp_settings_init = true;
            }

            ImGui::InputText(u8"TCP 地址", tcp_ip_buf, IM_ARRAYSIZE(tcp_ip_buf));
            ImGui::InputInt(u8"TCP 端口", &tcp_port_buf);
            if (ImGui::Button(u8"应用 TCP 设置"))
            {
                tcp_port_buf = std::clamp(tcp_port_buf, 1, 65535);
                config.tcp_ip = tcp_ip_buf;
                config.tcp_port = tcp_port_buf;
                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }

            OverlayUI::EndSection();
        }
    }

    if (config.capture_method == "capture_card")
    {
        if (OverlayUI::BeginSection(u8"采集卡（直采）", "capture_section_card"))
        {
            static const char* kFormatItems[] = { "AUTO", "NV12", "MJPG", "YUY2", "RGB32" };
            static const std::string kFormatNames[] = { "AUTO", "NV12", "MJPG", "YUY2", "RGB32" };

            if (!card_settings_init)
            {
                card_index_buf = config.capture_card_index;
                card_format_idx = 0;
                for (int i = 0; i < 5; ++i)
                    if (kFormatNames[i] == config.capture_card_format) { card_format_idx = i; break; }
                card_w_buf = config.capture_card_width;
                card_h_buf = config.capture_card_height;
                card_fps_buf = config.capture_card_fps;
                card_crop_w_buf = config.capture_card_crop_width;
                card_crop_h_buf = config.capture_card_crop_height;
                RefreshCaptureCardDeviceList();
                card_settings_init = true;
            }

            if (card_device_items.empty())
                RefreshCaptureCardDeviceList();

            int selected_device = 0;
            for (size_t i = 0; i < card_devices.size(); ++i)
            {
                if (card_devices[i].index == card_index_buf)
                {
                    selected_device = static_cast<int>(i);
                    break;
                }
            }

            if (ImGui::Combo(u8"设备", &selected_device, card_device_items.data(),
                             static_cast<int>(card_device_items.size())))
            {
                if (!card_devices.empty())
                    card_index_buf = card_devices[selected_device].index;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"直采路径使用 Media Foundation 直接对接采集卡驱动，不经过 OpenCV。");

            if (ImGui::Button(u8"刷新设备列表"))
                RefreshCaptureCardDeviceList();

            ImGui::Combo(u8"采集格式", &card_format_idx, kFormatItems, IM_ARRAYSIZE(kFormatItems));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    u8"AUTO：按 NV12 → MJPG → YUY2 → RGB32 顺序自动选择设备支持的最快格式。\n"
                    u8"NV12：直采原始 NV12，CPU 转 BGR。\n"
                    u8"MJPG：直采 JPEG，nvJPEG GPU 解码，零拷贝送给 detector。\n"
                    u8"YUY2 / RGB32：兼容老设备的回退路径。");

            ImGui::InputInt(u8"原始宽度 (0=自动)", &card_w_buf);
            ImGui::InputInt(u8"原始高度 (0=自动)", &card_h_buf);
            ImGui::InputInt(u8"裁剪宽度 (0=原始宽)", &card_crop_w_buf);
            ImGui::InputInt(u8"裁剪高度 (0=原始高)", &card_crop_h_buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"从原始画面中心裁剪指定区域；采集层不缩放。例如 1920x1080 裁 640x640 输出中间 640x640。");
            ImGui::InputInt(u8"采集 FPS (0=自动)", &card_fps_buf);

            if (ImGui::Button(u8"应用采集卡设置"))
            {
                if (card_index_buf < 0) card_index_buf = 0;
                if (card_w_buf < 0) card_w_buf = 0;
                if (card_h_buf < 0) card_h_buf = 0;
                if (card_fps_buf < 0) card_fps_buf = 0;
                if (card_crop_w_buf < 0) card_crop_w_buf = 0;
                if (card_crop_h_buf < 0) card_crop_h_buf = 0;

                config.capture_card_index = card_index_buf;
                config.capture_card_width = card_w_buf;
                config.capture_card_height = card_h_buf;
                config.capture_card_fps = card_fps_buf;
                config.capture_card_format = kFormatNames[card_format_idx];
                config.capture_card_crop_width = card_crop_w_buf;
                config.capture_card_crop_height = card_crop_h_buf;

                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }

            OverlayUI::EndSection();
        }
    }
}
