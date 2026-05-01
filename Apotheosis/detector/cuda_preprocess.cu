#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_types.hpp>

static __global__ void hwc_to_chw_norm_kernel(
    const float* __restrict__ srcHwc, int srcStepFloats,
    float* __restrict__ dstChw,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int hw = height * width;
    const int idx = y * width + x;

    const float* p = srcHwc + y * srcStepFloats + x * 3;

    dstChw[0 * hw + idx] = p[2]; // R
    dstChw[1 * hw + idx] = p[1]; // G
    dstChw[2 * hw + idx] = p[0]; // B
}

void launch_hwc_to_chw_norm(
    const cv::cuda::GpuMat& hwcFloat3,
    float* dstChw,
    int width,
    int height,
    cudaStream_t stream)
{
    const dim3 block(16, 16);
    const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    const int stepFloats = static_cast<int>(hwcFloat3.step) / sizeof(float);
    const float* srcPtr = reinterpret_cast<const float*>(hwcFloat3.ptr<float>());

    hwc_to_chw_norm_kernel<<<grid, block, 0, stream>>>(
        srcPtr, stepFloats, dstChw, width, height
    );
}

// ---------------------------------------------------------------------------
// Fused preprocess: uint8 BGR HWC -> float CHW RGB / 255
// ---------------------------------------------------------------------------
static __global__ void fused_bgr_u8_to_chw_rgb_f32_kernel(
    const unsigned char* __restrict__ src, int srcStepBytes,
    float* __restrict__ dst, int side)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= side || y >= side) return;

    const int hw = side * side;
    const int idx = y * side + x;

    const unsigned char* p = src + y * srcStepBytes + x * 3;
    constexpr float kInv255 = 1.0f / 255.0f;

    dst[0 * hw + idx] = static_cast<float>(p[2]) * kInv255; // R
    dst[1 * hw + idx] = static_cast<float>(p[1]) * kInv255; // G
    dst[2 * hw + idx] = static_cast<float>(p[0]) * kInv255; // B
}

void launch_fused_bgr_u8_to_chw_rgb_f32(
    const cv::cuda::GpuMat& bgrU8,
    float* dstChw,
    int side,
    cudaStream_t stream)
{
    const dim3 block(16, 16);
    const dim3 grid((side + block.x - 1) / block.x, (side + block.y - 1) / block.y);

    const int stepBytes = static_cast<int>(bgrU8.step);
    const unsigned char* srcPtr = bgrU8.ptr<unsigned char>();

    fused_bgr_u8_to_chw_rgb_f32_kernel<<<grid, block, 0, stream>>>(
        srcPtr, stepBytes, dstChw, side
    );
}

static __global__ void fused_bgr_u8_to_chw_rgb_f16_kernel(
    const unsigned char* __restrict__ src, int srcStepBytes,
    __half* __restrict__ dst, int side)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= side || y >= side) return;

    const int hw = side * side;
    const int idx = y * side + x;

    const unsigned char* p = src + y * srcStepBytes + x * 3;
    constexpr float kInv255 = 1.0f / 255.0f;

    dst[0 * hw + idx] = __float2half(static_cast<float>(p[2]) * kInv255); // R
    dst[1 * hw + idx] = __float2half(static_cast<float>(p[1]) * kInv255); // G
    dst[2 * hw + idx] = __float2half(static_cast<float>(p[0]) * kInv255); // B
}

void launch_fused_bgr_u8_to_chw_rgb_f16(
    const cv::cuda::GpuMat& bgrU8,
    __half* dstChw,
    int side,
    cudaStream_t stream)
{
    const dim3 block(16, 16);
    const dim3 grid((side + block.x - 1) / block.x, (side + block.y - 1) / block.y);

    const int stepBytes = static_cast<int>(bgrU8.step);
    const unsigned char* srcPtr = bgrU8.ptr<unsigned char>();

    fused_bgr_u8_to_chw_rgb_f16_kernel<<<grid, block, 0, stream>>>(
        srcPtr, stepBytes, dstChw, side
    );
}

// ---------------------------------------------------------------------------
// Transpose [1, C, N] (fp16/fp32) -> [N, C] fp32
// ---------------------------------------------------------------------------
template <typename SrcT>
static __global__ void transpose_cn_to_nc_kernel(
    const SrcT* __restrict__ src,
    float* __restrict__ dst,
    int C, int N)
{
    const int n = blockIdx.x * blockDim.x + threadIdx.x;
    const int c = blockIdx.y * blockDim.y + threadIdx.y;
    if (n >= N || c >= C) return;

    const SrcT v = src[c * N + n];
    float f;
    if constexpr (sizeof(SrcT) == 2)
        f = __half2float(*reinterpret_cast<const __half*>(&v));
    else
        f = static_cast<float>(v);

    dst[n * C + c] = f;
}

void launch_transpose_decode_cast(
    const void* srcCN,
    float* dstNC,
    int C,
    int N,
    bool isHalf,
    cudaStream_t stream)
{
    const dim3 block(32, 8);
    const dim3 grid((N + block.x - 1) / block.x, (C + block.y - 1) / block.y);

    if (isHalf)
    {
        transpose_cn_to_nc_kernel<__half><<<grid, block, 0, stream>>>(
            reinterpret_cast<const __half*>(srcCN), dstNC, C, N
        );
    }
    else
    {
        transpose_cn_to_nc_kernel<float><<<grid, block, 0, stream>>>(
            reinterpret_cast<const float*>(srcCN), dstNC, C, N
        );
    }
}

// ---------------------------------------------------------------------------
// Fused decode + filter: [1, C, N] (fp16/fp32) -> compact [K, 6] candidates
// ---------------------------------------------------------------------------
template <typename SrcT>
__device__ __forceinline__ float read_cn(const SrcT* src, int c, int n, int N)
{
    const SrcT v = src[c * N + n];
    if constexpr (sizeof(SrcT) == 2)
        return __half2float(*reinterpret_cast<const __half*>(&v));
    else
        return static_cast<float>(v);
}

template <typename SrcT>
static __global__ void decode_and_filter_kernel(
    const SrcT* __restrict__ src,
    int C, int N, int nc,
    float confThreshold,
    float imgScale,
    int maxCandidates,
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
        const float s = read_cn<SrcT>(src, 4 + c, n, N);
        if (s > bestScore) { bestScore = s; bestClass = c; }
    }

    if (bestScore <= confThreshold) return;

    // Keep coords in model-space; the CPU cols==6 decode path applies
    // img_scale consistently for both this and the EfficientNMS plugin path.
    (void)imgScale;
    const float cx = read_cn<SrcT>(src, 0, n, N);
    const float cy = read_cn<SrcT>(src, 1, n, N);
    const float ow = read_cn<SrcT>(src, 2, n, N);
    const float oh = read_cn<SrcT>(src, 3, n, N);

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
    float imgScale,
    int maxCandidates,
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
            C, N, numClasses, confThreshold, imgScale,
            maxCandidates, dstCounter, dstCandidates
        );
    }
    else
    {
        decode_and_filter_kernel<float><<<grid, block, 0, stream>>>(
            reinterpret_cast<const float*>(srcCN),
            C, N, numClasses, confThreshold, imgScale,
            maxCandidates, dstCounter, dstCandidates
        );
    }
}
