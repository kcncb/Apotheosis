#include "gpu_color_ops.h"

#include <cuda_runtime.h>

static __global__ void bgra_to_bgr_u8_kernel(
    const unsigned char* __restrict__ src, int srcStep,
    unsigned char* __restrict__ dst, int dstStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const unsigned char* sp = src + y * srcStep + x * 4;
    unsigned char* dp = dst + y * dstStep + x * 3;
    dp[0] = sp[0];
    dp[1] = sp[1];
    dp[2] = sp[2];
}

void launch_bgra_to_bgr_u8(
    const unsigned char* src, size_t srcStep,
    unsigned char* dst, size_t dstStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!src || !dst || width <= 0 || height <= 0)
        return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    bgra_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        src, static_cast<int>(srcStep), dst, static_cast<int>(dstStep), width, height);
}

static __global__ void resize_bgr_u8_bilinear_kernel(
    const unsigned char* __restrict__ src, int srcStep, int srcW, int srcH,
    unsigned char* __restrict__ dst, int dstStep, int dstW, int dstH)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstW || y >= dstH) return;

    const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
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

    const unsigned char* p00 = src + y0 * srcStep + x0 * 3;
    const unsigned char* p01 = src + y0 * srcStep + x1 * 3;
    const unsigned char* p10 = src + y1 * srcStep + x0 * 3;
    const unsigned char* p11 = src + y1 * srcStep + x1 * 3;

    unsigned char* dp = dst + y * dstStep + x * 3;
    #pragma unroll
    for (int c = 0; c < 3; ++c)
    {
        const float v = p00[c] * w00 + p01[c] * w01 + p10[c] * w10 + p11[c] * w11;
        const int iv = max(0, min(255, static_cast<int>(v + 0.5f)));
        dp[c] = static_cast<unsigned char>(iv);
    }
}

void launch_resize_bgr_u8_bilinear(
    const unsigned char* src, size_t srcStep, int srcW, int srcH,
    unsigned char* dst, size_t dstStep, int dstW, int dstH,
    cudaStream_t stream)
{
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return;
    const dim3 block(32, 8);
    const dim3 grid((dstW + block.x - 1) / block.x,
                    (dstH + block.y - 1) / block.y);
    resize_bgr_u8_bilinear_kernel<<<grid, block, 0, stream>>>(
        src, static_cast<int>(srcStep), srcW, srcH,
        dst, static_cast<int>(dstStep), dstW, dstH);
}
