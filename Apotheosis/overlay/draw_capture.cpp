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
#include "opencv_capture.h"
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

static int opencv_index_buf = 0;
static int opencv_api_idx = 0;
static int opencv_format_idx = 0;
static char opencv_url_buf[256] = "";
static int opencv_w_buf = 0;
static int opencv_h_buf = 0;
static int opencv_fps_buf = 0;
static int opencv_crop_w_buf = 0;
static int opencv_crop_h_buf = 0;
static bool opencv_settings_init = false;
static std::vector<OpenCVCaptureDeviceInfo> opencv_devices;
static std::vector<std::string> opencv_device_labels;
static std::vector<const char*> opencv_device_items;

static void RefreshOpenCVDeviceList(const std::string& api_name)
{
    opencv_devices = OpenCVCapture::EnumerateDevices(api_name, 16);
    opencv_device_labels.clear();
    opencv_device_items.clear();

    if (opencv_devices.empty())
    {
        opencv_device_labels.push_back(u8"未检测到可用设备");
    }
    else
    {
        for (const auto& device : opencv_devices)
            opencv_device_labels.push_back(device.name);
    }

    for (const auto& label : opencv_device_labels)
        opencv_device_items.push_back(label.c_str());
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

        std::vector<std::string> captureMethodOptions = { "udp_capture", "tcp_capture", "opencv_capture" };
        std::vector<const char*> captureMethodItems;
        for (const auto& option : captureMethodOptions)
            captureMethodItems.push_back(option.c_str());

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

    draw_capture_preview();

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

    if (config.capture_method == "opencv_capture")
    {
        if (OverlayUI::BeginSection(u8"OpenCV 采集卡", "capture_section_opencv"))
        {
            static const char* kApiItems[] = { "DSHOW", "MSMF", "FFMPEG", "ANY" };
            static const std::string kApiNames[] = { "DSHOW", "MSMF", "FFMPEG", "ANY" };
            static const char* kFormatItems[] = { "AUTO", "NV12", "MJPG", "YUY2", "YUYV", "RGB3", "BGR3" };
            static const std::string kFormatNames[] = { "AUTO", "NV12", "MJPG", "YUY2", "YUYV", "RGB3", "BGR3" };

            if (!opencv_settings_init)
            {
                opencv_index_buf = config.opencv_capture_index;
                opencv_api_idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (kApiNames[i] == config.opencv_capture_api) { opencv_api_idx = i; break; }
                opencv_format_idx = 0;
                for (int i = 0; i < 7; ++i)
                    if (kFormatNames[i] == config.opencv_capture_format) { opencv_format_idx = i; break; }
                memset(opencv_url_buf, 0, sizeof(opencv_url_buf));
                std::string u = config.opencv_capture_url;
                if (u.size() >= sizeof(opencv_url_buf))
                    u = u.substr(0, sizeof(opencv_url_buf) - 1);
                memcpy(opencv_url_buf, u.c_str(), u.size());
                opencv_w_buf = config.opencv_capture_width;
                opencv_h_buf = config.opencv_capture_height;
                opencv_fps_buf = config.opencv_capture_fps;
                opencv_crop_w_buf = config.opencv_capture_crop_width;
                opencv_crop_h_buf = config.opencv_capture_crop_height;
                RefreshOpenCVDeviceList(kApiNames[opencv_api_idx]);
                opencv_settings_init = true;
            }

            int selected_device = 0;
            for (size_t i = 0; i < opencv_devices.size(); ++i)
            {
                if (opencv_devices[i].index == opencv_index_buf)
                {
                    selected_device = static_cast<int>(i);
                    break;
                }
            }

            if (ImGui::Combo(u8"设备", &selected_device, opencv_device_items.data(), static_cast<int>(opencv_device_items.size())))
            {
                if (!opencv_devices.empty())
                    opencv_index_buf = opencv_devices[selected_device].index;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"自动检测当前可打开的视频输入设备。URL 非空时忽略设备选择。");

            if (ImGui::Button(u8"刷新设备列表"))
                RefreshOpenCVDeviceList(kApiNames[opencv_api_idx]);

            if (ImGui::Combo(u8"驱动接口", &opencv_api_idx, kApiItems, IM_ARRAYSIZE(kApiItems)))
                RefreshOpenCVDeviceList(kApiNames[opencv_api_idx]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"USB 采集卡通常选 DSHOW；UVC 4K / HDMI 采集卡可试 MSMF。");

            ImGui::Combo(u8"采集格式", &opencv_format_idx, kFormatItems, IM_ARRAYSIZE(kFormatItems));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"请求设备输出格式；新增 NV12。AUTO 表示不主动设置 FourCC。");

            ImGui::InputText(u8"URL（可选）", opencv_url_buf, IM_ARRAYSIZE(opencv_url_buf));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"留空 = 按索引打开本地采集卡；填 RTSP/文件路径可用网络/文件源。");

            ImGui::InputInt(u8"原始宽度 (0=自动)", &opencv_w_buf);
            ImGui::InputInt(u8"原始高度 (0=自动)", &opencv_h_buf);
            ImGui::InputInt(u8"裁剪宽度 (0=原始宽)", &opencv_crop_w_buf);
            ImGui::InputInt(u8"裁剪高度 (0=原始高)", &opencv_crop_h_buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(u8"直接从原始画面中心裁剪指定区域；采集层不缩放。例如 1920x1080 裁 640x640 会输出中间 640x640。");
            ImGui::InputInt(u8"采集 FPS (0=自动)", &opencv_fps_buf);

            if (ImGui::Button(u8"应用 OpenCV 设置"))
            {
                if (opencv_index_buf < 0) opencv_index_buf = 0;
                if (opencv_w_buf < 0) opencv_w_buf = 0;
                if (opencv_h_buf < 0) opencv_h_buf = 0;
                if (opencv_fps_buf < 0) opencv_fps_buf = 0;
                if (opencv_crop_w_buf < 0) opencv_crop_w_buf = 0;
                if (opencv_crop_h_buf < 0) opencv_crop_h_buf = 0;

                config.opencv_capture_index = opencv_index_buf;
                config.opencv_capture_api = kApiNames[opencv_api_idx];
                config.opencv_capture_url = opencv_url_buf;
                config.opencv_capture_width = opencv_w_buf;
                config.opencv_capture_height = opencv_h_buf;
                config.opencv_capture_fps = opencv_fps_buf;
                config.opencv_capture_format = kFormatNames[opencv_format_idx];
                config.opencv_capture_crop_width = opencv_crop_w_buf;
                config.opencv_capture_crop_height = opencv_crop_h_buf;

                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }

            OverlayUI::EndSection();
        }
    }
}
