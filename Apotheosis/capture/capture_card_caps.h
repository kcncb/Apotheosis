#pragma once

// =============================================================================
// 采集卡真实能力表 (纯数据 + 纯逻辑, 无 Windows / CUDA / OpenCV 依赖)
// =============================================================================
//
// 单独成文件的原因有两个:
//   1) Qt UI 只需要这张表, 不应该被迫拖进 CUDA / TensorRT 头文件;
//   2) 三级联动与选优逻辑是纯函数, 因此可以脱离 Windows 单独回归测试
//      (见 capture/tests/capture_card_caps_test.cpp)。
//
// 设计原则: 【只如实反映设备能力, 绝不"帮设备升级"】。
//
// 设备支持什么就列什么。UI 的 像素格式 / 分辨率 / 帧率 三级下拉全部由这张表
// 驱动, "联动"的含义是:
//
//   选了 NV12   -> 分辨率下拉只列 NV12 真实支持的那些
//   选了 1080p  -> 帧率下拉只列该 (格式,分辨率) 下真实支持的帧率
//
// 例: 卡支持 MJPG@1080p240 但不支持 NV12@1080p240, 那么用户选 NV12 时,
//     帧率下拉里【根本不会出现 240】—— 而不是让他选完再悄悄降级、或者黑屏。
//
// 这正是旧的"让用户手填宽/高/fps"做法必须废掉的原因: 手填的值设备做不到时,
// 后端只能静默换 mode, 表现为帧率掉、延迟飙、分辨率不对, 且无从解释。

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct MFCapability
{
    std::string      format;   // "NV12" | "MJPG" | "YUY2" | "RGB32" | 其它原始格式名
    int              width  = 0;
    int              height = 0;
    std::vector<int> fps;      // 该 (format,width,height) 下真实支持的帧率, 升序去重

    // 本程序是否真的能解码该像素格式。设备可能报出 H264 / P010 等我们不吃的
    // 格式 —— 探测时一并列出 (诚实反映设备能力), 但 UI 默认不展示。
    bool supported = false;
};

struct MFDeviceInfo
{
    int                       index = 0;
    std::string               name;
    std::string               friendly_name;   // 设备自报原始名, 用于配置持久化
    std::vector<MFCapability> caps;            // ProbeCapabilities() 填充
    bool                      caps_probed = false;
};

namespace mfcap
{

// 未压缩格式的"延迟代价"排序 —— 越小越优先。
//   NV12  1 : 4:2:0, 1.5B/px, 无解码, 喂 GPU 最省
//   YUY2  2 : 4:2:2, 2B/px,   无解码, 需一次 cvtColor
//   RGB32 3 : 4B/px,          无解码, 带宽最大
//   MJPG  9 : 卡内编码 + 进程内解码, 多一次往返, 延迟最差
inline int FormatLatencyRank(const std::string& format, bool supported)
{
    if (!supported)        return 100;
    if (format == "NV12")  return 1;
    if (format == "YUY2")  return 2;
    if (format == "RGB32") return 3;
    if (format == "MJPG")  return 9;
    return 50;
}

// 该设备真实支持的像素格式, 按"延迟从低到高"排序 (未压缩在前)。
inline std::vector<std::string> Formats(const MFDeviceInfo& dev,
                                        bool supported_only = true)
{
    std::vector<std::string> out;
    for (const auto& c : dev.caps)
    {
        if (supported_only && !c.supported) continue;
        if (std::find(out.begin(), out.end(), c.format) == out.end())
            out.push_back(c.format);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) {
                         return FormatLatencyRank(a, true) < FormatLatencyRank(b, true);
                     });
    return out;
}

// 在指定格式下真实支持的分辨率, 按像素数降序 (大在前)。
inline std::vector<std::pair<int, int>> Resolutions(const MFDeviceInfo& dev,
                                                    const std::string& format)
{
    std::vector<std::pair<int, int>> out;
    for (const auto& c : dev.caps)
    {
        if (c.format != format) continue;
        const std::pair<int, int> wh{ c.width, c.height };
        if (std::find(out.begin(), out.end(), wh) == out.end())
            out.push_back(wh);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return static_cast<long long>(a.first) * a.second
             > static_cast<long long>(b.first) * b.second;
    });
    return out;
}

// 在指定 格式+分辨率 下真实支持的帧率, 升序。
inline std::vector<int> FpsList(const MFDeviceInfo& dev, const std::string& format,
                                int width, int height)
{
    for (const auto& c : dev.caps)
        if (c.format == format && c.width == width && c.height == height)
            return c.fps;   // 探测时已升序去重
    return {};
}

// 该组合是否被设备真实支持 (帧率允许 ±1 容差, 设备常报 29.97 / 59.94)。
inline bool Supports(const MFDeviceInfo& dev, const std::string& format,
                     int width, int height, int fps)
{
    for (const auto& c : dev.caps)
    {
        if (c.format != format || c.width != width || c.height != height)
            continue;
        if (fps <= 0) return true;
        for (int f : c.fps)
            if (std::abs(f - fps) <= 1) return true;
        return false;
    }
    return false;
}

// 校验一组配置是否被设备真实支持。
//
// 这里【刻意不提供"吸附到最近可用组合"的函数】。采集卡路径不接受任何回退:
// 配置与设备能力对不上时, 由 UI 直接把无效项从下拉里去掉, 让用户重新选;
// 绝不在后台悄悄换成"差不多的"模式 —— 那样用户以为在用 1080p240, 实际
// 可能跑在 720p60, 而手上只有"手感不对"这一条线索。
inline bool Validate(const MFDeviceInfo& dev, const std::string& format,
                     int width, int height, int fps)
{
    // 先确认本程序真的能解码该格式 —— 设备支持 H264 / P010 不代表我们吃不下。
    // 这类格式在能力表里如实保留(supported=false), 但绝不能通过校验。
    bool decodable = false;
    for (const auto& c : dev.caps)
        if (c.format == format) { decodable = c.supported; break; }
    if (!decodable) return false;

    return Supports(dev, format, width, height, fps);
}

// 在设备真实能力范围内挑最优组合。
//
// 排序原则: 【分辨率是硬约束, 帧率是偏好, 格式是最后的偏好】。
//
//   1. 精确分辨率匹配 —— 分辨率是用户(或模型)选定的, 绝不能为了凑一个未压缩
//      格式或更高帧率就悄悄换成别的尺寸 (那正是"分辨率不对"的来源)
//   2. 帧率达标 —— 控制环频率 = 检测帧率
//   3. 帧率差距最小 —— 都打不到目标时, 尽量做高, 而不是退而求其次
//   4. 未压缩格式优先: NV12 > YUY2 > RGB32 > MJPG (压缩多一次编解码往返)
//   5. 分辨率最接近、帧率最高
//
// 例: 目标 1920x1080@120, 设备有 NV12@720p120 与 MJPG@1080p120 ——
//     必须选 MJPG@1080p, 而不是 NV12@720p。分辨率必须守住。
//
// out_reason 返回人类可读的选择理由, 直接展示在 UI 上 —— 让用户知道
// "为什么给我选了这个", 而不是对着一堆数字猜。
inline bool PickBest(const MFDeviceInfo& dev, int want_w, int want_h, int want_fps,
                     std::string& format, int& width, int& height, int& fps,
                     std::string* out_reason = nullptr)
{
    struct Candidate { std::string format; int width, height, fps; };
    std::vector<Candidate> cands;
    for (const auto& c : dev.caps)
    {
        if (!c.supported || c.fps.empty()) continue;
        for (int f : c.fps)
            cands.push_back({ c.format, c.width, c.height, f });
    }
    if (cands.empty()) return false;

    const auto score = [&](const Candidate& c) {
        const int       fps_ok  = (want_fps > 0 && c.fps >= want_fps) ? 0 : 1;
        const int       fps_gap = (want_fps > 0) ? std::max(0, want_fps - c.fps) : 0;
        // 分辨率精确匹配优先于格式偏好。少了这一项, 1080p120 会被"更省事的"
        // NV12@720p120 抢走 —— 帧率一样, 但画面尺寸被换掉了。
        const int       exact   = (want_w > 0 && want_h > 0
                                   && c.width == want_w && c.height == want_h) ? 0 : 1;
        const int       rank    = FormatLatencyRank(c.format, true);
        const long long px      = static_cast<long long>(c.width) * c.height;
        const long long dpx     = std::llabs(px - static_cast<long long>(want_w) * want_h);
        // fps_gap 必须排在 rank 之前: 当【没有任何格式】能打到目标帧率时
        // (fps_ok 全为 1), 应该尽量把帧率做高, 而不是为了省解码去选个 60fps。
        return std::make_tuple(exact, fps_ok, fps_gap, rank, dpx, -c.fps);
    };

    const Candidate* best = &cands.front();
    auto best_score = score(*best);
    for (const auto& c : cands)
    {
        auto s = score(c);
        if (s < best_score) { best_score = s; best = &c; }
    }

    format = best->format;
    width  = best->width;
    height = best->height;
    fps    = best->fps;

    if (out_reason)
    {
        std::ostringstream os;
        os << "设备能力内选优: " << format << " " << width << "x" << height
           << "@" << fps;
        // 任何"没按你要求来"的地方都必须写出来, 不允许静默降级。
        if (want_w > 0 && want_h > 0 && (width != want_w || height != want_h))
            os << " (设备在目标帧率下没有 " << want_w << "x" << want_h
               << ", 最接近的是 " << width << "x" << height << ")";
        else if (want_fps > 0 && fps < want_fps)
            os << " (该组合下设备最高只能到 " << fps << "fps, 目标 "
               << want_fps << "fps 达不到)";
        else if (format == "MJPG")
            os << " (未压缩格式在目标分辨率/帧率下不可用, 改用 MJPG 换帧率)";
        else
            os << " (未压缩格式, 无编解码往返)";
        *out_reason = os.str();
    }
    return true;
}

// 便于 UI / 日志展示的一行摘要, 例:
//   "NV12 1280x720@60/120, MJPG 1920x1080@60/120/240"
inline std::string Describe(const MFDeviceInfo& dev)
{
    if (dev.caps.empty())
        return dev.caps_probed ? "(device reported no video capabilities)" : "(not probed)";

    std::ostringstream os;
    bool first = true;
    for (const auto& c : dev.caps)
    {
        if (!first) os << ", ";
        first = false;
        os << c.format << " " << c.width << "x" << c.height << "@";
        for (size_t i = 0; i < c.fps.size(); ++i)
        {
            if (i) os << "/";
            os << c.fps[i];
        }
        if (!c.supported) os << " (unsupported)";
    }
    return os.str();
}

} // namespace mfcap
