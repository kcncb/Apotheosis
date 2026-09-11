// =============================================================================
// 采集卡现场诊断 —— 回答"为什么采不到画面"
// =============================================================================
//
// 纯 Media Foundation, 不依赖 CUDA / OpenCV / TensorRT / Qt, 因此任何一台装了
// Windows SDK 的机器都能单独编译运行 (build/diag/build_probe.bat):
//
//   cl /std:c++20 /utf-8 /EHsc /O2 capture_card_dump.cpp ^
//      /link mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
//
//   capture_card_dump.exe [设备名] [格式] [宽] [高] [帧率]
//   默认: VC-009PRO NV12 1920 1080 120
//
// 它做三件事:
//   [A] 列出本机所有采集设备 (friendly name —— 配置里存的就是它)
//   [B] 列出目标设备【真实宣称】的 格式/分辨率/帧率 组合
//   [C] 完全按主程序的方式走一遍: 建 reader(禁用 converter) -> 严格协商 ->
//       读一帧。每一步失败都打印真实 HRESULT 和可操作的原因。
//
// 为什么需要它: 主程序里设备协商发生在采集线程内部, 失败只写一行 stderr; 而
// 采集线程退出后 captureThread 仍持有非空 capturer, 于是表现为"界面正常但永远
// 没有画面"。现场光看界面无法区分: 设备不支持 / 被别的程序独占 / 无 HDMI 信号 /
// 分辨率刷新率超出带宽。这个工具把每一层都摊开。
//
// 注意: 一个 IMFMediaSource 同时只能挂一个 source reader, 所以本工具自始至终
// 只用【一个】reader 走完全程 —— 与主程序 MFCapture::ReceiveThread 一致。
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
        return std::string();
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return std::string();
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    return result;
}

std::string Hex(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

const char* SubtypeName(REFGUID sub)
{
    if (sub == MFVideoFormat_NV12)   return "NV12";
    if (sub == MFVideoFormat_MJPG)   return "MJPG";
    if (sub == MFVideoFormat_YUY2)   return "YUY2";
    if (sub == MFVideoFormat_RGB32)  return "RGB32";
    if (sub == MFVideoFormat_ARGB32) return "ARGB32";
    if (sub == MFVideoFormat_H264)   return "H264";
    if (sub == MFVideoFormat_P010)   return "P010";
    if (sub == MFVideoFormat_I420)   return "I420";
    if (sub == MFVideoFormat_YV12)   return "YV12";
    if (sub == MFVideoFormat_UYVY)   return "UYVY";
    return "OTHER";
}

// 只解释我们在采集路径上真会撞到的几个错误码, 其余原样输出。
std::string ExplainHr(HRESULT hr)
{
    switch (static_cast<unsigned long>(hr))
    {
    case 0xC00D3E85: return "MF_E_SHUTDOWN (设备对象已被关闭; 通常是被别的程序/上一次会话占用)";
    case 0xC00D36B4: return "MF_E_INVALIDMEDIATYPE (设备拒绝该媒体类型)";
    case 0xC00D36B9: return "MF_E_NO_MORE_TYPES (没有更多媒体类型)";
    case 0xC00D3704: return "MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED (设备被拔出/重置)";
    case 0xC00D3EA1: return "MF_E_UNSUPPORTED_BYTESTREAM_TYPE";
    case 0x80070005: return "E_ACCESSDENIED (设备被独占, 典型的是另一个程序正在用它)";
    case 0x8007001F: return "ERROR_GEN_FAILURE (设备级故障)";
    default:         return "";
    }
}

void PrintHr(const char* what, HRESULT hr)
{
    const std::string extra = ExplainHr(hr);
    printf("      [FAIL] %s (hr=%s)%s%s\n", what, Hex(hr).c_str(),
           extra.empty() ? "" : " ", extra.c_str());
}

struct Cap
{
    std::string          format;
    int                  w = 0;
    int                  h = 0;
    double               fps = 0.0;
    GUID                 subtype{};
    ComPtr<IMFMediaType> type;
};

double ReadFps(IMFMediaType* t)
{
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) && den != 0 && num != 0)
        return static_cast<double>(num) / static_cast<double>(den);
    if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MAX, &num, &den)) && den != 0 && num != 0)
        return static_cast<double>(num) / static_cast<double>(den);
    return 0.0;
}

// 与 MFCapture 一致: 从 0 枚举原生媒体类型直到 MF_E_NO_MORE_TYPES。
std::vector<Cap> EnumerateCaps(IMFSourceReader* reader)
{
    std::vector<Cap> caps;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !t)
            break;

        GUID sub{};
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)))
            continue;

        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
            continue;

        Cap c;
        c.format  = SubtypeName(sub);
        c.w       = static_cast<int>(w);
        c.h       = static_cast<int>(h);
        c.fps     = ReadFps(t.Get());
        c.subtype = sub;
        c.type    = t;
        caps.push_back(std::move(c));
    }
    return caps;
}

void PrintCaps(const std::vector<Cap>& caps)
{
    std::vector<std::string> formatOrder;
    std::map<std::string, std::map<std::pair<int,int>, std::vector<double>>> tree;

    for (const auto& c : caps)
    {
        if (std::find(formatOrder.begin(), formatOrder.end(), c.format) == formatOrder.end())
            formatOrder.push_back(c.format);
        auto& fpsList = tree[c.format][{ c.w, c.h }];
        if (std::find_if(fpsList.begin(), fpsList.end(),
                         [&](double f) { return std::abs(f - c.fps) < 0.5; }) == fpsList.end())
            fpsList.push_back(c.fps);
    }

    for (const auto& fmt : formatOrder)
    {
        bool firstRes = true;
        for (auto& [wh, fpsList] : tree[fmt])
        {
            std::sort(fpsList.begin(), fpsList.end());
            std::string fps;
            for (size_t i = 0; i < fpsList.size(); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.0f", fpsList[i]);
                if (i) fps += "/";
                fps += buf;
            }
            if (firstRes)
                printf("      %-6s : %dx%d @ %s\n", fmt.c_str(), wh.first, wh.second, fps.c_str());
            else
                printf("               %dx%d @ %s\n", wh.first, wh.second, fps.c_str());
            firstRes = false;
        }
    }
}

struct Device
{
    int                 index = 0;
    std::string         friendly;
    std::string         symbolic;
    ComPtr<IMFActivate> activate;
};

std::vector<Device> EnumerateDevices()
{
    std::vector<Device> out;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &activates, &count)) || count == 0)
        return out;

    for (UINT32 i = 0; i < count; ++i)
    {
        Device d;
        d.index = static_cast<int>(i);
        d.activate = activates[i];

        WCHAR* buf = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &buf, &len)) && buf)
        {
            d.friendly = WideToUtf8(buf);
            CoTaskMemFree(buf);
            buf = nullptr;
        }
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &buf, &len)) && buf)
        {
            d.symbolic = WideToUtf8(buf);
            CoTaskMemFree(buf);
        }
        out.push_back(std::move(d));
    }
    CoTaskMemFree(activates);
    return out;
}

// 严格协商 —— 与 MFCapture::SelectExactMediaType 同一套判定:
// 格式 / 分辨率 / 帧率(±1 容差) 三处必须同时命中, 任何一处不符都直接失败,
// 不做任何替换。
bool Negotiate(IMFSourceReader* reader,
               const std::vector<Cap>& caps,
               const std::string& wantFormat,
               int wantW, int wantH, int wantFps,
               std::string& outError)
{
    std::vector<const Cap*> sameFormat;
    std::vector<const Cap*> sameRes;
    const Cap* exact = nullptr;
    std::set<std::string> otherFormats;
    std::vector<std::pair<int,int>> allRes;

    for (const auto& c : caps)
    {
        if (c.format != wantFormat)
        {
            otherFormats.insert(c.format);
            continue;
        }
        const auto wh = std::make_pair(c.w, c.h);
        if (std::find(allRes.begin(), allRes.end(), wh) == allRes.end())
            allRes.push_back(wh);

        if (c.w == wantW && c.h == wantH)
        {
            if (std::abs(c.fps - static_cast<double>(wantFps)) <= 1.0 && !exact)
                exact = &c;
            sameRes.push_back(&c);
        }
        sameFormat.push_back(&c);
    }

    auto joinRes = [&] {
        std::string s;
        for (size_t i = 0; i < allRes.size(); ++i)
        {
            if (i) s += ", ";
            s += std::to_string(allRes[i].first) + "x" + std::to_string(allRes[i].second);
        }
        return s;
    };

    if (sameFormat.empty())
    {
        std::string others;
        for (const auto& f : otherFormats) { if (!others.empty()) others += ", "; others += f; }
        outError = "device does not offer the requested pixel format " + wantFormat
                 + (others.empty() ? "; it offers no usable pixel format at all"
                                   : "; it offers: " + others);
        return false;
    }

    if (sameRes.empty())
    {
        outError = "device does not offer " + std::to_string(wantW) + "x"
                 + std::to_string(wantH) + " in " + wantFormat
                 + "; that format supports: " + joinRes();
        return false;
    }

    if (!exact)
    {
        std::string fpsList;
        for (const auto* c : sameRes)
        {
            if (!fpsList.empty()) fpsList += ", ";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f", c->fps);
            fpsList += buf;
        }
        outError = "device does not support " + wantFormat + " "
                 + std::to_string(wantW) + "x" + std::to_string(wantH) + " @ "
                 + std::to_string(wantFps) + "fps; that combination supports fps: " + fpsList;
        return false;
    }

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(exact->type.Get(), MF_MT_FRAME_RATE, &num, &den)) &&
        den != 0 && num != 0)
        printf("      命中媒体类型: %s %dx%d @ %.4g fps\n",
               exact->format.c_str(), exact->w, exact->h,
               static_cast<double>(num) / den);

    const HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                   nullptr, exact->type.Get());
    if (FAILED(hr))
    {
        outError = "SetCurrentMediaType failed hr=" + Hex(hr);
        const std::string extra = ExplainHr(hr);
        if (!extra.empty())
            outError += " | " + extra;
        return false;
    }
    outError.clear();
    return true;
}

// 协商成功后确认设备真的报了帧几何 (主程序会在这里再挡一次)。
bool CheckGeometry(IMFSourceReader* reader, std::string& outError)
{
    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) || !current)
    {
        outError = "GetCurrentMediaType failed after negotiation";
        return false;
    }
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
    {
        outError = "device accepted the mode but reported no frame geometry";
        return false;
    }
    LONG stride = 0;
    if (FAILED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))))
        stride = 0;
    printf("      帧几何: %ux%u  默认 stride=%ld\n", w, h, stride);
    outError.clear();
    return true;
}

// 协商成功后持续拉流, 统计真实到达的帧率。
//
// 只读"一帧"是不够的: 有些故障是首帧能出来、随后就停 (带宽不足 / 信号丢失),
// 所以这里跑满整段窗口再给数字 —— 它同时验证了"能出画面"和"能持续出画面"。
bool MeasureStream(IMFSourceReader* reader, int durationMs, std::string& outError)
{
    const DWORD start = GetTickCount();
    int frames = 0;
    int nullSamples = 0;
    LONGLONG firstTs = 0;
    LONGLONG lastTs = 0;

    while (GetTickCount() - start < static_cast<DWORD>(durationMs))
    {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr))
        {
            outError = "ReadSample failed hr=" + Hex(hr);
            const std::string extra = ExplainHr(hr);
            if (!extra.empty())
                outError += " | " + extra;
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            outError = "stream reported end-of-stream (设备被拔出或没有信号)";
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
            outError = "device changed the negotiated media type mid-stream";
            return false;
        }
        if (sample)
        {
            if (frames == 0) firstTs = ts;
            lastTs = ts;
            ++frames;
        }
        else
        {
            ++nullSamples;
        }
    }

    if (frames == 0)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "%dms 内一帧都没有 (设备已打开但从不投递数据; %d 次空样本)",
            durationMs, nullSamples);
        outError = buf;
        return false;
    }

    // 优先用设备时间戳算真实帧率; 时间戳不可用时退回到墙钟。
    double fps = 0.0;
    if (lastTs > firstTs && frames > 1)
    {
        const double spanSec = static_cast<double>(lastTs - firstTs) / 1.0e7;
        if (spanSec > 0.0)
            fps = static_cast<double>(frames - 1) / spanSec;
    }
    if (fps <= 0.0)
        fps = static_cast<double>(frames) * 1000.0 / static_cast<double>(durationMs);

    printf("      [ ok ] %dms 内收到 %d 帧 -> 实测 %.1f fps (空样本 %d)\n",
           durationMs, frames, fps, nullSamples);
    outError.clear();
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string wantDevice = (argc > 1) ? argv[1] : "VC-009PRO";
    const std::string wantFormat = (argc > 2) ? argv[2] : "NV12";
    const int wantW   = (argc > 3) ? std::atoi(argv[3]) : 1920;
    const int wantH   = (argc > 4) ? std::atoi(argv[4]) : 1080;
    const int wantFps = (argc > 5) ? std::atoi(argv[5]) : 120;

    SetConsoleOutputCP(CP_UTF8);

    printf("=== 采集卡现场诊断 ===\n");
    printf("目标组合: 设备=\"%s\"  %s %dx%d @ %dfps\n\n",
           wantDevice.c_str(), wantFormat.c_str(), wantW, wantH, wantFps);

    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        printf("[FAIL] MFStartup 失败 —— Media Foundation 运行时不可用。\n");
        return 2;
    }

    int exitCode = 0;
    ComPtr<IMFMediaSource>  source;
    ComPtr<IMFSourceReader> reader;

    // ---------------------------------------------------------------- [A] 枚举
    printf("[A] 本机视频采集设备\n");
    const auto devices = EnumerateDevices();
    if (devices.empty())
    {
        printf("      (未发现任何采集设备)\n");
        printf("\n结论: 系统层面就没有采集卡。检查: USB 连接 / 驱动 / 设备管理器。\n");
        MFShutdown();
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 3;
    }
    for (const auto& d : devices)
    {
        printf("      [%d] %s\n", d.index, d.friendly.c_str());
        if (!d.symbolic.empty())
            printf("          %s\n", d.symbolic.c_str());
    }
    printf("\n");

    const Device* target = nullptr;
    for (const auto& d : devices)
        if (d.friendly == wantDevice) { target = &d; break; }

    if (!target)
    {
        printf("[FAIL] 配置里的设备名 \"%s\" 与上面任何一台都对不上。\n", wantDevice.c_str());
        printf("       主程序按 friendly name 精确匹配, 名字不一致会直接放弃且【不会】换卡。\n");
        printf("       修正办法: 打开采集设置页重新选一次设备。\n");
        MFShutdown();
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 4;
    }

    // -------------------------------------------------- [B] 激活 + 建 reader
    printf("[B] 激活设备并建立 reader (与主程序一致: 禁用 converter)\n");

    const HRESULT actHr = target->activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(actHr))
    {
        PrintHr("ActivateObject (设备无法激活)", actHr);
        printf("\n>>>> 最常见原因: 采集卡正被另一个程序独占 (主程序自己没退干净 / OBS /\n");
        printf("     相机应用 / 浏览器页面)。把所有可能用到摄像头的程序关掉再试。\n");
        MFShutdown();
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 5;
    }

    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 2);
    if (readerAttrs)
    {
        readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    }

    HRESULT readerHr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
    if (FAILED(readerHr))
    {
        PrintHr("MFCreateSourceReaderFromMediaSource (禁用 converter)", readerHr);

        // 回退对照: 允许 converter 时能否建起来 —— 用来区分"设备本身打不开"和
        // "只是不接受禁用 converter"这两种完全不同的故障。
        ComPtr<IMFSourceReader> loose;
        const HRESULT looseHr =
            MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &loose);
        if (SUCCEEDED(looseHr))
            printf("      [注意] 允许 converter 时 reader 可以建立 —— 说明问题出在\n"
                   "             禁用 converter 的路径上, 而不是设备打不开。\n");
        else
            PrintHr("即使允许 converter 也建不起来", looseHr);

        MFShutdown();
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 6;
    }

    // ------------------------------------------------------------ [C] 能力表
    printf("      [ ok ] reader 已建立\n\n");
    printf("[C] \"%s\" 真实宣称的能力\n", target->friendly.c_str());

    const auto caps = EnumerateCaps(reader.Get());
    if (caps.empty())
        printf("      (设备没有上报任何视频能力 —— 通常是没有信号 / EDID 未读到)\n");
    else
        PrintCaps(caps);
    printf("\n");

    // ------------------------------------------------------ [D] 协商 + 读帧
    printf("[D] 用主程序的方式严格协商 + 试读一帧\n");

    std::string why;
    if (!Negotiate(reader.Get(), caps, wantFormat, wantW, wantH, wantFps, why))
    {
        printf("      [FAIL] 协商失败。\n");
        printf("             原因: %s\n", why.c_str());
        printf("\n>>>> 这就是主程序采不到画面的原因。\n");
        printf("     主程序不会自动替换格式/分辨率/帧率, 必须改成上面 [C] 里真实存在的组合。\n");
        exitCode = 8;
    }
    else
    {
        printf("      [ ok ] 协商通过: %s %dx%d @ %dfps\n",
               wantFormat.c_str(), wantW, wantH, wantFps);

        std::string geoErr;
        if (!CheckGeometry(reader.Get(), geoErr))
            printf("      [warn] %s\n", geoErr.c_str());

        std::string readErr;
        if (MeasureStream(reader.Get(), 3000, readErr))
        {
            printf("      [ ok ] 采集链路是通的, 且能持续出帧。\n");
        }
        else
        {
            printf("      [FAIL] 协商通过但取不到帧: %s\n", readErr.c_str());
            printf("\n>>>> 设备已打开但没有数据。常见原因:\n");
            printf("     * 采集卡被其它程序独占\n");
            printf("     * HDMI 没有信号 / 源设备没输出 / 输入口选错\n");
            printf("     * 分辨率+刷新率超出线材或卡的 USB 带宽 (1080p120 NV12 尤其吃带宽)\n");
            printf("     * HDCP 保护\n");
            exitCode = 9;
        }
    }

    reader.Reset();
    source.Reset();
    MFShutdown();
    if (SUCCEEDED(coInit))
        CoUninitialize();

    printf("\n=== 诊断结束 (exit=%d) ===\n", exitCode);
    return exitCode;
}
