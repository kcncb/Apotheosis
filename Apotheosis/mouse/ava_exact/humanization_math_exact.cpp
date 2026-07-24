#include "humanization_math_exact.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace cvm::recovered {
namespace {
constexpr float kTinyDistance = 0.000099999997f;
constexpr float kMinSigma = 0.079999998f;
constexpr float kSqrtTwo = 1.414213538f;
constexpr float kTwoPi = 6.2831855f;
constexpr float kInv24 = 0.000000059604645f;
constexpr std::uint32_t kZeroSeedReplacement = 0x9E3779B9u;
}

float finite_or(float value, float fallback) noexcept {
    // ucrtbase!_fdclass returns >0 only for infinities and NaNs here.
    return std::isfinite(value) ? value : fallback;
}

float smoothstep(float edge0, float edge1, float x) noexcept {
    if (edge1 <= edge0)
        return x < edge1 ? 0.0f : 1.0f;
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return (3.0f - (t + t)) * (t * t);
}

float shifted_lognormal_cdf(
    float value, float shift, float log_mean, float log_stddev) noexcept {
    const float x = value - std::max(shift, 0.0f);
    if (x <= kTinyDistance)
        return 0.0f;
    const float sigma = std::max(kMinSigma, log_stddev);
    const float z = -((std::log(x) - log_mean) / (sigma * kSqrtTwo));
    return std::erfc(z) * 0.5f;
}

float shifted_lognormal_pdf(
    float value, float shift, float log_mean, float log_stddev) noexcept {
    const float x = value - std::max(shift, 0.0f);
    if (x <= kTinyDistance)
        return 0.0f;
    const float sigma = std::max(kMinSigma, log_stddev);
    const float z = (std::log(x) - log_mean) / sigma;
    const float denominator = std::sqrt(kTwoPi) * (sigma * x);
    if (denominator <= 0.000001f)
        return 0.0f;
    return std::exp((-0.5f * z) * z) / denominator;
}

float normalized_dt(float dt_seconds) noexcept {
    const float t = finite_or(dt_seconds, 0.025f) / 0.2f;
    return std::clamp(t, 0.0f, 1.0f);
}

std::uint32_t qx_next_u32(std::uint32_t& state) noexcept {
    std::uint32_t value = state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    if (value == 0)
        value = kZeroSeedReplacement;
    state = value;
    return value;
}

float qx_uniform24(std::uint32_t value) noexcept {
    return static_cast<float>(value >> 8) * kInv24;
}

float random_normal(
    std::uint32_t& state, float mean, float standard_deviation) noexcept {
    if (standard_deviation <= 0.0f)
        return mean;
    const std::uint32_t first = qx_next_u32(state);
    const std::uint32_t second = qx_next_u32(state);
    const float u1 = std::max(FLT_MIN, qx_uniform24(first));
    const float radius = std::sqrt(-2.0f * std::log(u1));
    const float angle = qx_uniform24(second) * kTwoPi;
    return mean + radius * std::cos(angle) * standard_deviation;
}

void rotate_unit_vector(float angle_step, float& x, float& y) noexcept {
    if (angle_step <= 0.0f)
        return;
    const float next_x = x + y * angle_step;
    const float next_y = y - x * angle_step;
    const float norm = std::sqrt(std::max(next_x * next_x + next_y * next_y, 0.000001f));
    // sub_1400464C0 performs one reciprocal and two multiplies.  Keeping that
    // operation order avoids the one-ULP difference produced by two divides.
    const float inverse_norm = 1.0f / norm;
    x = next_x * inverse_norm;
    y = next_y * inverse_norm;
}

} // namespace cvm::recovered
