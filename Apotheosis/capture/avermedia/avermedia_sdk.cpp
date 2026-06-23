#include "avermedia_sdk.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iostream>

namespace avermedia {

namespace {

// 按优先级试以下 DLL。Pro 是较新的合并版,普通版本是老 SDK;_64 是某些版本的
// x64 后缀命名。任意一个加载成功即可用。
constexpr const char* kDllCandidates[] = {
    "AVerCapAPIPro.dll",
    "AVerCapAPI.dll",
    "AVerCapAPI64.dll",
    "AVerCapAPI_x64.dll",
};

// 把字符串转小写并返回拷贝(ASCII)。
std::string ToLower(const std::string& s)
{
    std::string out;
    out.resize(s.size());
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <typename T>
bool Resolve(HMODULE h, const char* name, T& outPtr)
{
    FARPROC p = GetProcAddress(h, name);
    outPtr = reinterpret_cast<T>(p);
    return p != nullptr;
}

} // namespace

SdkLoader& SdkLoader::Instance()
{
    static SdkLoader g;
    return g;
}

SdkLoader::SdkLoader()
{
    TryLoad();
}

SdkLoader::~SdkLoader()
{
    if (handle_)
    {
        FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

void SdkLoader::TryLoad()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (loaded_)
        return;

    for (const char* dll : kDllCandidates)
    {
        HMODULE h = LoadLibraryA(dll);
        if (!h)
            continue;

        handle_ = h;
        char fullPath[MAX_PATH] = { 0 };
        GetModuleFileNameA(h, fullPath, MAX_PATH);
        dll_path_ = fullPath;

        if (ResolveAll())
        {
            loaded_ = true;
            std::cout << "[AVerMedia] SDK loaded: " << dll_path_ << std::endl;
            return;
        }

        // 关键函数没解析全 — 当作未加载,卸掉。
        std::cerr << "[AVerMedia] " << dll
                  << " loaded but required exports missing; treating as absent."
                  << std::endl;
        api_ = {};
        FreeLibrary(h);
        handle_ = nullptr;
        dll_path_.clear();
    }
    // 没找到任何 DLL — 静默,这是合法场景(没接卡 / 没装驱动)。
}

bool SdkLoader::ResolveAll()
{
    HMODULE h = static_cast<HMODULE>(handle_);
    if (!h)
        return false;

    // 关键 6 个: 没有就不可用。
    bool ok = true;
    ok &= Resolve(h, "AVerCaptureGetSDKVersion",     api_.GetSDKVersion);
    ok &= Resolve(h, "AVerCaptureGetDeviceNum",      api_.GetDeviceNum);
    ok &= Resolve(h, "AVerCaptureCreate",            api_.CreateEngine);
    ok &= Resolve(h, "AVerCaptureRelease",           api_.ReleaseEngine);
    ok &= Resolve(h, "AVerCaptureStart",             api_.StartStreaming);
    ok &= Resolve(h, "AVerCaptureStop",              api_.StopStreaming);

    // 可选: 缺了不致命,运行时按可用性走分支。
    Resolve(h, "AVerSetVideoSource",         api_.SetVideoSource);
    Resolve(h, "AVerGetVideoSource",         api_.GetVideoSource);
    Resolve(h, "AVerGetSignalPresence",      api_.GetSignalPresence);
    Resolve(h, "AVerGetVideoInfo",           api_.GetVideoInfo);
    Resolve(h, "AVerSetVideoResolution",     api_.SetVideoResolution);
    Resolve(h, "AVerSetVideoInputFrameRate", api_.SetVideoInputFrameRate);
    Resolve(h, "AVerSetVideoPixelFormat",    api_.SetVideoPixelFormat);
    Resolve(h, "AVerCaptureGetDeviceFriendlyName", api_.GetDeviceFriendlyName);

    return ok;
}

bool SdkLoader::IsUsable() const
{
    if (!loaded_)
        return false;
    return api_.CreateEngine && api_.ReleaseEngine
        && api_.StartStreaming && api_.StopStreaming
        && api_.GetDeviceNum;
}

bool IsAverMediaFriendlyName(const std::string& name)
{
    static const std::array<const char*, 9> kKeys = {
        "avermedia",
        "live gamer",
        "extremecap",
        "darkcrystal",
        "gc5",      // GC553 / GC551 / GC575 系列(USB)
        "gc7",      // GC715 / GC753 系列(PCIe)
        "bu1",      // BU110 / BU113 等
        "c727",     // C727 PCIe
        "c875",     // C875 / C729 等专业卡
    };
    const std::string lower = ToLower(name);
    for (const char* k : kKeys)
    {
        if (lower.find(k) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace avermedia
