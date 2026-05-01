#pragma once
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <opencv2/core/cuda.hpp>

// Legacy path (float HWC -> float CHW with BGR->RGB). Kept for callers that
// still want the two-step flow.
void launch_hwc_to_chw_norm(
    const cv::cuda::GpuMat& hwcFloat3,
    float* dstChw,
    int width,
    int height,
    cudaStream_t stream
);

// Fused kernel: uint8 BGR HWC (resized square input) -> float CHW, BGR->RGB,
// divided by 255. Single kernel launch, single pass over memory; removes the
// intermediate float HWC buffer and the OpenCV convertTo call.
void launch_fused_bgr_u8_to_chw_rgb_f32(
    const cv::cuda::GpuMat& bgrU8,
    float* dstChw,
    int side,
    cudaStream_t stream
);

// FP16 variant: same fusion (uint8 BGR HWC -> half CHW RGB / 255) but writes
// directly into the engine's __half input buffer. Saves the implicit
// fp32->fp16 cast TensorRT would otherwise insert when the input dtype is
// kHALF and halves the input binding memory footprint.
void launch_fused_bgr_u8_to_chw_rgb_f16(
    const cv::cuda::GpuMat& bgrU8,
    __half* dstChw,
    int side,
    cudaStream_t stream
);

// Transpose + cast kernel for YOLOv8-style outputs shaped [1, C, N].
// Produces a row-major [N, C] float buffer so CPU post-processing can iterate
// anchors cache-friendly (unit stride per row). Works for both fp32 and fp16
// source tensors. Pass isHalf=true to cast __half -> float on the GPU.
void launch_transpose_decode_cast(
    const void* srcCN,
    float* dstNC,
    int C,
    int N,
    bool isHalf,
    cudaStream_t stream
);

// Fused decode + confidence filter kernel. Reads YOLOv8-style raw output
// [1, C, N] (fp16 or fp32), does argmax over nc class scores per anchor,
// converts (cx, cy, w, h) -> (x1, y1, w, h) scaled back to capture space,
// and atomically pushes kept detections into a compact [K, 6] buffer.
//
// dstCounter must be pre-zeroed on the same stream. The CPU then only has to
// D2H ~K*24 bytes instead of N*C*4 bytes, skip the fp16->fp32 cast, skip the
// cross-class argmax loop, and run NMS on a much smaller candidate set.
//
// The layout of each kept row is: [x1, y1, w, h, score, classId_as_float]
// matching the existing cols==6 postProcessYolo path.
void launch_decode_and_filter(
    const void* srcCN,
    int C,
    int N,
    int numClasses,
    bool isHalf,
    float confThreshold,
    float imgScale,
    int maxCandidates,
    int* dstCounter,
    float* dstCandidates,
    cudaStream_t stream
);
