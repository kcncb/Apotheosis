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

// Fused decode + confidence filter kernel. Reads YOLOv8-style raw output
// (fp16 or fp32), does argmax over nc class scores per anchor, converts
// (cx, cy, w, h) -> (x1, y1, x2, y2) in model space, and atomically pushes
// kept detections into a compact [K, 6] buffer.
//
// `cnLayout` selects between the two YOLO-family export layouts:
//   true  -> [1, C, N] (Ultralytics default, channels-major)
//   false -> [1, N, C] (transposed export, anchors-major)
// In both cases C is the channel count (4 + numClasses) and N the anchor count.
//
// dstCounter must be pre-zeroed on the same stream. The CPU then only has to
// D2H ~K*24 bytes instead of N*C*4 bytes, skip the fp16->fp32 cast, skip the
// cross-class argmax loop, and run NMS on a much smaller candidate set.
//
// The layout of each kept row is: [x1, y1, x2, y2, score, classId_as_float]
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
    bool cnLayout,
    int* dstCounter,
    float* dstCandidates,
    cudaStream_t stream
);
