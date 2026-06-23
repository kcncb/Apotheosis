#include <cuda_runtime.h>
#include <cuda_fp16.h>

#include "cuda_preprocess.h"

// ---------------------------------------------------------------------------
// Fused preprocess: uint8 BGR/BGRA/GRAY HWC (any source size) ->
//   bilinear resize to side x side
//   -> BGR->RGB (or GRAY broadcast)
//   -> /255
//   -> half CHW written into the engine's __half input binding.
//
// This replaces the prior chain of (cv::cuda::cvtColor + cv::cuda::resize +
// fused convert kernel). One launch, no intermediate GpuMat, and it does not
// depend on the OpenCV CUDA modules being built for the current GPU's compute
// capability — nvcc emits a kernel image for whatever arch this project's
// CMAKE_CUDA_ARCHITECTURES targets.
// ---------------------------------------------------------------------------
static __global__ void resize_bgr_u8_to_chw_rgb_f16_kernel(
    const unsigned char* __restrict__ src,
    int srcStepBytes, int srcW, int srcH, int srcChannels,
    __half* __restrict__ dst, int side)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= side || y >= side) return;

    // Pixel-center sampling, matches OpenCV INTER_LINEAR semantics.
    const float scaleX = static_cast<float>(srcW) / static_cast<float>(side);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(side);
    const float fx = (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
    const float fy = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;

    const int x0 = max(0, min(srcW - 1, static_cast<int>(floorf(fx))));
    const int y0 = max(0, min(srcH - 1, static_cast<int>(floorf(fy))));
    const int x1 = min(srcW - 1, x0 + 1);
    const int y1 = min(srcH - 1, y0 + 1);
    const float ax = fx - floorf(fx);
    const float ay = fy - floorf(fy);

    const float w00 = (1.0f - ax) * (1.0f - ay);
    const float w01 = ax * (1.0f - ay);
    const float w10 = (1.0f - ax) * ay;
    const float w11 = ax * ay;

    const unsigned char* p00 = src + y0 * srcStepBytes + x0 * srcChannels;
    const unsigned char* p01 = src + y0 * srcStepBytes + x1 * srcChannels;
    const unsigned char* p10 = src + y1 * srcStepBytes + x0 * srcChannels;
    const unsigned char* p11 = src + y1 * srcStepBytes + x1 * srcChannels;

    constexpr float kInv255 = 1.0f / 255.0f;
    const int hw = side * side;
    const int idx = y * side + x;

    float r;
    float g;
    float b;
    if (srcChannels == 1)
    {
        const float v = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) * kInv255;
        r = v;
        g = v;
        b = v;
    }
    else
    {
        // src is BGR or BGRA (alpha is dropped). Swap to RGB on store.
        const float bv = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) * kInv255;
        const float gv = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) * kInv255;
        const float rv = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) * kInv255;
        r = rv;
        g = gv;
        b = bv;
    }

    dst[0 * hw + idx] = __float2half(r);
    dst[1 * hw + idx] = __float2half(g);
    dst[2 * hw + idx] = __float2half(b);
}

void launch_resize_bgr_u8_to_chw_rgb_f16(
    const GpuFrame& src,
    __half* dstChw,
    int side,
    cudaStream_t stream)
{
    if (src.empty()) return;
    const dim3 block(16, 16);
    const dim3 grid((side + block.x - 1) / block.x, (side + block.y - 1) / block.y);

    resize_bgr_u8_to_chw_rgb_f16_kernel<<<grid, block, 0, stream>>>(
        src.data, static_cast<int>(src.step),
        src.cols, src.rows, src.channels,
        dstChw, side
    );
}

// ---------------------------------------------------------------------------
// Fused decode + filter: [1, C, N] (fp16/fp32) -> compact [K, 6] candidates
// ---------------------------------------------------------------------------
template <typename SrcT>
__device__ __forceinline__ float load_value(const SrcT* src, int idx)
{
    const SrcT v = src[idx];
    if constexpr (sizeof(SrcT) == 2)
        return __half2float(*reinterpret_cast<const __half*>(&v));
    else
        return static_cast<float>(v);
}

template <typename SrcT>
__device__ __forceinline__ float read_cn(const SrcT* src, int c, int n, int C, int N, bool cnLayout)
{
    // cnLayout=true  -> model output is [1, C, N], element (c,n) at c*N + n
    // cnLayout=false -> model output is [1, N, C] (transposed export), element
    //                   (c,n) at n*C + c
    const int idx = cnLayout ? (c * N + n) : (n * C + c);
    return load_value<SrcT>(src, idx);
}

template <typename SrcT>
static __global__ void decode_and_filter_kernel(
    const SrcT* __restrict__ src,
    int C, int N, int nc,
    float confThreshold,
    float smallConf,
    float areaThreshPx,
    float imgScale,
    int maxCandidates,
    bool cnLayout,
    int* __restrict__ counter,
    float* __restrict__ dst)
{
    const int n = blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= N) return;

    // argmax over class scores
    float bestScore = 0.0f;
    int bestClass = 0;
    #pragma unroll 4
    for (int c = 0; c < nc; ++c)
    {
        const float s = read_cn<SrcT>(src, 4 + c, n, C, N, cnLayout);
        if (s > bestScore) { bestScore = s; bestClass = c; }
    }

    // Area-adaptive confidence threshold (small-target recall). Only when the
    // feature is ENABLED do we pay for the early ow/oh reads needed to compute
    // box area. When disabled we fall straight through to the original fast
    // path that reads NO geometry for the ~99% of anchors that fail the
    // threshold — keeping this kernel byte-for-byte as cheap as before.
    float thr = confThreshold;
    if (smallConf >= 0.0f && areaThreshPx > 0.0f)
    {
        const float owEarly = read_cn<SrcT>(src, 2, n, C, N, cnLayout);
        const float ohEarly = read_cn<SrcT>(src, 3, n, C, N, cnLayout);
        const float areaDet = owEarly * ohEarly * imgScale * imgScale;
        if (areaDet < areaThreshPx)
            thr = smallConf;
    }

    if (bestScore <= thr) return;

    // Past the threshold: now read box geometry and emit. Coords stay in
    // model-space; the CPU cols==6 decode path applies img_scale consistently.
    const float cx = read_cn<SrcT>(src, 0, n, C, N, cnLayout);
    const float cy = read_cn<SrcT>(src, 1, n, C, N, cnLayout);
    const float ow = read_cn<SrcT>(src, 2, n, C, N, cnLayout);
    const float oh = read_cn<SrcT>(src, 3, n, C, N, cnLayout);

    const int idx = atomicAdd(counter, 1);
    if (idx >= maxCandidates) return;

    float* out = dst + idx * 6;
    // Layout matches the existing cols==6 post-process path so the CPU decode
    // branch is reused unchanged: [x1, y1, x2, y2, score, classId].
    out[0] = cx - 0.5f * ow;            // x1
    out[1] = cy - 0.5f * oh;            // y1
    out[2] = cx + 0.5f * ow;            // x2
    out[3] = cy + 0.5f * oh;            // y2
    out[4] = bestScore;
    out[5] = static_cast<float>(bestClass);
}

void launch_decode_and_filter(
    const void* srcCN,
    int C, int N, int numClasses,
    bool isHalf,
    float confThreshold,
    float smallConf,
    float areaThreshPx,
    float imgScale,
    int maxCandidates,
    bool cnLayout,
    int* dstCounter,
    float* dstCandidates,
    cudaStream_t stream)
{
    // Zero the counter on-stream so this whole op stays graph-capturable.
    cudaMemsetAsync(dstCounter, 0, sizeof(int), stream);

    const int block = 256;
    const int grid = (N + block - 1) / block;

    if (isHalf)
    {
        decode_and_filter_kernel<__half><<<grid, block, 0, stream>>>(
            reinterpret_cast<const __half*>(srcCN),
            C, N, numClasses, confThreshold, smallConf, areaThreshPx, imgScale,
            maxCandidates, cnLayout, dstCounter, dstCandidates
        );
    }
    else
    {
        decode_and_filter_kernel<float><<<grid, block, 0, stream>>>(
            reinterpret_cast<const float*>(srcCN),
            C, N, numClasses, confThreshold, smallConf, areaThreshPx, imgScale,
            maxCandidates, cnLayout, dstCounter, dstCandidates
        );
    }
}
