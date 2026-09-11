#pragma once
#include <cstdint>
namespace runtime
{
// Immutable identity/geometry of the image underlying a detection. This travels
// with the frame, not through telemetry globals. Coordinates are crop pixels.
struct FrameContext
{
    uint64_t sequence = 0;
    int64_t captured_ns = 0;
    int width = 0, height = 0;
};
}
