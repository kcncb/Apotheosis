#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>
#include <opencv2/opencv.hpp>
#include <wrl/client.h>

#include "imgui/imgui.h"

#include "capture.h"
#include "crosshair/crosshair_detector.h"
#include "detection_buffer.h"
#include "draw_settings.h"
#include "i_detector.h"
#include "overlay.h"
#include "runtime/inference_session.h"
#include "Apotheosis.h"

namespace
{
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_previewTex;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_previewSRV;
int g_previewTexW = 0;
int g_previewTexH = 0;

bool ensure_texture(int width, int height)
{
    if (!g_pd3dDevice || width <= 0 || height <= 0)
        return false;

    if (g_previewTex && width == g_previewTexW && height == g_previewTexH)
        return true;

    g_previewTex.Reset();
    g_previewSRV.Reset();
    g_previewTexW = width;
    g_previewTexH = height;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, g_previewTex.GetAddressOf())))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(g_pd3dDevice->CreateShaderResourceView(g_previewTex.Get(), &srv, g_previewSRV.GetAddressOf())))
    {
        g_previewTex.Reset();
        return false;
    }
    return true;
}

void upload_frame(const cv::Mat& bgr)
{
    if (bgr.empty() || !g_pd3dDeviceContext)
        return;
    if (!ensure_texture(bgr.cols, bgr.rows))
        return;

    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_pd3dDeviceContext->Map(g_previewTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return;

    const size_t row_bytes = static_cast<size_t>(g_previewTexW) * 4u;
    for (int y = 0; y < g_previewTexH; ++y)
    {
        std::memcpy(static_cast<uint8_t*>(ms.pData) + ms.RowPitch * y,
                    rgba.ptr(y), row_bytes);
    }
    g_pd3dDeviceContext->Unmap(g_previewTex.Get(), 0);
}

cv::Rect center_roi(const cv::Size& frame, int rect_w, int rect_h)
{
    const int w = std::max(4, rect_w);
    const int h = std::max(4, rect_h);
    const cv::Rect frame_rect(0, 0, frame.width, frame.height);
    return cv::Rect(frame.width / 2 - w / 2, frame.height / 2 - h / 2, w, h) & frame_rect;
}

crosshair::CrosshairDetectorSettings snapshot_crosshair_settings(bool& has_enabled_color)
{
    crosshair::CrosshairDetectorSettings settings;
    settings.enabled = true;

    std::lock_guard<std::recursive_mutex> cfg(configMutex);
    settings.rect_w = config.crosshair_rect_w;
    settings.rect_h = config.crosshair_rect_h;
    settings.min_area = config.crosshair_min_area;
    settings.max_area = config.crosshair_max_area;
    settings.colors.reserve(config.crosshair_colors.size());

    has_enabled_color = false;
    for (const auto& c : config.crosshair_colors)
    {
        crosshair::CrosshairColorBand b;
        b.name = c.name;
        b.enabled = c.enabled;
        b.h_low = c.h_low;
        b.h_high = c.h_high;
        b.s_min = c.s_min;
        b.s_max = c.s_max;
        b.v_min = c.v_min;
        b.v_max = c.v_max;
        has_enabled_color = has_enabled_color || b.enabled;
        settings.colors.push_back(std::move(b));
    }
    return settings;
}

void draw_crosshair_marker(ImDrawList* draw_list, const ImVec2& center, float scale)
{
    const float arm = std::clamp(7.0f * scale, 5.0f, 14.0f);
    const float gap = std::clamp(2.0f * scale, 1.5f, 4.0f);
    const ImU32 shadow = IM_COL32(0, 0, 0, 220);
    const ImU32 color = IM_COL32(255, 60, 60, 255);

    draw_list->AddCircle(center, arm * 0.55f, shadow, 24, 3.0f);
    draw_list->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x - gap, center.y), shadow, 3.0f);
    draw_list->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + arm, center.y), shadow, 3.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y - gap), shadow, 3.0f);
    draw_list->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + arm), shadow, 3.0f);

    draw_list->AddCircle(center, arm * 0.55f, color, 24, 1.6f);
    draw_list->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x - gap, center.y), color, 1.6f);
    draw_list->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + arm, center.y), color, 1.6f);
    draw_list->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y - gap), color, 1.6f);
    draw_list->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + arm), color, 1.6f);
}
} // namespace

void draw_detection_preview()
{
    runtime::InferenceSession* session = g_inference_session;
    const bool running = session && session->running();

    if (!running)
    {
        ImGui::TextDisabled(u8"启动推理后将显示预览画面。");
        return;
    }

    cv::Mat frame;
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!latestFrame.empty()) latestFrame.copyTo(frame);
    }

    if (frame.empty())
    {
        ImGui::TextDisabled(u8"等待首帧画面...");
        return;
    }

    upload_frame(frame);
    if (!g_previewSRV || g_previewTexW <= 0 || g_previewTexH <= 0)
    {
        ImGui::TextDisabled(u8"预览纹理不可用。");
        return;
    }

    const float available = ImGui::GetContentRegionAvail().x;
    const float maxWidth = std::clamp(available - 8.0f, 120.0f, 720.0f);
    const float scale = maxWidth / static_cast<float>(g_previewTexW);
    const ImVec2 previewSize(maxWidth, static_cast<float>(g_previewTexH) * scale);

    const ImVec2 originScreen = ImGui::GetCursorScreenPos();
    ImGui::Image(g_previewSRV.Get(), previewSize);

    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    int version = 0;
    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        boxes = detectionBuffer.boxes;
        classes = detectionBuffer.classes;
        version = detectionBuffer.version;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 boxColor = IM_COL32(80, 220, 110, 235);
    const ImU32 textColor = IM_COL32(240, 245, 250, 255);
    const ImU32 textBg = IM_COL32(0, 0, 0, 180);

    static int lastVersionForFps = -1;
    static int framesSinceFpsUpdate = 0;
    static double lastFpsUpdateTime = 0.0;
    static float inferenceFps = 0.0f;
    const double now = ImGui::GetTime();
    if (lastFpsUpdateTime <= 0.0)
        lastFpsUpdateTime = now;
    if (version != lastVersionForFps)
    {
        if (lastVersionForFps >= 0)
            framesSinceFpsUpdate += std::max(0, version - lastVersionForFps);
        else
            framesSinceFpsUpdate = 0;
        lastVersionForFps = version;
    }
    const double fpsElapsed = now - lastFpsUpdateTime;
    if (fpsElapsed >= 0.5)
    {
        inferenceFps = static_cast<float>(framesSinceFpsUpdate / fpsElapsed);
        framesSinceFpsUpdate = 0;
        lastFpsUpdateTime = now;
    }

    float inferenceLatencyMs = 0.0f;
    if (session->detector())
    {
        inferenceLatencyMs = static_cast<float>(
            session->detector()->lastPreprocessTime().count()
            + session->detector()->lastInferenceTime().count()
            + session->detector()->lastCopyTime().count()
            + session->detector()->lastPostprocessTime().count()
            + session->detector()->lastNmsTime().count());
    }

    float displayedInferenceFps = inferenceFps;
    if (displayedInferenceFps <= 0.01f && inferenceLatencyMs > 0.01f)
        displayedInferenceFps = 1000.0f / inferenceLatencyMs;

    char perfText[96];
    std::snprintf(perfText, sizeof(perfText), u8"推理 %.1f FPS | 延迟 %.1f ms", displayedInferenceFps, inferenceLatencyMs);
    const ImVec2 perfPos(originScreen.x + 6.0f, originScreen.y + 6.0f);
    const ImVec2 perfTextSize = ImGui::CalcTextSize(perfText);
    drawList->AddRectFilled(
        ImVec2(perfPos.x - 4.0f, perfPos.y - 3.0f),
        ImVec2(perfPos.x + perfTextSize.x + 5.0f, perfPos.y + perfTextSize.y + 4.0f),
        IM_COL32(0, 0, 0, 165),
        4.0f);
    drawList->AddText(perfPos, IM_COL32(255, 255, 255, 245), perfText);

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const cv::Rect& r = boxes[i];
        const ImVec2 tl(originScreen.x + r.x * scale, originScreen.y + r.y * scale);
        const ImVec2 br(originScreen.x + (r.x + r.width) * scale, originScreen.y + (r.y + r.height) * scale);
        drawList->AddRect(tl, br, boxColor, 0.0f, 0, 1.5f);

        char label[48];
        const int classId = (i < classes.size()) ? classes[i] : -1;
        std::snprintf(label, sizeof(label), u8"类别 %d", classId);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 labelTl(tl.x, tl.y - textSize.y - 4.0f);
        const ImVec2 labelBr(tl.x + textSize.x + 6.0f, tl.y);
        drawList->AddRectFilled(labelTl, labelBr, textBg);
        drawList->AddText(ImVec2(labelTl.x + 3.0f, labelTl.y + 1.0f), textColor, label);
    }

    bool has_enabled_color = false;
    const auto crosshair_settings = snapshot_crosshair_settings(has_enabled_color);
    const cv::Rect roi = center_roi(frame.size(), crosshair_settings.rect_w, crosshair_settings.rect_h);
    if (roi.width > 0 && roi.height > 0)
    {
        const ImVec2 roiTl(originScreen.x + roi.x * scale, originScreen.y + roi.y * scale);
        const ImVec2 roiBr(originScreen.x + (roi.x + roi.width) * scale, originScreen.y + (roi.y + roi.height) * scale);
        drawList->AddRectFilled(roiTl, roiBr, IM_COL32(0, 190, 255, 28));
        drawList->AddRect(roiTl, roiBr, IM_COL32(0, 190, 255, 240), 0.0f, 0, 1.5f);
        drawList->AddText(ImVec2(roiTl.x + 4.0f, roiTl.y + 3.0f), IM_COL32(0, 220, 255, 255), u8"准星找色范围");
    }

    bool crosshair_hit = false;
    if (has_enabled_color && frame.type() == CV_8UC3)
    {
        static crosshair::CrosshairDetector detector;
        auto hit = detector.detect(frame, crosshair_settings);
        if (hit)
        {
            crosshair_hit = true;
            draw_crosshair_marker(drawList,
                ImVec2(originScreen.x + hit->x * scale, originScreen.y + hit->y * scale),
                scale);
        }
    }

    ImGui::Text(u8"检测数: %d（帧 #%d）", static_cast<int>(boxes.size()), version);
    if (session->detector())
    {
        ImGui::SameLine();
        ImGui::TextDisabled(u8"| 推理 %.1f ms", session->detector()->lastInferenceTime().count());
    }
    ImGui::TextDisabled(u8"准星找色: %s | 范围 %dx%d | 面积 %d-%d",
        crosshair_hit ? u8"已命中" : (has_enabled_color ? u8"未命中" : u8"无启用颜色"),
        crosshair_settings.rect_w,
        crosshair_settings.rect_h,
        crosshair_settings.min_area,
        crosshair_settings.max_area);
}
