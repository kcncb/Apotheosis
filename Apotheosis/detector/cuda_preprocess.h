#pragma once
#include <cuda_runtime.h>
#include <cuda_fp16.h>

#include "../mem/gpu_image.h"

// Fused preprocess for the TensorRT detector input path. Reads a uint8 source
// frame (1, 3, or 4 channels — BGR / BGRA / GRAY), bilinear-resizes to
// side x side, swaps to RGB (or broadcasts gray), divides by 255, and writes
// the result as half CHW directly into the engine's __half input binding.
//
// Replaces the prior pipeline of cv::cuda::cvtColor + cv::cuda::resize +
// fused-convert kernel. One launch, no intermediate buffer, and it does not
// require OpenCV's CUDA modules to ship a kernel image for the current GPU.
void launch_resize_bgr_u8_to_chw_rgb_f16(
    const GpuFrame& src,
    __half* dstChw,
    int side,
    cudaStream_t stream
);

// ★ 2026-09-17: launch_decode_and_filter 的声明整段删除 —— 它服务 raw YOLO
//   输出路径, 而本程序现在只接受 end2end 模型 [1,N,6](解码已烘进图内)。
