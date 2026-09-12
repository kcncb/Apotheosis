#pragma once

#include "target_selection_exact.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace cvm::recovered {

// Fixed 4-state / 2-measurement Kalman filter recovered from
// 0x14005C6A0, 0x14005C9A0 and 0x14005CEB0.
struct TargetKalman4x2Exact {
    std::array<double, 16> transition{};
    std::array<double, 8> observation{};
    double position_noise_weight{0.05};
    double velocity_noise_weight{0.00625};

    // Native scratch survives failed inversions, so retain it as state.
    std::array<double, 4> last_projected_inverse{};

    TargetKalman4x2Exact() noexcept;

    void predict(
        std::array<double, 4>& mean,
        std::array<double, 16>& covariance,
        double scale) noexcept;

    void update(
        std::array<double, 4>& mean,
        std::array<double, 16>& covariance,
        std::array<double, 2> measurement,
        double scale) noexcept;
};

struct TargetTrackerExact {
    bool active{};
    SelectedTarget104Abi target{};
    std::int32_t lost_frames{};
    std::int32_t max_lost_frames{3};
    float search_radius{0.5f};

    TargetKalman4x2Exact kalman{};
    std::array<double, 4> mean{};
    std::array<double, 16> covariance{};
    std::array<double, 2> measurement_center{};
    std::array<float, 4> last_box{};
    std::array<float, 4> predicted_box{};
    std::array<float, 4> predicted_related_box{};

    float related_center_x_ratio{};
    float related_center_y_ratio{};
    float related_width_ratio{};
    float related_height_ratio{};
    std::int32_t related_class_id{-1};
    float related_confidence{};
    bool related_transform_valid{};

    void clear() noexcept; // 0x140064BD0
    void initialize(const SelectedTarget104Abi& measurement) noexcept;
    void update(const SelectedTarget104Abi* measurement) noexcept;

    // 0x140065190. When lost_frames >= 2, native subtracts origin from both
    // mean and last_box before predicting the exported record.
    const SelectedTarget104Abi* build_output(
        const std::array<float, 2>* origin_offset = nullptr) noexcept;

private:
    void extract_related_transform(const SelectedTarget104Abi& value) noexcept;
};

} // namespace cvm::recovered
