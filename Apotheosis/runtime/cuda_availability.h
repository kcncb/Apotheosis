#ifndef RUNTIME_CUDA_AVAILABILITY_H
#define RUNTIME_CUDA_AVAILABILITY_H

#include <string>

namespace runtime
{
// Result of probing CUDA + TensorRT runtime DLLs at process startup.
struct CudaRuntimeStatus
{
    bool cudart_loadable = false;    // cudart64_12.dll reachable
    bool nvinfer_loadable = false;   // nvinfer_10.dll reachable
    bool nvonnxparser_loadable = false; // nvonnxparser_10.dll reachable
    bool device_available = false;   // >= 1 CUDA-capable device
    int cuda_runtime_version = 0;    // cudaRuntimeGetVersion(), 0 if unavailable
    int device_count = 0;
    std::string failure_reason;      // populated when trt_ready() is false

    bool trt_ready() const noexcept
    {
        return cudart_loadable && nvinfer_loadable && nvonnxparser_loadable && device_available;
    }
};

// Probe TensorRT availability. Must be called before the first CUDA/TRT symbol is
// touched so that delay-loaded modules fail gracefully instead of aborting the
// process. Subsequent calls return the cached result.
const CudaRuntimeStatus& probe_cuda_runtime();

// Convenience alias for UI code: returns true iff the TRT backend is safe to select.
bool is_tensorrt_available();
} // namespace runtime

#endif // RUNTIME_CUDA_AVAILABILITY_H
