#include "gpu_jpeg_decoder.h"

#include <iostream>

namespace capture {

GpuJpegDecoder::GpuJpegDecoder() = default;

GpuJpegDecoder::~GpuJpegDecoder()
{
    cleanup();
}

void GpuJpegDecoder::cleanup()
{
    if (state_)
    {
        nvjpegJpegStateDestroy(state_);
        state_ = nullptr;
    }
    if (handle_)
    {
        nvjpegDestroy(handle_);
        handle_ = nullptr;
    }
    initialized_ = false;
}

bool GpuJpegDecoder::init()
{
    if (initialized_) return true;

    // NVJPEG_BACKEND_DEFAULT picks hybrid CPU/GPU. The hardware (NVJPEG_BACKEND_HARDWARE)
    // backend is only available on A100/Hopper-class cards; default is portable.
    nvjpegStatus_t st = nvjpegCreateSimple(&handle_);
    if (st != NVJPEG_STATUS_SUCCESS)
    {
        std::cerr << "[GpuJpegDecoder] nvjpegCreateSimple failed: " << st << std::endl;
        cleanup();
        return false;
    }

    st = nvjpegJpegStateCreate(handle_, &state_);
    if (st != NVJPEG_STATUS_SUCCESS)
    {
        std::cerr << "[GpuJpegDecoder] nvjpegJpegStateCreate failed: " << st << std::endl;
        cleanup();
        return false;
    }

    initialized_ = true;
    return true;
}

bool GpuJpegDecoder::decode(
    const uint8_t* data,
    size_t size,
    cv::cuda::GpuMat& dst,
    cudaStream_t stream)
{
    if (!initialized_ || !data || size == 0) return false;

    int nComponents = 0;
    nvjpegChromaSubsampling_t subsampling;
    int widths[NVJPEG_MAX_COMPONENT] = { 0 };
    int heights[NVJPEG_MAX_COMPONENT] = { 0 };

    nvjpegStatus_t st = nvjpegGetImageInfo(
        handle_, data, size, &nComponents, &subsampling, widths, heights);
    if (st != NVJPEG_STATUS_SUCCESS)
        return false;

    const int W = widths[0];
    const int H = heights[0];
    if (W <= 0 || H <= 0) return false;

    // NVJPEG_OUTPUT_BGRI -> interleaved BGR, channel[0] holds the whole image
    // with pitch = 3*W (or the allocated GpuMat step, whichever is larger).
    // Reuse dst if its layout already matches to avoid reallocations on the
    // UDP hot path.
    if (dst.empty() || dst.rows != H || dst.cols != W || dst.type() != CV_8UC3)
    {
        dst.create(H, W, CV_8UC3);
    }

    nvjpegImage_t out{};
    out.channel[0] = dst.data;
    out.pitch[0] = static_cast<unsigned int>(dst.step);

    st = nvjpegDecode(
        handle_, state_, data, size,
        NVJPEG_OUTPUT_BGRI, &out, stream);

    return st == NVJPEG_STATUS_SUCCESS;
}

} // namespace capture
