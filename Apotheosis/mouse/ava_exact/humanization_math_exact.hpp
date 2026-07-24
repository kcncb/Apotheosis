#pragma once

#include <cstdint>

namespace cvm::recovered {

float finite_or(float value, float fallback) noexcept;
float smoothstep(float edge0, float edge1, float x) noexcept;
float shifted_lognormal_cdf(
    float value, float shift, float log_mean, float log_stddev) noexcept;
float shifted_lognormal_pdf(
    float value, float shift, float log_mean, float log_stddev) noexcept;
float normalized_dt(float dt_seconds) noexcept;

// Xorshift32 sequence used at QX state +0x3C.  A zero result is replaced by
// 0x9E3779B9, matching the conditional move in the binary.
std::uint32_t qx_next_u32(std::uint32_t& state) noexcept;
float qx_uniform24(std::uint32_t value) noexcept;
float random_normal(
    std::uint32_t& state, float mean, float standard_deviation) noexcept;

void rotate_unit_vector(float angle_step, float& x, float& y) noexcept;

} // namespace cvm::recovered
