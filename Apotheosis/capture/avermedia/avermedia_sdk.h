#ifndef APOTHEOSIS_AVERMEDIA_SDK_H
#define APOTHEOSIS_AVERMEDIA_SDK_H

// 圆刚 (AVerMedia) 采集卡 SDK 的运行时动态加载层。
//
// 设计目标:
//   - 编译期零依赖: SDK 头文件 / .lib 不在仓库时仍能编过。
//   - 运行期可选: 若 exe 同目录(或 PATH)有 AVerCapAPIPro.dll / AVerCapAPI.dll /
//     AVerCapAPI64.dll,自动 LoadLibrary 并解析函数指针;否则 IsLoaded()=false,
//     capture.cpp 自动退化到原 OpenCV / Media Foundation 路径。
//   - 即使 SDK 在场但解析失败的单个函数指针保持 nullptr,调用方按可用性使用。
//
// 函数原型按 AVerMedia 公开 SDK 头文件 (AVerCapAPI.h / AVerCapAPIPro.h) 的惯例
// 给出 __stdcall 签名。设备句柄类型在 SDK 里是 HANDLE;我们透传 void*。

#include <cstdint>
#include <mutex>
#include <string>

namespace avermedia {

using DeviceHandle = void*;

// AVerMedia SDK 错误码(节选,与 SDK 头文件一致)。
enum AverErr : uint32_t {
    AVER_ERR_SUCCESS         = 0,
    AVER_ERR_FAIL            = 1,
    AVER_ERR_INVALID_PARAM   = 2,
    AVER_ERR_NOT_SUPPORTED   = 3,
    AVER_ERR_DEVICE_IN_USE   = 4,
    AVER_ERR_NO_DEVICE       = 5,
};

// 视频源(HDMI / DVI / VGA / Component / Composite / S-Video / SDI)。
enum AverVideoSource : uint32_t {
    AVER_SRC_HDMI       = 0,
    AVER_SRC_DVI        = 1,
    AVER_SRC_VGA        = 2,
    AVER_SRC_COMPONENT  = 3,
    AVER_SRC_COMPOSITE  = 4,
    AVER_SRC_SVIDEO     = 5,
    AVER_SRC_SDI        = 6,
};

// 像素格式(SDK 内部枚举,与 FOURCC 不同)。
enum AverPixelFormat : uint32_t {
    AVER_PIX_YUY2  = 0,
    AVER_PIX_UYVY  = 1,
    AVER_PIX_NV12  = 2,
    AVER_PIX_RGB24 = 3,
    AVER_PIX_RGB32 = 4,   // BGRA in memory (与项目 gpu_color_ops 直接兼容)
    AVER_PIX_MJPG  = 5,
};

// 帧回调:由 SDK 在内部线程上调用,buffer 是设备共享内存,长度 length,带毫秒级时间戳。
// SDK 文档要求回调内只做 memcpy / push 到队列,不要做长耗时操作。
using FrameCallback = void(__stdcall*)(uint8_t* buffer,
                                       uint32_t length,
                                       uint64_t timestamp_100ns,
                                       void* user_ctx);

// SDK 函数指针表。Loader 解析 DLL 时填这些指针;未解析到的保持 nullptr。
// 名字与 AVerMedia SDK 头文件中的 C 接口一致(__stdcall WINAPI)。
struct ApiTable {
    // 基础: 版本 / 设备数 / 创建-释放句柄
    uint32_t (__stdcall* GetSDKVersion)(char* version_out, uint32_t buffer_size);
    uint32_t (__stdcall* GetDeviceNum)(uint32_t* num_out);
    uint32_t (__stdcall* CreateEngine)(uint32_t device_index, DeviceHandle* handle_out);
    uint32_t (__stdcall* ReleaseEngine)(DeviceHandle handle);

    // 视频源 / 信号探测
    uint32_t (__stdcall* SetVideoSource)(DeviceHandle handle, AverVideoSource source);
    uint32_t (__stdcall* GetVideoSource)(DeviceHandle handle, AverVideoSource* source_out);
    uint32_t (__stdcall* GetSignalPresence)(DeviceHandle handle, uint32_t* present_out);
    uint32_t (__stdcall* GetVideoInfo)(DeviceHandle handle,
                                       uint32_t* width_out,
                                       uint32_t* height_out,
                                       uint32_t* fps_x100_out,
                                       uint32_t* interlaced_out);

    // 格式 / 分辨率 / 帧率
    uint32_t (__stdcall* SetVideoResolution)(DeviceHandle handle, uint32_t width, uint32_t height);
    uint32_t (__stdcall* SetVideoInputFrameRate)(DeviceHandle handle, uint32_t fps_x100);
    uint32_t (__stdcall* SetVideoPixelFormat)(DeviceHandle handle, AverPixelFormat fmt);

    // 启停 + 回调注册
    uint32_t (__stdcall* StartStreaming)(DeviceHandle handle, FrameCallback cb, void* user_ctx);
    uint32_t (__stdcall* StopStreaming)(DeviceHandle handle);

    // 设备名查询(用于自动识别)。device_index 与 MF 枚举的索引未必对齐,所以
    // 我们用 friendly-name 字符串匹配做 backstop。
    uint32_t (__stdcall* GetDeviceFriendlyName)(uint32_t device_index,
                                                char* name_out,
                                                uint32_t buffer_size);
};

// 单例 Loader。第一次访问触发 LoadLibrary。线程安全。
class SdkLoader
{
public:
    static SdkLoader& Instance();

    // 是否成功加载到任一 AVerCapAPI*.dll。
    bool IsLoaded() const { return loaded_; }
    // 最关键的几个函数是否都解析到。任一缺失则视为不可用(直接退化)。
    bool IsUsable() const;

    // 返回函数指针表。永远非空;若 IsLoaded()=false 则全是 nullptr。
    const ApiTable& Api() const { return api_; }

    // 加载出的 DLL 路径(用于日志)。空字符串表示未加载。
    const std::string& DllPath() const { return dll_path_; }

    SdkLoader(const SdkLoader&) = delete;
    SdkLoader& operator=(const SdkLoader&) = delete;

private:
    SdkLoader();
    ~SdkLoader();

    void TryLoad();
    bool ResolveAll();

    mutable std::mutex mtx_;
    void* handle_{ nullptr };           // HMODULE
    bool loaded_{ false };
    ApiTable api_{};
    std::string dll_path_;
};

// 名称启发式: friendly-name 是否长得像圆刚 / AVerMedia 卡。匹配 "AVerMedia" /
// "Live Gamer" / "ExtremeCap" / "GC5xx" / "GC7xx" 等。大小写不敏感。
bool IsAverMediaFriendlyName(const std::string& name);

} // namespace avermedia

#endif // APOTHEOSIS_AVERMEDIA_SDK_H
