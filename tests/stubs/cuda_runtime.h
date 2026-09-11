#pragma once
// Test-only instrumented CUDA event API. Never on the application's include path.
struct TestCudaEvent { int records = 0; };
using cudaEvent_t = TestCudaEvent*;
using cudaStream_t = void*;
using cudaError_t = int;
inline constexpr int cudaSuccess = 0;
inline constexpr unsigned cudaEventDisableTiming = 2;
inline int liveEvents = 0;
inline bool failCreate = false;
inline bool failRecord = false;
inline cudaError_t cudaEventCreateWithFlags(cudaEvent_t* out, unsigned)
{
    if (failCreate) return 1;
    *out = new TestCudaEvent;
    ++liveEvents;
    return 0;
}
inline cudaError_t cudaEventDestroy(cudaEvent_t event) { delete event; --liveEvents; return 0; }
inline cudaError_t cudaEventRecord(cudaEvent_t event, cudaStream_t)
{
    if (failRecord) return 1;
    ++event->records;
    return 0;
}
