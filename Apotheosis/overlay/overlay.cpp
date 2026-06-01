#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <tchar.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>
#include <cmath>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "overlay.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/overlay_image.h"
#include "include/other_tools.h"
#include "config.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "auth/auth_state.h"

#include "trt_detector.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")

ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
IDXGISwapChain1* g_pSwapChain = NULL;
IDCompositionDevice* g_dcompDevice = NULL;
IDCompositionTarget* g_dcompTarget = NULL;
IDCompositionVisual* g_dcompVisual = NULL;
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
HWND g_hwnd = NULL;

extern Config config;
extern std::recursive_mutex configMutex;
extern std::atomic<bool> shouldExit;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

ID3D11BlendState* g_pBlendState = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

const int BASE_OVERLAY_WIDTH = 720;
const int BASE_OVERLAY_HEIGHT = 500;
static const int MIN_EDITOR_OPACITY = 220;

int overlayWidth = 0;
int overlayHeight = 0;

static const int DRAG_BAR_HEIGHT_PX = 30;
static const int MIN_OVERLAY_W = 420;
static const int MIN_OVERLAY_H = 300;
static const int RESIZE_BORDER_PX = 8;
static const int WORKAREA_MARGIN_PX = 20;
static const float WINDOW_BUTTON_SIZE = 28.0f;
static const float WINDOW_BUTTON_GAP = 2.0f;

static bool g_autoResizeEnabled = true;
static ImGuiStyle g_baseStyle{};
static bool g_baseStyleReady = false;
static float g_runtimeUiScale = -1.0f;

static OverlayImage g_logo{};
static HICON g_logoIconBig = NULL;
static HICON g_logoIconSmall = NULL;

static void LoadOverlayLogo()
{
    if (g_logo.srv || !g_pd3dDevice)
        return;

    // Look in the executable directory first, then fall back to the working dir.
    char modulePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(NULL, modulePath, MAX_PATH) > 0)
    {
        if (char* slash = std::strrchr(modulePath, '\\'))
            slash[1] = '\0';
        std::string p = std::string(modulePath) + "apotheosis_logo.png";
        if (OverlayImage_LoadFromFile(p.c_str(), g_pd3dDevice, g_logo))
            return;
    }
    OverlayImage_LoadFromFile("apotheosis_logo.png", g_pd3dDevice, g_logo);
}

static void ApplyOverlayWindowIcon()
{
    if (!g_hwnd) return;

    char modulePath[MAX_PATH] = {};
    std::string path = "apotheosis_logo.png";
    if (::GetModuleFileNameA(NULL, modulePath, MAX_PATH) > 0)
    {
        if (char* slash = std::strrchr(modulePath, '\\'))
            slash[1] = '\0';
        std::string p = std::string(modulePath) + "apotheosis_logo.png";
        if (::GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            path = p;
    }

    g_logoIconBig = OverlayImage_LoadHIconFromFile(path.c_str());
    if (g_logoIconBig)
    {
        ::SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_logoIconBig));
        ::SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_logoIconBig));
    }
}

static void DrawSidebarBrand(float availWidth)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    const float logoSize = std::clamp(availWidth * 0.55f, 36.0f, 84.0f);
    const float blockH = logoSize + 18.0f + 6.0f; // logo + text line + bottom margin

    // Subtle bottom gold separator.
    const ImU32 goldHi = IM_COL32(240, 210, 130, 235);
    const ImU32 gold = IM_COL32(212, 175, 95, 200);

    if (g_logo.srv)
    {
        const float cx = pos.x + (availWidth - logoSize) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx, pos.y));
        ImGui::Image(reinterpret_cast<ImTextureID>(g_logo.srv), ImVec2(logoSize, logoSize));
    }
    else
    {
        ImGui::Dummy(ImVec2(0.0f, logoSize));
    }

    // "APOTHEOSIS" wordmark in gold.
    const char* brand = "APOTHEOSIS";
    const ImVec2 ts = ImGui::CalcTextSize(brand);
    const float tx = pos.x + (availWidth - ts.x) * 0.5f;
    const float ty = pos.y + logoSize + 2.0f;
    draw->AddText(ImVec2(tx, ty), goldHi, brand);

    // Hairline separator under the brand.
    const float sepY = ty + ts.y + 4.0f;
    draw->AddLine(ImVec2(pos.x + 4.0f, sepY), ImVec2(pos.x + availWidth - 4.0f, sepY), gold, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + blockH));
}

std::vector<std::string> availableModels;
std::vector<std::string> key_names;
std::vector<const char*> key_names_cstrs;

static UINT GetDpiForWindowSafe(HWND hwnd);
static RECT GetOverlayWorkArea(HWND hwnd);
static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h);
static void EnsureOverlayInsideWorkArea(HWND hwnd);
static float GetWindowButtonStripWidth();
static bool IsPointInWindowButtonStrip(HWND hwnd, POINT clientPt);

std::vector<std::string> getAvailableModels();

static inline int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static float ComputeRuntimeUiScale(float windowW, float windowH)
{
    const float safeW = (windowW > 1.0f) ? windowW : static_cast<float>(BASE_OVERLAY_WIDTH);
    const float safeH = (windowH > 1.0f) ? windowH : static_cast<float>(BASE_OVERLAY_HEIGHT);
    const float refW = static_cast<float>(BASE_OVERLAY_WIDTH);
    const float refH = static_cast<float>(BASE_OVERLAY_HEIGHT);

    const float wFactor = safeW / refW;
    const float hFactor = safeH / refH;
    float autoFactor = std::sqrt(wFactor * hFactor);
    autoFactor = std::clamp(autoFactor, 0.85f, 1.90f);

    const float userFactor = std::clamp(config.overlay_ui_scale, 0.85f, 1.35f);
    return std::clamp(autoFactor * userFactor, 0.80f, 2.20f);
}

static void ApplyRuntimeUiScale(float windowW, float windowH)
{
    if (!g_baseStyleReady)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const float targetScale = ComputeRuntimeUiScale(windowW, windowH);
    if (std::fabs(targetScale - g_runtimeUiScale) > 0.01f)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = g_baseStyle;
        style.ScaleAllSizes(targetScale);
        g_runtimeUiScale = targetScale;
    }
    io.FontGlobalScale = targetScale;
}

static void TryAutoResizeOverlay(float extraContentWidth)
{
    if (!g_hwnd || !g_autoResizeEnabled)
        return;

    // Keep auto-grow only for severe horizontal overflow.
    if (extraContentWidth <= 120.0f)
        return;

    const int extraPx = ClampInt(static_cast<int>(extraContentWidth + 0.5f), 0, 260);
    if (extraPx <= 0)
        return;

    RECT wndRect{};
    ::GetWindowRect(g_hwnd, &wndRect);

    int targetX = wndRect.left;
    int targetY = wndRect.top;
    int targetW = overlayWidth + extraPx;
    int targetH = overlayHeight; // Height is user-controlled; content area already scrolls vertically.

    ClampOverlayToWorkArea(g_hwnd, targetX, targetY, targetW, targetH);

    if (targetW != overlayWidth || targetX != wndRect.left || targetY != wndRect.top)
        SetWindowPos(g_hwnd, NULL, targetX, targetY, targetW, targetH, SWP_NOZORDER);
}

void Overlay_SetOpacity(int opacity255)
{
    if (!g_hwnd) return;

    opacity255 = ClampInt(opacity255, MIN_EDITOR_OPACITY, 255);

    LONG exStyle = GetWindowLong(g_hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) == 0)
        SetWindowLong(g_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

    SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)opacity255, LWA_ALPHA);
}

static inline ImVec4 RGBA(int r, int g, int b, int a = 255)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static void ApplyTheme_RoseDark()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(9.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 5.0f);
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 10.0f;
    style.IndentSpacing = 12.0f;

    ImVec4* c = style.Colors;

    const ImVec4 bg0 = RGBA(4, 4, 4, 250);
    const ImVec4 bg1 = RGBA(10, 10, 10, 250);
    const ImVec4 bg2 = RGBA(16, 16, 16, 245);
    const ImVec4 stroke = RGBA(255, 255, 255, 56);
    const ImVec4 strokeHi = RGBA(255, 255, 255, 92);

    const ImVec4 text = RGBA(232, 237, 245, 255);
    const ImVec4 textDim = RGBA(143, 160, 182, 255);
    const ImVec4 bright = RGBA(245, 245, 245, 255);

    // Apotheosis gold accent palette (sampled from logo).
    const ImVec4 gold = RGBA(212, 175, 95, 255);
    const ImVec4 goldHi = RGBA(240, 210, 130, 255);
    const ImVec4 goldDim = RGBA(160, 125, 55, 255);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;

    c[ImGuiCol_WindowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = bg1;

    c[ImGuiCol_Border] = stroke;
    c[ImGuiCol_BorderShadow] = RGBA(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = RGBA(24, 24, 24, 250);
    c[ImGuiCol_FrameBgActive] = RGBA(31, 31, 31, 252);

    c[ImGuiCol_TitleBg] = bg1;
    c[ImGuiCol_TitleBgActive] = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bg1;
    c[ImGuiCol_MenuBarBg] = bg0;

    c[ImGuiCol_ScrollbarBg] = RGBA(0, 0, 0, 95);
    c[ImGuiCol_ScrollbarGrab] = RGBA(96, 96, 96, 170);
    c[ImGuiCol_ScrollbarGrabHovered] = RGBA(122, 122, 122, 210);
    c[ImGuiCol_ScrollbarGrabActive] = RGBA(145, 145, 145, 232);

    c[ImGuiCol_CheckMark] = goldHi;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_SliderGrabActive] = goldHi;

    c[ImGuiCol_Button] = RGBA(14, 14, 14, 246);
    c[ImGuiCol_ButtonHovered] = RGBA(28, 24, 14, 250);
    c[ImGuiCol_ButtonActive] = RGBA(48, 38, 18, 252);

    c[ImGuiCol_Header] = RGBA(40, 32, 14, 200);
    c[ImGuiCol_HeaderHovered] = RGBA(60, 48, 20, 220);
    c[ImGuiCol_HeaderActive] = RGBA(86, 68, 28, 235);

    c[ImGuiCol_Separator] = stroke;
    c[ImGuiCol_SeparatorHovered] = RGBA(212, 175, 95, 160);
    c[ImGuiCol_SeparatorActive] = RGBA(240, 210, 130, 220);

    c[ImGuiCol_Tab] = RGBA(14, 14, 14, 248);
    c[ImGuiCol_TabHovered] = RGBA(40, 32, 14, 250);
    c[ImGuiCol_TabActive] = RGBA(70, 56, 22, 252);
    c[ImGuiCol_TabUnfocused] = RGBA(12, 12, 12, 240);
    c[ImGuiCol_TabUnfocusedActive] = RGBA(40, 32, 14, 248);

    c[ImGuiCol_ResizeGrip] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = RGBA(0, 0, 0, 0);

    c[ImGuiCol_PlotLines] = RGBA(216, 216, 216, 255);
    c[ImGuiCol_PlotHistogram] = RGBA(216, 216, 216, 255);

    c[ImGuiCol_TableHeaderBg] = bg1;
    c[ImGuiCol_TableBorderStrong] = stroke;
    c[ImGuiCol_TableBorderLight] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_TableRowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = RGBA(255, 255, 255, 6);

    c[ImGuiCol_NavHighlight] = RGBA(240, 210, 130, 160);
    c[ImGuiCol_NavWindowingHighlight] = RGBA(240, 210, 130, 130);
    c[ImGuiCol_NavWindowingDimBg] = RGBA(0, 0, 0, 110);

    c[ImGuiCol_TextSelectedBg] = RGBA(212, 175, 95, 80);
    c[ImGuiCol_DragDropTarget] = RGBA(240, 210, 130, 220);
}

struct OverlayTabItem
{
    const char* label;
    const char* group;
    const char* description;
    void (*draw)();
};

static const OverlayTabItem kOverlayTabs[] = {
    { u8"账号授权", u8"会话", u8"登录、注册并获取模型授权。",                               draw_auth },
    { u8"启动",     u8"会话", u8"选择推理后端与模型，启动或停止推理。",                       draw_session },
    { u8"模型加密", u8"会话", u8"加密模型并把模型授权给指定账号。",                           draw_model_tools },
    { u8"画面采集", u8"核心", u8"采集源和画面输入相关设置。",                                 draw_capture_settings },
    { u8"目标",     u8"核心", u8"按类别分配：删除、过滤、瞄准。",                             draw_target },
    { u8"瞄准热键", u8"控制", u8"定义每个瞄准热键的触发键、类别优先级、Y 偏移与完整鼠标参数。", draw_hotkeys },
    { u8"准星找色", u8"控制", u8"BGR 到 HSV 识别动态准星颜色，替代静态画面中心作为瞄准参考点。", draw_crosshair },
    { u8"镭射找色", u8"控制", u8"独立识别镭射线并取其末端作为瞄准参考点;与准星找色互不影响、准星优先。", draw_laser },
    { u8"宏脚本",   u8"控制", u8"G HUB / LGS 兼容的 Lua 宏。脚本可直接复制粘贴运行,自动走当前硬件后端。", draw_macro },
    { u8"硬件",     u8"核心", u8"物理输入设备：Win32 / GHUB / Arduino / Kmbox / MAKCU。",       draw_hardware },
    { u8"AI 模型",  u8"核心", u8"模型与检测器阈值。",                                         draw_ai },
    { u8"界面外观", u8"控制", u8"编辑器外观与缩放。",                                         draw_overlay },
    { u8"性能统计", u8"监控", u8"性能与耗时曲线。",                                           draw_stats },
    { u8"日志",     u8"监控", u8"查看启动、运行与错误输出。",                                 draw_log },
    { u8"调试",     u8"监控", u8"截图按键绑定与诊断。",                                       draw_debug },
};

enum class WindowButtonIcon
{
    Minimize,
    Maximize,
    Restore,
    Close,
};

static void DrawMainPanelBackground(const ImVec2& pos, const ImVec2& size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, max, IM_COL32(4, 4, 4, 248), 0.0f);
    draw->AddRect(pos, max, IM_COL32(255, 255, 255, 56), 0.0f, 0, 1.0f);
}

static bool DrawSidebarTabButton(const char* label, bool selected)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + style.ItemSpacing.y * 0.15f);
    if (size.x < 1.0f)
        size.x = 1.0f;

    const std::string id = std::string("##nav_") + label;
    const bool pressed = ImGui::InvisibleButton(id.c_str(), size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);

    ImU32 rowBg = IM_COL32(0, 0, 0, 0);
    if (selected)
        rowBg = IM_COL32(56, 44, 18, 220);
    else if (hovered)
        rowBg = IM_COL32(32, 28, 16, 210);

    if ((rowBg >> IM_COL32_A_SHIFT) != 0)
        draw->AddRectFilled(pos, max, rowBg, 0.0f);
    if (selected)
    {
        // Left gold accent bar + thin gold border for the active row.
        draw->AddRectFilled(pos, ImVec2(pos.x + 3.0f, max.y), IM_COL32(240, 210, 130, 240), 0.0f);
        draw->AddRect(pos, max, IM_COL32(212, 175, 95, 200), 0.0f, 0, 1.0f);
    }

    const float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
    const ImU32 textCol = selected ? IM_COL32(245, 222, 156, 255)
                                   : (hovered ? IM_COL32(232, 226, 210, 255)
                                              : IM_COL32(192, 200, 214, 240));
    draw->AddText(ImVec2(pos.x + style.FramePadding.x + 6.0f, textY), textCol, label);

    return pressed;
}

static float GetWindowButtonStripWidth()
{
    return WINDOW_BUTTON_SIZE * 3.0f + WINDOW_BUTTON_GAP * 2.0f;
}

static bool IsPointInWindowButtonStrip(HWND hwnd, POINT clientPt)
{
    RECT rc{};
    ::GetClientRect(hwnd, &rc);

    const float stripW = GetWindowButtonStripWidth();
    const int left = static_cast<int>(std::floor(static_cast<float>(rc.right) - stripW - 4.0f));
    const int bottom = static_cast<int>(std::ceil(WINDOW_BUTTON_SIZE + 2.0f));

    return clientPt.x >= left && clientPt.x < rc.right && clientPt.y >= rc.top && clientPt.y < bottom;
}

static bool DrawWindowButton(const char* id, WindowButtonIcon icon, const char* tooltip, bool danger = false)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(WINDOW_BUTTON_SIZE, WINDOW_BUTTON_SIZE);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImU32 bg = IM_COL32(0, 0, 0, 0);
    if (danger && (hovered || active))
        bg = active ? IM_COL32(196, 43, 28, 245) : IM_COL32(216, 54, 38, 230);
    else if (active)
        bg = IM_COL32(54, 54, 54, 220);
    else if (hovered)
        bg = IM_COL32(38, 38, 38, 210);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    if ((bg >> IM_COL32_A_SHIFT) != 0)
        draw->AddRectFilled(pos, max, bg, 0.0f);

    const ImU32 iconCol = (danger && hovered) ? IM_COL32(255, 255, 255, 255) : IM_COL32(226, 232, 242, 245);
    const float cx = pos.x + size.x * 0.5f;
    const float cy = pos.y + size.y * 0.5f;
    const float s = 8.0f;
    const float thickness = 1.4f;

    switch (icon)
    {
    case WindowButtonIcon::Minimize:
        draw->AddLine(ImVec2(cx - s * 0.55f, cy + s * 0.35f), ImVec2(cx + s * 0.55f, cy + s * 0.35f), iconCol, thickness);
        break;
    case WindowButtonIcon::Maximize:
        draw->AddRect(ImVec2(cx - s * 0.55f, cy - s * 0.55f), ImVec2(cx + s * 0.55f, cy + s * 0.55f), iconCol, 0.0f, 0, thickness);
        break;
    case WindowButtonIcon::Restore:
        draw->AddRect(ImVec2(cx - s * 0.30f, cy - s * 0.58f), ImVec2(cx + s * 0.62f, cy + s * 0.34f), iconCol, 0.0f, 0, thickness);
        draw->AddRectFilled(ImVec2(cx - s * 0.48f, cy - s * 0.40f), ImVec2(cx + s * 0.44f, cy + s * 0.52f), bg ? bg : IM_COL32(4, 4, 4, 248), 0.0f);
        draw->AddRect(ImVec2(cx - s * 0.48f, cy - s * 0.40f), ImVec2(cx + s * 0.44f, cy + s * 0.52f), iconCol, 0.0f, 0, thickness);
        break;
    case WindowButtonIcon::Close:
        draw->AddLine(ImVec2(cx - s * 0.50f, cy - s * 0.50f), ImVec2(cx + s * 0.50f, cy + s * 0.50f), iconCol, thickness);
        draw->AddLine(ImVec2(cx + s * 0.50f, cy - s * 0.50f), ImVec2(cx - s * 0.50f, cy + s * 0.50f), iconCol, thickness);
        break;
    }

    if (hovered)
        ImGui::SetTooltip("%s", tooltip);

    return pressed;
}

static void DrawWindowControls()
{
    if (!g_hwnd)
        return;

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - GetWindowButtonStripWidth() - 4.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(WINDOW_BUTTON_GAP, 0.0f));

    if (DrawWindowButton("##overlay_minimize", WindowButtonIcon::Minimize, u8"最小化"))
        ::ShowWindow(g_hwnd, SW_MINIMIZE);

    ImGui::SameLine();
    const bool maximized = ::IsZoomed(g_hwnd) != FALSE;
    if (DrawWindowButton("##overlay_maximize", maximized ? WindowButtonIcon::Restore : WindowButtonIcon::Maximize, maximized ? u8"还原" : u8"最大化"))
        ::ShowWindow(g_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);

    ImGui::SameLine();
    if (DrawWindowButton("##overlay_close", WindowButtonIcon::Close, u8"关闭", true))
        ::PostMessage(g_hwnd, WM_CLOSE, 0, 0);

    ImGui::PopStyleVar();
}

static UINT GetDpiForWindowSafe(HWND hwnd)
{
    UINT dpi = 96;
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto pGetDpiForWindow = (UINT(WINAPI*)(HWND))::GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow)
            dpi = pGetDpiForWindow(hwnd);
    }
    return dpi;
}

static RECT GetOverlayWorkArea(HWND hwnd)
{
    RECT work{};
    HMONITOR monitor = nullptr;

    if (hwnd)
    {
        monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    else
    {
        POINT pt{};
        ::GetCursorPos(&pt);
        monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    work.left = 0;
    work.top = 0;
    work.right = ::GetSystemMetrics(SM_CXSCREEN);
    work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    return work;
}

static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h)
{
    const RECT work = GetOverlayWorkArea(hwnd);
    const UINT dpi = hwnd ? GetDpiForWindowSafe(hwnd) : 96;

    const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
    const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);

    const int workW = OtherTools::MaxInt(1, static_cast<int>(work.right - work.left - WORKAREA_MARGIN_PX));
    const int workH = OtherTools::MaxInt(1, static_cast<int>(work.bottom - work.top - WORKAREA_MARGIN_PX));

    const int maxW = OtherTools::MaxInt(minW, workW);
    const int maxH = OtherTools::MaxInt(minH, workH);

    w = ClampInt(w, minW, maxW);
    h = ClampInt(h, minH, maxH);

    const int maxX = OtherTools::MaxInt(static_cast<int>(work.left), static_cast<int>(work.right - w));
    const int maxY = OtherTools::MaxInt(static_cast<int>(work.top), static_cast<int>(work.bottom - h));
    x = ClampInt(x, static_cast<int>(work.left), maxX);
    y = ClampInt(y, static_cast<int>(work.top), maxY);
}

static void EnsureOverlayInsideWorkArea(HWND hwnd)
{
    if (!hwnd)
        return;

    RECT wndRect{};
    ::GetWindowRect(hwnd, &wndRect);

    const int oldW = overlayWidth;
    const int oldH = overlayHeight;

    int x = wndRect.left;
    int y = wndRect.top;
    int w = overlayWidth;
    int h = overlayHeight;
    ClampOverlayToWorkArea(hwnd, x, y, w, h);

    overlayWidth = w;
    overlayHeight = h;

    if (x != wndRect.left || y != wndRect.top || w != oldW || h != oldH)
        ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER);
}

bool InitializeBlendState()
{
    D3D11_BLEND_DESC blendDesc;
    ZeroMemory(&blendDesc, sizeof(blendDesc));

    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = g_pd3dDevice->CreateBlendState(&blendDesc, &g_pBlendState);
    if (FAILED(hr))
        return false;

    float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    g_pd3dDeviceContext->OMSetBlendState(g_pBlendState, blendFactor, 0xffffffff);
    return true;
}

bool CreateDeviceD3D(HWND hWnd)
{
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        ARRAYSIZE(featureLevelArray),
        D3D11_SDK_VERSION,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);

    if (FAILED(hr))
        return false;

    IDXGIDevice* dxgiDev = nullptr;
    hr = g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr) || !dxgiDev)
        return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter)
    {
        dxgiDev->Release();
        return false;
    }

    IDXGIFactory2* factory2 = nullptr;
    {
        IDXGIFactory* baseFactory = nullptr;
        hr = adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        if (FAILED(hr) || !baseFactory)
        {
            adapter->Release();
            dxgiDev->Release();
            return false;
        }
        hr = baseFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        baseFactory->Release();
    }

    if (FAILED(hr) || !factory2)
    {
        adapter->Release();
        dxgiDev->Release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = overlayWidth;
    scd.Height = overlayHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    hr = factory2->CreateSwapChainForComposition(
        g_pd3dDevice,
        &scd,
        nullptr,
        &g_pSwapChain);

    factory2->Release();
    adapter->Release();

    if (FAILED(hr) || !g_pSwapChain)
    {
        dxgiDev->Release();
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDev->Release();
    if (FAILED(hr) || !g_dcompDevice)
        return false;

    hr = g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    if (FAILED(hr) || !g_dcompTarget)
        return false;

    hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
    if (FAILED(hr) || !g_dcompVisual)
        return false;

    hr = g_dcompVisual->SetContent(g_pSwapChain);
    if (FAILED(hr))
        return false;

    hr = g_dcompTarget->SetRoot(g_dcompVisual);
    if (FAILED(hr))
        return false;

    g_dcompDevice->Commit();

    if (!InitializeBlendState())
        return false;

    CreateRenderTarget();
    return true;
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = NULL;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = NULL; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = NULL; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = NULL; }

    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_NCHITTEST:
        {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);

            RECT rc;
            ::GetClientRect(hWnd, &rc);

            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int border = ::MulDiv(RESIZE_BORDER_PX, (int)dpi, 96);
            const bool left = pt.x < rc.left + border;
            const bool right = pt.x >= rc.right - border;
            const bool top = pt.y < rc.top + border;
            const bool bottom = pt.y >= rc.bottom - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            if (IsPointInWindowButtonStrip(hWnd, pt))
                return HTCLIENT;

            if (pt.y >= rc.top && pt.y < rc.top + DRAG_BAR_HEIGHT_PX)
                return HTCAPTION;

            return HTCLIENT;
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
            const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);
            const RECT work = GetOverlayWorkArea(hWnd);
            const int maxW = OtherTools::MaxInt(minW, static_cast<int>((work.right - work.left) - WORKAREA_MARGIN_PX));
            const int maxH = OtherTools::MaxInt(minH, static_cast<int>((work.bottom - work.top) - WORKAREA_MARGIN_PX));
            mmi->ptMinTrackSize.x = minW;
            mmi->ptMinTrackSize.y = minH;
            if (maxW > 0) mmi->ptMaxTrackSize.x = maxW;
            if (maxH > 0) mmi->ptMaxTrackSize.y = maxH;
            return 0;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_EXITSIZEMOVE:
        g_autoResizeEnabled = false;
        EnsureOverlayInsideWorkArea(hWnd);
        return 0;

    case WM_DISPLAYCHANGE:
        EnsureOverlayInsideWorkArea(hWnd);
        return 0;

    case WM_DPICHANGED:
        EnsureOverlayInsideWorkArea(hWnd);
        return 0;

    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
        {
            const UINT width = (UINT)LOWORD(lParam);
            const UINT height = (UINT)HIWORD(lParam);

            overlayWidth = (int)width;
            overlayHeight = (int)height;

            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            if (g_dcompDevice) g_dcompDevice->Commit();
        }
        return 0;

    case WM_DESTROY:
        shouldExit = true;
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void SetupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.0f;
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;

    // Chinese UI: prefer Microsoft YaHei / YaHei UI (bundled with Windows) and
    // explicitly load the Simplified Chinese glyph ranges so both CJK text and
    // basic Latin render from a single font atlas. Falls back gracefully.
    const ImWchar* zhRanges = io.Fonts->GetGlyphRangesChineseFull();
    const char* fontCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\msyhl.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    };
    ImFont* loadedFont = nullptr;
    for (const char* path : fontCandidates)
    {
        loadedFont = io.Fonts->AddFontFromFileTTF(path, 15.0f, &fontConfig, zhRanges);
        if (loadedFont) break;
    }
    if (!loadedFont)
    {
        io.Fonts->AddFontDefault();
    }

    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ApplyTheme_RoseDark();
    g_baseStyle = ImGui::GetStyle();
    g_baseStyleReady = true;
    g_runtimeUiScale = -1.0f;
}

bool CreateOverlayWindow()
{
    overlayWidth = BASE_OVERLAY_WIDTH;
    overlayHeight = BASE_OVERLAY_HEIGHT;

    {
        int x = 0;
        int y = 0;
        int w = overlayWidth;
        int h = overlayHeight;
        ClampOverlayToWorkArea(nullptr, x, y, w, h);
        overlayWidth = w;
        overlayHeight = h;
    }

    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(NULL),
        NULL,
        NULL,
        NULL,
        NULL,
        _T("Chrome"),
        NULL
    };
    ::RegisterClassEx(&wc);

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_LAYERED;
    const DWORD style = WS_POPUP;

    RECT wr = { 0, 0, overlayWidth, overlayHeight };
    ::AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    const int wndW = wr.right - wr.left;
    const int wndH = wr.bottom - wr.top;

    g_hwnd = ::CreateWindowEx(
        exStyle,
        wc.lpszClassName, _T("Apotheosis 控制台"),
        style,
        0, 0, wndW, wndH,
        NULL, NULL, wc.hInstance, NULL);

    if (g_hwnd == NULL)
        return false;

    EnsureOverlayInsideWorkArea(g_hwnd);

    BOOL dwm = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&dwm)) && dwm)
    {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }

    if (config.overlay_opacity < MIN_EDITOR_OPACITY)  config.overlay_opacity = MIN_EDITOR_OPACITY;
    if (config.overlay_opacity >= 256) config.overlay_opacity = 255;

    Overlay_SetOpacity(config.overlay_opacity);

    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    return true;
}

void OverlayThread()
{
    if (!CreateOverlayWindow())
    {
        std::cout << "[Overlay] Can't create overlay window!" << std::endl;
        return;
    }

    SetupImGui();

    LoadOverlayLogo();
    ApplyOverlayWindowIcon();

    // Launcher semantics: show the window immediately on process start so the
    // user can pick backend/model and click Start. The window stays visible
    // for the whole session 閳?there is no toggle hotkey anymore.
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);

    for (const auto& pair : KeyCodes::key_code_map)
        key_names.push_back(pair.first);

    std::sort(key_names.begin(), key_names.end());
    key_names_cstrs.reserve(key_names.size());
    for (const auto& name : key_names)
        key_names_cstrs.push_back(name.c_str());

    availableModels = getAvailableModels();

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (!shouldExit)
    {
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                shouldExit = true;
                break;
            }
        }
        if (shouldExit) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const float w = (float)overlayWidth;
        const float h = (float)overlayHeight;
        ApplyRuntimeUiScale(w, h);
        const float sidebarWidth = std::clamp(w * 0.23f, w * 0.18f, w * 0.30f);

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::Begin("##editor_root", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        DrawMainPanelBackground(ImGui::GetWindowPos(), ImGui::GetWindowSize());
        DrawWindowControls();
        ImGui::SetCursorPos(ImVec2(0.0f, static_cast<float>(DRAG_BAR_HEIGHT_PX)));

        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);

            static int activeTab = 0;
            const int tabCount = (int)(sizeof(kOverlayTabs) / sizeof(kOverlayTabs[0]));
            if (activeTab < 0 || activeTab >= tabCount)
                activeTab = 0;

            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(11, 11, 11, 245));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 56));
            ImGui::BeginChild("##options_nav", ImVec2(sidebarWidth, h - static_cast<float>(DRAG_BAR_HEIGHT_PX)), true,
                ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_AlwaysVerticalScrollbar);

            DrawSidebarBrand(ImGui::GetContentRegionAvail().x);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            const char* lastGroup = nullptr;
            for (int i = 0; i < tabCount; ++i)
            {
                const char* group = kOverlayTabs[i].group;
                if (!lastGroup || std::strcmp(lastGroup, group) != 0)
                {
                    if (lastGroup)
                        ImGui::Dummy(ImVec2(0.0f, 2.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(212, 175, 95, 235));
                    ImGui::TextUnformatted(group);
                    ImGui::PopStyleColor();
                }
                if (DrawSidebarTabButton(kOverlayTabs[i].label, activeTab == i))
                    activeTab = i;
                lastGroup = group;
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            ImGui::SameLine(0.0f, 6.0f);

            float contentExtraW = 0.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 12, 12, 245));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 56));
            ImGui::BeginChild("##options_content", ImVec2(0.0f, h - static_cast<float>(DRAG_BAR_HEIGHT_PX)), true,
                ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_AlwaysVerticalScrollbar);

            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 210, 130, 255));
            ImGui::TextUnformatted(kOverlayTabs[activeTab].label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(143, 160, 182, 255));
            ImGui::TextWrapped("%s", kOverlayTabs[activeTab].description);
            ImGui::PopStyleColor();
            ImGui::Separator();

            kOverlayTabs[activeTab].draw();

            const float overflowX = ImGui::GetScrollMaxX();
            contentExtraW = overflowX;
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            TryAutoResizeOverlay(contentExtraW);

            OverlayConfig_TrySave();
        }

        ImGui::End();
        ImGui::Render();

        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT result = g_pSwapChain->Present(0, 0);
        if (result == DXGI_STATUS_OCCLUDED || result == DXGI_ERROR_ACCESS_LOST)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    OverlayImage_Release(g_logo);
    if (g_logoIconBig) { ::DestroyIcon(g_logoIconBig); g_logoIconBig = NULL; }
    if (g_logoIconSmall) { ::DestroyIcon(g_logoIconSmall); g_logoIconSmall = NULL; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClass(_T("Chrome"), GetModuleHandle(NULL));
}
