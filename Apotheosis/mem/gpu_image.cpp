#include "gpu_image.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstring>

GpuImage::Storage::~Storage()
{
    if (base)
        cudaFree(base);
}

bool GpuImage::create(int rows, int cols, int channels)
{
    if (rows <= 0 || cols <= 0 || channels <= 0)
        return false;

    const size_t step = static_cast<size_t>(cols) * static_cast<size_t>(channels);
    const size_t total = step * static_cast<size_t>(rows);

    // If we already own a buffer big enough and our shape matches, reuse it.
    if (storage_ && storage_.use_count() == 1
        && storage_->capacity >= total
        && rows_ == rows && cols_ == cols && channels_ == channels)
    {
        data_ = storage_->base;
        step_ = step;
        return true;
    }

    auto storage = std::make_shared<Storage>();
    if (cudaMalloc(reinterpret_cast<void**>(&storage->base), total) != cudaSuccess)
        return false;
    storage->capacity = total;

    storage_ = std::move(storage);
    data_ = storage_->base;
    rows_ = rows;
    cols_ = cols;
    channels_ = channels;
    step_ = step;
    return true;
}

void GpuImage::release() noexcept
{
    storage_.reset();
    data_ = nullptr;
    rows_ = cols_ = channels_ = 0;
    step_ = 0;
}

bool GpuImage::upload(const unsigned char* src,
                      int rows,
                      int cols,
                      int channels,
                      size_t srcStep,
                      cudaStream_t stream)
{
    if (!src || !create(rows, cols, channels))
        return false;

    const size_t widthBytes = static_cast<size_t>(cols) * static_cast<size_t>(channels);
    cudaError_t err;
    if (stream)
    {
        err = cudaMemcpy2DAsync(
            data_, step_, src, srcStep, widthBytes, static_cast<size_t>(rows),
            cudaMemcpyHostToDevice, stream);
    }
    else
    {
        err = cudaMemcpy2D(
            data_, step_, src, srcStep, widthBytes, static_cast<size_t>(rows),
            cudaMemcpyHostToDevice);
    }
    return err == cudaSuccess;
}

void GpuImage::download(cv::Mat& dst, cudaStream_t stream) const
{
    if (empty())
    {
        dst.release();
        return;
    }

    const int cvType = CV_MAKETYPE(CV_8U, channels_);
    if (dst.rows != rows_ || dst.cols != cols_ || dst.type() != cvType)
        dst.create(rows_, cols_, cvType);

    const size_t widthBytes = static_cast<size_t>(cols_) * static_cast<size_t>(channels_);
    if (stream)
    {
        cudaMemcpy2DAsync(
            dst.data, dst.step, data_, step_, widthBytes, static_cast<size_t>(rows_),
            cudaMemcpyDeviceToHost, stream);
    }
    else
    {
        cudaMemcpy2D(
            dst.data, dst.step, data_, step_, widthBytes, static_cast<size_t>(rows_),
            cudaMemcpyDeviceToHost);
    }
}

GpuImage GpuImage::subRect(int x, int y, int w, int h) const
{
    GpuImage out;
    if (empty() || w <= 0 || h <= 0)
        return out;
    x = std::max(0, std::min(x, cols_));
    y = std::max(0, std::min(y, rows_));
    w = std::min(w, cols_ - x);
    h = std::min(h, rows_ - y);
    if (w <= 0 || h <= 0)
        return out;

    out.storage_ = storage_;
    out.data_ = data_ + static_cast<size_t>(y) * step_
              + static_cast<size_t>(x) * static_cast<size_t>(channels_);
    out.rows_ = h;
    out.cols_ = w;
    out.channels_ = channels_;
    out.step_ = step_;
    return out;
}
