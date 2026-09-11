#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace capture
{
inline bool rawFrameFits(size_t bytes, int width, int height, int stride,
                         int channels, bool nv12)
{
    if (width <= 0 || height <= 0 || channels <= 0 || stride == 0) return false;
    const uint64_t pitch = stride < 0 ? -static_cast<int64_t>(stride) : stride;
    const uint64_t row = static_cast<uint64_t>(width) * channels;
    if (pitch < row || pitch > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    if ((nv12 || channels == 2) && (width & 1)) return false;
    if (nv12 && ((height & 1) || stride < 0)) return false;
    if (stride < 0 && channels != 4) return false;
    const uint64_t rows = nv12 ? static_cast<uint64_t>(height) + height / 2 : height;
    const uint64_t needed = (rows - 1) * pitch + row;
    return needed <= bytes;
}
}
