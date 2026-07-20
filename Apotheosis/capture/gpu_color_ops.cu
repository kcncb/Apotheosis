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

// ---- NV12 -> BGR8 -----------------------------------------------------------
// One thread per output pixel. U/V plane is 4:2:0 downsampled + interleaved
// (NVDEC NV12 layout): UV row i = floor(y/2), UV col j = (x/2)*2 (U) and +1 (V).
// BT.601 full-range JFIF:
//   R = Y + 1.402   * (V - 128)
//   G = Y - 0.34414 * (U - 128) - 0.71414 * (V - 128)
//   B = Y + 1.772   * (U - 128)
// Q10 fixed point: x1024 integer math, shift back to byte, clamp.
static __device__ __forceinline__ unsigned char clamp_byte(int v) {
    return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static __global__ void nv12_to_bgr_u8_kernel(
    const unsigned char* __restrict__ y_plane, int yStep,
    const unsigned char* __restrict__ uv_plane, int uvStep,
    unsigned char* __restrict__ bgr, int bgrStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || yy >= height) return;

    const int Y = (int)y_plane[yy * yStep + x];
    const int uvRow = yy >> 1;
    const int uvCol = (x >> 1) << 1;
    const int U = (int)uv_plane[uvRow * uvStep + uvCol]     - 128;
    const int V = (int)uv_plane[uvRow * uvStep + uvCol + 1] - 128;

    int R = (Y * 1024 +              1436 * V + 512) >> 10;
    int G = (Y * 1024 -  352 * U -    731 * V + 512) >> 10;
    int B = (Y * 1024 + 1815 * U              + 512) >> 10;

    unsigned char* dp = bgr + yy * bgrStep + x * 3;
    dp[0] = clamp_byte(B);
    dp[1] = clamp_byte(G);
    dp[2] = clamp_byte(R);
}

void launch_nv12_to_bgr_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!y || !uv || !bgr || width <= 0 || height <= 0) return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    nv12_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        y,  (int)yStep,
        uv, (int)uvStep,
        bgr,(int)bgrStep,
        width, height);
}

// ---- YUV444 planar -> BGR8 -------------------------------------------------
// NVDEC 4:4:4 surface layout: Y / U / V three independent full-resolution
// planes, each at `stride` pitch. 1 thread per output pixel.
// BT.601 full-range JFIF (same coefficients as NV12 kernel, but full-res chroma
// -> no subsampling step). Q10 fixed point.
static __global__ void yuv444_to_bgr_u8_kernel(
    const unsigned char* __restrict__ y_p,
    const unsigned char* __restrict__ u_p,
    const unsigned char* __restrict__ v_p,
    int stride,
    unsigned char* __restrict__ bgr, int bgrStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || yy >= height) return;

    const int row = yy * stride + x;
    const int Y = (int)y_p[row];
    const int U = (int)u_p[row] - 128;
    const int V = (int)v_p[row] - 128;

    int R = (Y * 1024 +              1436 * V + 512) >> 10;
    int G = (Y * 1024 -  352 * U -    731 * V + 512) >> 10;
    int B = (Y * 1024 + 1815 * U              + 512) >> 10;

    unsigned char* dp = bgr + yy * bgrStep + x * 3;
    dp[0] = clamp_byte(B);
    dp[1] = clamp_byte(G);
    dp[2] = clamp_byte(R);
}

void launch_yuv444_to_bgr_u8(
    const unsigned char* y,
    const unsigned char* u,
    const unsigned char* v,
    size_t stride,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!y || !u || !v || !bgr || width <= 0 || height <= 0) return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    yuv444_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        y, u, v, (int)stride,
        bgr, (int)bgrStep,
        width, height);
}

// ---- In-place 圆形掩码 ------------------------------------------------------
// 半径平方比较代替 sqrt;一个线程一个像素,圆外置 0,圆内不动。
static __global__ void circle_mask_bgr_u8_kernel(
    unsigned char* __restrict__ img, int step,
    int width, int height,
    int cx, int cy, int radius_sq)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int dx = x - cx;
    const int dy = y - cy;
    if (dx * dx + dy * dy > radius_sq)
    {
        unsigned char* p = img + y * step + x * 3;
        p[0] = 0; p[1] = 0; p[2] = 0;
    }
}

void launch_circle_mask_bgr_u8(
    unsigned char* img, size_t step,
    int width, int height,
    cudaStream_t stream)
{
    if (!img || width <= 0 || height <= 0) return;
    const int cx = width / 2;
    const int cy = height / 2;
    const int r  = (width < height ? width : height) / 2;
    const int r2 = r * r;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    circle_mask_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        img, (int)step, width, height, cx, cy, r2);
}
