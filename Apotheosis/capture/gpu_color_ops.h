#pragma once

#include <cuda_runtime.h>

// Hand-written replacements for the cv::cuda algorithms the capture path
// used to call. Keeping these here means nothing in the capture pipeline
// pulls in OpenCV's CUDA modules.

// Drop the alpha channel from a packed BGRA8 image, producing packed BGR8.
// Source / destination strides are independent (in bytes). All work is
// queued on `stream`; caller is responsible for synchronization.
void launch_bgra_to_bgr_u8(
    const unsigned char* src, size_t srcStep,
    unsigned char* dst, size_t dstStep,
    int width, int height,
    cudaStream_t stream);

// Bilinear resize for packed BGR8 (3-channel, uint8). Independent
// source/destination strides; interpolation uses pixel-center sampling to
// match cv::resize / cv::cuda::resize semantics with INTER_LINEAR.
void launch_resize_bgr_u8_bilinear(
    const unsigned char* src, size_t srcStep, int srcW, int srcH,
    unsigned char* dst, size_t dstStep, int dstW, int dstH,
    cudaStream_t stream);

// Convert NV12 (semi-planar Y + interleaved UV at half resolution) to packed
// BGR8. Used by the H.264 NVDEC path in eth_capture — NVDEC outputs NV12
// natively, the rest of the detector pipeline expects BGR8 interleaved.
// JFIF / BT.601 full-range; same colorspace the ProSexy NVENC encoded.
void launch_nv12_to_bgr_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream);

// In-place 圆形掩码:把内切圆之外的像素清零(黑)。center =(width/2,height/2),
// radius = min(width,height)/2。专门给 capture 路径用,替代旧的 OpenCV CPU
// apply_circle_mask——CMP 40HX 上一次 416x416 的 cv::Mat::copyTo(mask) 加上
// PCIe Gen1 D2H 之后才能跑,显著拖慢 240fps 链路。下推到 GPU 后 detector 直接
// 吃到 masked GpuImage,不再需要 CPU 走一遭。
void launch_circle_mask_bgr_u8(
    unsigned char* img, size_t step,
    int width, int height,
    cudaStream_t stream);
