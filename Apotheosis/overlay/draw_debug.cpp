#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <d3d11.h>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "Apotheosis.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "include/other_tools.h"
#include "capture.h"
#include "overlay/ui_sections.h"

#include "depth/depth_mask.h"
#include "runtime/aim_telemetry.h"
#include "config/config.h"
#include <atomic>
#include <mutex>

// Replay playback state. Owned here because the Debug panel toggles it and
// the detection-preview overlay reads it. Frame index is in detection
// buffer order; wraps at snapshot end.
std::atomic<bool> g_replay_playback_active{ false };
std::atomic<int>  g_replay_playback_frame{ 0 };

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p)       \
    do {                      \
        if ((p) != nullptr) { \
            (p)->Release();   \
            (p) = nullptr;    \
        }                     \
    } while (0)
#endif

int prev_screenshot_delay = config.screenshot_delay;
bool prev_verbose = config.verbose;

static ID3D11Texture2D* g_debugTex = nullptr;
static ID3D11ShaderResourceView* g_debugSRV = nullptr;
static int texW = 0, texH = 0;

static ID3D11Texture2D* g_maskTex = nullptr;
static ID3D11ShaderResourceView* g_maskSRV = nullptr;
static int maskTexW = 0, maskTexH = 0;

static float debug_scale = 0.5f;

static int findDebugKeyIndexByName(const std::string& keyName)
{
    for (size_t k = 0; k < key_names.size(); ++k)
    {
        if (key_names[k] == keyName)
            return static_cast<int>(k);
    }
    return 0;
}

static bool drawScreenshotButtonRows()
{
    if (key_names_cstrs.empty())
    {
        ImGui::TextDisabled(u8"没有可用的按键列表。");
        return false;
    }

    bool changed = false;
    if (config.screenshot_button.empty())
    {
        config.screenshot_button.push_back("None");
        changed = true;
    }

    for (size_t i = 0; i < config.screenshot_button.size();)
    {
        std::string& currentKeyName = config.screenshot_button[i];
        int currentIndex = findDebugKeyIndexByName(currentKeyName);

        ImGui::PushID(static_cast<int>(i));

        const float rowAvail = ImGui::GetContentRegionAvail().x;
        const float actionBtnW = ImGui::GetFrameHeight();
        float comboWidth = rowAvail - (actionBtnW * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f);
        const float comboMin = rowAvail * 0.56f;
        if (comboWidth < comboMin)
            comboWidth = comboMin;
        if (comboWidth < 1.0f)
            comboWidth = 1.0f;
        ImGui::SetNextItemWidth(comboWidth);

        if (ImGui::Combo("##screenshot_binding_combo", &currentIndex, key_names_cstrs.data(), static_cast<int>(key_names_cstrs.size())))
        {
            currentKeyName = key_names[currentIndex];
            changed = true;
        }

        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(actionBtnW, 0.0f)))
        {
            config.screenshot_button.insert(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true;
        }

        ImGui::SameLine(0.0f, 3.0f);
        bool removedCurrent = false;
        if (ImGui::Button("-", ImVec2(actionBtnW, 0.0f)))
        {
            if (config.screenshot_button.size() <= 1)
            {
                config.screenshot_button[0] = "None";
            }
            else
            {
                config.screenshot_button.erase(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removedCurrent = true;
            }
            changed = true;
        }

        ImGui::PopID();

        if (removedCurrent)
            continue;

        ++i;
    }

    return changed;
}

static void uploadDebugFrame(const cv::Mat& bgr)
{
    if (bgr.empty()) return;

    if (!g_debugTex || bgr.cols != texW || bgr.rows != texH)
    {
        SAFE_RELEASE(g_debugTex);
        SAFE_RELEASE(g_debugSRV);

        texW = bgr.cols;  texH = bgr.rows;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = texW;
        td.Height = texH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_debugTex);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        g_pd3dDevice->CreateShaderResourceView(g_debugTex, &sd, &g_debugSRV);
    }

    static cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_debugTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < texH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), texW * 4);
        g_pd3dDeviceContext->Unmap(g_debugTex, 0);
    }
}

static void uploadMaskFrame(const cv::Mat& rgba)
{
    if (rgba.empty()) return;

    if (!g_maskTex || rgba.cols != maskTexW || rgba.rows != maskTexH)
    {
        SAFE_RELEASE(g_maskTex);
        SAFE_RELEASE(g_maskSRV);

        maskTexW = rgba.cols;
        maskTexH = rgba.rows;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = maskTexW;
        td.Height = maskTexH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_maskTex);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        g_pd3dDevice->CreateShaderResourceView(g_maskTex, &sd, &g_maskSRV);
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_maskTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < maskTexH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), maskTexW * 4);
        g_pd3dDeviceContext->Unmap(g_maskTex, 0);
    }
}

void draw_debug_frame()
{
    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
            latestFrame.copyTo(frameCopy);
    }

    uploadDebugFrame(frameCopy);

    if (!g_debugSRV) return;

    ImGui::SliderFloat(u8"调试缩放", &debug_scale, 0.1f, 2.0f, "%.1fx");

    ImVec2 image_size(texW * debug_scale, texH * debug_scale);
    ImGui::Image(g_debugSRV, image_size);

    ImVec2 image_pos = ImGui::GetItemRectMin();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (config.depth_mask_enabled)
    {
        auto& depthMask = depth_anything::GetDepthMaskGenerator();
        cv::Mat mask = depthMask.getMask();
        if (!mask.empty() && mask.size() == frameCopy.size())
        {
            cv::Mat alpha(mask.size(), CV_8U, cv::Scalar(0));
            alpha.setTo(config.depth_mask_alpha, mask);

            std::vector<cv::Mat> channels(4);
            channels[0] = cv::Mat(mask.size(), CV_8U, cv::Scalar(255));
            channels[1] = cv::Mat(mask.size(), CV_8U, cv::Scalar(0));
            channels[2] = cv::Mat(mask.size(), CV_8U, cv::Scalar(0));
            channels[3] = alpha;

            cv::Mat rgba;
            cv::merge(channels, rgba);
            uploadMaskFrame(rgba);

            if (g_maskSRV)
            {
                ImVec2 overlay_max(image_pos.x + image_size.x, image_pos.y + image_size.y);
                draw_list->AddImage(g_maskSRV, image_pos, overlay_max);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        for (size_t i = 0; i < detectionBuffer.boxes.size(); ++i)
        {
            const cv::Rect& box = detectionBuffer.boxes[i];

            ImVec2 p1(image_pos.x + box.x * debug_scale,
                image_pos.y + box.y * debug_scale);
            ImVec2 p2(p1.x + box.width * debug_scale,
                p1.y + box.height * debug_scale);

            ImU32 color = IM_COL32(255, 0, 0, 255);

            draw_list->AddRect(p1, p2, color, 0.0f, 0, 2.0f);

            if (i < detectionBuffer.classes.size())
            {
                std::string label = std::to_string(detectionBuffer.classes[i]);
                draw_list->AddText(ImVec2(p1.x, p1.y - 16), IM_COL32(255, 255, 0, 255), label.c_str());
            }
        }
    }

    if (globalMouseThread)
    {
        auto futurePts = globalMouseThread->getFuturePositions();
        if (!futurePts.empty())
        {
            float scale_x = static_cast<float>(texW) / config.detection_resolution;
            float scale_y = static_cast<float>(texH) / config.detection_resolution;

            ImVec2 clip_min = image_pos;
            ImVec2 clip_max = ImVec2(image_pos.x + texW * debug_scale,
                image_pos.y + texH * debug_scale);
            draw_list->PushClipRect(clip_min, clip_max, true);

            int totalPts = static_cast<int>(futurePts.size());
            for (size_t i = 0; i < futurePts.size(); ++i)
            {
                int px = static_cast<int>(futurePts[i].first * scale_x);
                int py = static_cast<int>(futurePts[i].second * scale_y);
                ImVec2 pt(image_pos.x + px * debug_scale,
                    image_pos.y + py * debug_scale);

                int b = static_cast<int>(255 - (i * 255.0 / totalPts));
                int r = static_cast<int>(i * 255.0 / totalPts);
                int g = 50;

                ImU32 fillColor = IM_COL32(r, g, b, 255);
                ImU32 outlineColor = IM_COL32(255, 255, 255, 255);

                draw_list->AddCircleFilled(pt, 4.0f * debug_scale, fillColor);
                draw_list->AddCircle(pt, 4.0f * debug_scale, outlineColor, 0, 1.0f);
            }

            draw_list->PopClipRect();
        }
    }
}

void draw_capture_preview()
{
    if (OverlayUI::BeginSection(u8"采集预览", "capture_section_preview"))
    {
        if (ImGui::Checkbox(u8"显示预览窗口", &config.show_window))
        {
            OverlayConfig_MarkDirty();
        }

        if (config.show_window)
        {
            draw_debug_frame();
        }

        OverlayUI::EndSection();
    }
}

void draw_debug()
{
    if (OverlayUI::BeginSection(u8"截图按键", "debug_section_screenshot_buttons"))
    {
        if (drawScreenshotButtonRows())
            OverlayConfig_MarkDirty();

        ImGui::InputInt(u8"截图延迟 (ms)", &config.screenshot_delay, 50, 500);
        ImGui::Checkbox(u8"控制台输出详细日志", &config.verbose);

        if (ImGui::Button(u8"打印 OpenCV 构建信息##button_cv2_build_info"))
        {
            std::cout << cv::getBuildInformation() << std::endl;
        }

        OverlayUI::EndSection();
    }

    if (prev_screenshot_delay != config.screenshot_delay ||
        prev_verbose != config.verbose)
    {
        prev_screenshot_delay = config.screenshot_delay;
        prev_verbose = config.verbose;
        OverlayConfig_MarkDirty();
    }

    // ---------------------------------------------------------------------
    // Aim trajectory replay playback floats as its own window so we don't
    // have to fight with the existing D3D capture preview pipeline. Boxes
    // are drawn in detection-pixel space scaled to the window content area.
    // ---------------------------------------------------------------------
    if (g_replay_playback_active.load())
    {
        const auto frames = runtime::ReplayBuffer::instance().snapshot();
        if (frames.empty())
        {
            g_replay_playback_active.store(false);
        }
        else
        {
            // Advance frame index based on real wall-clock and the chosen
            // playback speed. We re-derive elapsed each call rather than
            // storing a start time so re-opening the panel keeps the cursor.
            static auto s_playback_anchor = std::chrono::steady_clock::now();
            static int  s_playback_anchor_frame = 0;
            const int curIdx = g_replay_playback_frame.load();
            if (curIdx == 0)
            {
                s_playback_anchor = std::chrono::steady_clock::now();
                s_playback_anchor_frame = 0;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - s_playback_anchor).count();
            const double scaled = elapsed * static_cast<double>(std::max(0.05f, config.replay_playback_speed));
            // Map elapsed scaled time to a frame index by walking until the
            // next snapshot ts is past `start_ts + scaled`.
            const auto baseTs = frames.front().ts;
            int newIdx = s_playback_anchor_frame;
            while (newIdx + 1 < static_cast<int>(frames.size()))
            {
                const double tsOffset = std::chrono::duration<double>(frames[newIdx + 1].ts - baseTs).count();
                if (tsOffset > scaled) break;
                ++newIdx;
            }
            if (newIdx >= static_cast<int>(frames.size()) - 1)
            {
                // Loop the snapshot indefinitely until user stops.
                s_playback_anchor = now;
                s_playback_anchor_frame = 0;
                newIdx = 0;
            }
            g_replay_playback_frame.store(newIdx);

            const runtime::ReplayFrame& f = frames[newIdx];
            ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(u8"瞄准轨迹回放##playback", nullptr,
                             ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::Text(u8"帧 %d / %zu  (%.1fs 缓冲, %.2fx 慢放)",
                            newIdx + 1, frames.size(),
                            static_cast<double>(config.replay_seconds),
                            static_cast<double>(config.replay_playback_speed));
                bool playing = g_replay_playback_active.load();
                if (ImGui::Checkbox(u8"播放中", &playing))
                    g_replay_playback_active.store(playing);

                const ImVec2 topLeft = ImGui::GetCursorScreenPos();
                const ImVec2 avail   = ImGui::GetContentRegionAvail();
                const float side     = std::min(avail.x, avail.y);
                const float scale    = side / std::max(1, config.detection_resolution);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(topLeft,
                                  ImVec2(topLeft.x + side, topLeft.y + side),
                                  IM_COL32(20, 20, 24, 255));

                for (size_t i = 0; i < f.boxes.size(); ++i)
                {
                    const auto& b = f.boxes[i];
                    const int track_id = (i < f.track_ids.size()) ? f.track_ids[i] : -1;
                    const bool is_lock = (track_id == f.locked_track_id) && f.locked_track_id >= 0;
                    const ImU32 col = is_lock
                        ? IM_COL32(255, 200, 60, 230)
                        : IM_COL32(120, 200, 255, 180);
                    const ImVec2 a(topLeft.x + b.x * scale, topLeft.y + b.y * scale);
                    const ImVec2 c(a.x + b.width * scale, a.y + b.height * scale);
                    dl->AddRect(a, c, col, 0.0f, 0, is_lock ? 2.5f : 1.5f);
                }

                if (f.locked_track_id >= 0)
                {
                    const ImVec2 piv(topLeft.x + f.pivot_x * scale,
                                     topLeft.y + f.pivot_y * scale);
                    dl->AddCircleFilled(piv, 4.0f, IM_COL32(255, 80, 80, 240));
                }
                if (f.hotkey_active)
                {
                    dl->AddRect(topLeft,
                                ImVec2(topLeft.x + side, topLeft.y + side),
                                IM_COL32(255, 100, 100, 200), 0.0f, 0, 2.0f);
                }
                ImGui::Dummy(ImVec2(side, side));
            }
            ImGui::End();
        }
    }

    // ---------------------------------------------------------------------
    // Aim trajectory replay. Snapshot captures the last N seconds of
    // detections + lock state so the user can slow-play their own aim and
    // see why a swing over- or under-shot. Lives inside the existing debug
    // page because it's a tuning aid, not a runtime feature.
    // ---------------------------------------------------------------------
    if (OverlayUI::BeginSection(u8"瞄准轨迹回放", "debug_section_replay"))
    {
        bool replay_dirty = false;
        replay_dirty |= ImGui::Checkbox(u8"启用环形录制", &config.replay_record_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(u8"开启后持续把每帧的检测框、锁定 ID、锚点写入环形缓冲。关闭时无任何分配。");
        replay_dirty |= ImGui::SliderInt(u8"保留秒数", &config.replay_seconds, 1, 60);
        replay_dirty |= ImGui::SliderFloat(u8"回放速度", &config.replay_playback_speed, 0.05f, 2.0f, "%.2fx");
        if (replay_dirty) OverlayConfig_MarkDirty();

        const auto& replay = runtime::ReplayBuffer::instance();
        ImGui::TextDisabled(u8"缓冲帧数：%zu", replay.size());
        if (ImGui::Button(u8"清空缓冲"))
            runtime::ReplayBuffer::instance().clear();
        ImGui::SameLine();
        if (ImGui::Button(u8"快照并慢放（叠在画面上）"))
        {
            // The actual playback overlay floats below as its own ImGui
            // window when g_replay_playback_active is set.
            g_replay_playback_frame.store(0);
            g_replay_playback_active.store(true);
        }
        ImGui::SameLine();
        if (ImGui::Button(u8"停止回放"))
            g_replay_playback_active.store(false);
        ImGui::SameLine();
        ImGui::TextDisabled(g_replay_playback_active.load() ? u8"● 回放中" : u8"○ 待机");
        OverlayUI::EndSection();
    }

    // ---------------------------------------------------------------------
    // Dynamic FOV telemetry — read-only readout of what the mouse loop is
    // currently using as the elliptical aim region. Per-hotkey toggle and
    // tuning live on the Hotkeys page (the FOV is a per-hotkey trait).
    // ---------------------------------------------------------------------
    if (OverlayUI::BeginSection(u8"动态 FOV (实时)", "debug_section_dynamic_fov"))
    {
        const float rx = g_dynamic_fov_radius_x_px.load();
        const float ry = g_dynamic_fov_radius_y_px.load();
        if (rx > 0.0f && ry > 0.0f)
        {
            ImGui::Text(u8"当前有效 FOV (像素直径)：%.0f × %.0f",
                        rx * 2.0f, ry * 2.0f);
            ImGui::TextDisabled(u8"（瞄准时按目标距准星距离与 bbox 大小动态收缩）");
        }
        else
        {
            ImGui::TextDisabled(u8"未启用 / 无锁定目标。在「瞄准热键」面板的「动态 FOV」子段中开启。");
        }
        OverlayUI::EndSection();
    }
}
