#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <mutex>
#include <sstream>

#include "cuda_availability.h"

#include <cuda_runtime_api.h>

namespace runtime
{
namespace
{
// RAII wrapper for LoadLibraryW handles used solely for probing DLL presence.
class ProbeLibrary
{
public:
    explicit ProbeLibrary(const wchar_t* name) noexcept : handle_(::LoadLibraryW(name)) {}
    ~ProbeLibrary() { if (handle_) ::FreeLibrary(handle_); }
    ProbeLibrary(const ProbeLibrary&) = delete;
    ProbeLibrary& operator=(const ProbeLibrary&) = delete;
    bool loaded() const noexcept { return handle_ != nullptr; }

private:
    HMODULE handle_{};
};

CudaRuntimeStatus compute_status()
{
    CudaRuntimeStatus status;
    std::ostringstream reasons;

    const ProbeLibrary cudart(L"cudart64_12.dll");
    status.cudart_loadable = cudart.loaded();
    if (!status.cudart_loadable)
    {
        reasons << "cudart64_12.dll not found; ";
    }

    const ProbeLibrary nvinfer(L"nvinfer_10.dll");
    status.nvinfer_loadable = nvinfer.loaded();
    if (!status.nvinfer_loadable)
    {
        reasons << "nvinfer_10.dll not found; ";
    }

    const ProbeLibrary nvonnxparser(L"nvonnxparser_10.dll");
    status.nvonnxparser_loadable = nvonnxparser.loaded();
    if (!status.nvonnxparser_loadable)
    {
        reasons << "nvonnxparser_10.dll not found; ";
    }

    if (status.cudart_loadable)
    {
        // Delay-loaded cudart calls resolve lazily; guarded by DLL presence above.
        int runtime_version = 0;
        if (cudaRuntimeGetVersion(&runtime_version) == cudaSuccess)
        {
            status.cuda_runtime_version = runtime_version;
        }

        int device_count = 0;
        if (cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0)
        {
            status.device_count = device_count;
            status.device_available = true;
        }
        else
        {
            reasons << "no CUDA devices; ";
        }
    }

    status.failure_reason = reasons.str();
    if (!status.failure_reason.empty() && status.failure_reason.size() >= 2)
        status.failure_reason.resize(status.failure_reason.size() - 2); // trim trailing "; "
    return status;
}
} // namespace

const CudaRuntimeStatus& probe_cuda_runtime()
{
    static std::once_flag flag;
    static CudaRuntimeStatus cached;
    std::call_once(flag, [] { cached = compute_status(); });
    return cached;
}

bool is_tensorrt_available()
{
    return probe_cuda_runtime().trt_ready();
}
} // namespace runtime
