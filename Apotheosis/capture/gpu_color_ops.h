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
