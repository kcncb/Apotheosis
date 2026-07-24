#include "target_tracker_exact.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cvm::recovered {
namespace {

double dot4(
    double a0, double b0,
    double a1, double b1,
    double a2, double b2,
    double a3, double b3) noexcept {
    return a0 * b0 + 0.0
        + a1 * b1
        + a2 * b2
        + a3 * b3;
}

bool invert_2x2_native(
    const std::array<double, 4>& input,
    std::array<double, 4>& output) noexcept {
    const double determinant = input[3] * input[0] - input[2] * input[1];
    if (std::fabs(determinant) < 1.0e-12)
        return false;
    const double reciprocal = 1.0 / determinant;
    output[0] = reciprocal * input[3];
    output[1] = -input[1] * reciprocal;
    output[2] = -input[2] * reciprocal;
    output[3] = reciprocal * input[0];
    return true;
}

float clamp_center_to_range(double value, double low, double high) noexcept {
    const double selected_low = low <= value ? value : low;
    const double selected = value <= high ? selected_low : high;
    return static_cast<float>(selected);
}

} // namespace

TargetKalman4x2Exact::TargetKalman4x2Exact() noexcept {
    transition = {
        1.0, 0.0, 1.0, 0.0,
        0.0, 1.0, 0.0, 1.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    };
    observation = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
    };
}

void TargetKalman4x2Exact::predict(
    std::array<double, 4>& mean,
    std::array<double, 16>& covariance,
    double scale) noexcept {
    const double effective_scale = std::fmax(1.0, scale);
    const double position_std = effective_scale * position_noise_weight;
    const double velocity_std = effective_scale * velocity_noise_weight;
    const std::array<double, 4> process_diagonal{
        position_std * position_std,
        position_std * position_std,
        velocity_std * velocity_std,
        velocity_std * velocity_std,
    };

    std::array<double, 4> predicted_mean{};
    for (std::size_t row = 0; row < 4; ++row) {
        predicted_mean[row] = dot4(
            mean[0], transition[row * 4 + 0],
            mean[1], transition[row * 4 + 1],
            mean[2], transition[row * 4 + 2],
            mean[3], transition[row * 4 + 3]);
    }

    std::array<double, 16> first_product{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            first_product[row * 4 + column] = dot4(
                transition[row * 4 + 0], covariance[0 * 4 + column],
                transition[row * 4 + 1], covariance[1 * 4 + column],
                transition[row * 4 + 2], covariance[2 * 4 + column],
                transition[row * 4 + 3], covariance[3 * 4 + column]);
        }
    }

    std::array<double, 16> predicted_covariance{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            predicted_covariance[row * 4 + column] = dot4(
                first_product[row * 4 + 0], transition[column * 4 + 0],
                first_product[row * 4 + 1], transition[column * 4 + 1],
                first_product[row * 4 + 2], transition[column * 4 + 2],
                first_product[row * 4 + 3], transition[column * 4 + 3])
                + (row == column ? process_diagonal[row] : 0.0);
        }
    }
    mean = predicted_mean;
    covariance = predicted_covariance;
}

void TargetKalman4x2Exact::update(
    std::array<double, 4>& mean,
    std::array<double, 16>& covariance,
    std::array<double, 2> measurement,
    double scale) noexcept {
    const double measurement_std = std::fmax(1.0, scale)
        * position_noise_weight;
    const double measurement_variance = measurement_std * measurement_std;

    const std::array<double, 2> projected_mean{mean[0], mean[1]};
    std::array<double, 4> projected_covariance{
        measurement_variance + covariance[0], covariance[1],
        covariance[4], measurement_variance + covariance[5],
    };

    if (!invert_2x2_native(projected_covariance, last_projected_inverse)) {
        const double a = projected_covariance[0] + 0.000001;
        const double b = projected_covariance[1];
        const double c = projected_covariance[2];
        const double d = projected_covariance[3] + 0.000001;
        const double determinant = d * a - b * c;
        if (std::fabs(determinant) >= 1.0e-12) {
            const double reciprocal = 1.0 / determinant;
            last_projected_inverse[0] = d * reciprocal;
            last_projected_inverse[1] = -b * reciprocal;
            last_projected_inverse[2] = -c * reciprocal;
            last_projected_inverse[3] = a * reciprocal;
        }
    }

    // P*H^T is the first two columns of P.
    std::array<double, 8> covariance_observation{};
    for (std::size_t row = 0; row < 4; ++row) {
        covariance_observation[row * 2 + 0] = covariance[row * 4 + 0];
        covariance_observation[row * 2 + 1] = covariance[row * 4 + 1];
    }

    std::array<double, 8> gain{};
    for (std::size_t row = 0; row < 4; ++row) {
        const double p0 = covariance_observation[row * 2 + 0];
        const double p1 = covariance_observation[row * 2 + 1];
        gain[row * 2 + 0] = p0 * last_projected_inverse[0] + 0.0
            + p1 * last_projected_inverse[2];
        gain[row * 2 + 1] = p0 * last_projected_inverse[1] + 0.0
            + p1 * last_projected_inverse[3];
    }

    const std::array<double, 2> innovation{
        measurement[0] - projected_mean[0],
        measurement[1] - projected_mean[1],
    };
    std::array<double, 4> updated_mean{};
    for (std::size_t row = 0; row < 4; ++row) {
        updated_mean[row] = innovation[0] * gain[row * 2 + 0] + 0.0
            + innovation[1] * gain[row * 2 + 1]
            + mean[row];
    }

    std::array<double, 8> gain_projected{};
    for (std::size_t row = 0; row < 4; ++row) {
        gain_projected[row * 2 + 0] =
            gain[row * 2 + 0] * projected_covariance[0] + 0.0
            + projected_covariance[2] * gain[row * 2 + 1];
        gain_projected[row * 2 + 1] =
            gain[row * 2 + 0] * projected_covariance[1] + 0.0
            + gain[row * 2 + 1] * projected_covariance[3];
    }

    std::array<double, 16> updated_covariance{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            updated_covariance[row * 4 + column] = covariance[row * 4 + column]
                - (gain[column * 2 + 0] * gain_projected[row * 2 + 0] + 0.0
                   + gain[column * 2 + 1] * gain_projected[row * 2 + 1]);
        }
    }
    mean = updated_mean;
    covariance = updated_covariance;
}

void TargetTrackerExact::clear() noexcept {
    active = false;
    target = {};
    target.related_class_id = -1;
    lost_frames = 0;
    mean = {};
    covariance = {};
    measurement_center = {};
    last_box = {};
    predicted_box = {};
    predicted_related_box = {};
    related_center_x_ratio = 0.0f;
    related_center_y_ratio = 0.0f;
    related_width_ratio = 0.0f;
    related_height_ratio = 0.0f;
    related_class_id = -1;
    related_confidence = 0.0f;
    related_transform_valid = false;
}

void TargetTrackerExact::extract_related_transform(
    const SelectedTarget104Abi& value) noexcept {
    if (!value.related_box_valid)
        return;
    const float width = value.right - value.left;
    const float height = value.bottom - value.top;
    const double primary_center_x =
        (static_cast<double>(value.right) + static_cast<double>(value.left)) * 0.5;
    const double primary_center_y =
        (static_cast<double>(value.bottom) + static_cast<double>(value.top)) * 0.5;
    const double related_center_x =
        (static_cast<double>(value.related_right)
            + static_cast<double>(value.related_left)) * 0.5;
    const double related_center_y =
        (static_cast<double>(value.related_bottom)
            + static_cast<double>(value.related_top)) * 0.5;
    related_center_x_ratio = width > 0.0f
        ? static_cast<float>((related_center_x - primary_center_x) / width)
        : 0.0f;
    related_center_y_ratio = height > 0.0f
        ? static_cast<float>((related_center_y - primary_center_y) / height)
        : 0.0f;
    related_width_ratio = width > 0.0f
        ? (value.related_right - value.related_left) / width
        : 0.0f;
    related_height_ratio = height > 0.0f
        ? (value.related_bottom - value.related_top) / height
        : 0.0f;
    related_class_id = value.related_class_id;
    related_confidence = value.related_confidence;
    related_transform_valid = true;
}

void TargetTrackerExact::initialize(
    const SelectedTarget104Abi& measurement) noexcept {
    clear();
    target = measurement;
    target.target_flag = 0;
    target.target_kind_or_age = 0;
    lost_frames = 0;
    active = true;

    measurement_center[0] =
        (static_cast<double>(measurement.right)
            + static_cast<double>(measurement.left)) * 0.5;
    measurement_center[1] =
        (static_cast<double>(measurement.bottom)
            + static_cast<double>(measurement.top)) * 0.5;
    mean = {measurement_center[0], measurement_center[1], 0.0, 0.0};

    const double scale = std::fmax(
        1.0, static_cast<double>(measurement.effective_height));
    const double position_std =
        (kalman.position_noise_weight + kalman.position_noise_weight) * scale;
    const double velocity_std =
        kalman.velocity_noise_weight * 10.0 * scale;
    covariance = {};
    covariance[0] = position_std * position_std;
    covariance[5] = position_std * position_std;
    covariance[10] = velocity_std * velocity_std;
    covariance[15] = velocity_std * velocity_std;
    last_box = {measurement.left, measurement.top,
                measurement.right, measurement.bottom};
    extract_related_transform(measurement);
}

void TargetTrackerExact::update(
    const SelectedTarget104Abi* measurement) noexcept {
    if (!measurement) {
        if (active) {
            ++lost_frames;
            target.target_kind_or_age = lost_frames;
            if (lost_frames > max_lost_frames)
                clear();
        }
        return;
    }
    if (!active) {
        initialize(*measurement);
        return;
    }

    const std::int32_t previous_class = target.class_id;
    lost_frames = 0;
    SelectedTarget104Abi incoming = *measurement;
    incoming.target_kind_or_age = 0;
    measurement_center[0] =
        (static_cast<double>(incoming.right)
            + static_cast<double>(incoming.left)) * 0.5;
    measurement_center[1] =
        (static_cast<double>(incoming.bottom)
            + static_cast<double>(incoming.top)) * 0.5;
    kalman.update(mean, covariance, measurement_center,
                  static_cast<double>(incoming.effective_height));
    last_box = {incoming.left, incoming.top, incoming.right, incoming.bottom};
    if (incoming.class_id != previous_class)
        related_transform_valid = false;
    extract_related_transform(incoming);
    target = incoming;
    target.target_flag = 0;
}

const SelectedTarget104Abi* TargetTrackerExact::build_output(
    const std::array<float, 2>* origin_offset) noexcept {
    if (!active)
        return nullptr;

    const float width = std::fmax(1.0f, last_box[2] - last_box[0]);
    const float height = std::fmax(1.0f, last_box[3] - last_box[1]);
    kalman.predict(mean, covariance, static_cast<double>(height));

    if (lost_frames >= 2 && origin_offset) {
        mean[0] -= (*origin_offset)[0];
        mean[1] -= (*origin_offset)[1];
        last_box[0] -= (*origin_offset)[0];
        last_box[2] -= (*origin_offset)[0];
        last_box[1] -= (*origin_offset)[1];
        last_box[3] -= (*origin_offset)[1];
    }

    const float half_width = width * 0.5f;
    const float half_height = height * 0.5f;
    float center_x{};
    float center_y{};
    if (search_radius > 0.0f) {
        const double last_center_x =
            (static_cast<double>(last_box[0]) + static_cast<double>(last_box[2])) * 0.5;
        const double last_center_y =
            (static_cast<double>(last_box[1]) + static_cast<double>(last_box[3])) * 0.5;
        const float loss_scale = std::fmax(
            static_cast<float>(lost_frames) * 0.25f, 1.0f);
        const float expansion_f =
            (std::fmin(height, width) * search_radius) * loss_scale;
        const double expansion = static_cast<double>(expansion_f);
        center_x = clamp_center_to_range(
            mean[0], last_center_x - expansion, last_center_x + expansion);
        center_y = clamp_center_to_range(
            mean[1], last_center_y - expansion, last_center_y + expansion);
    } else {
        center_x = static_cast<float>(mean[0]);
        center_y = static_cast<float>(mean[1]);
    }

    predicted_box = {
        center_x - half_width,
        center_y - half_height,
        center_x + half_width,
        center_y + half_height,
    };
    target.left = predicted_box[0];
    target.top = predicted_box[1];
    target.right = predicted_box[2];
    target.bottom = predicted_box[3];
    target.effective_width = predicted_box[2] - predicted_box[0];
    target.effective_height = predicted_box[3] - predicted_box[1];
    target.effective_center_x =
        (static_cast<double>(predicted_box[2])
            + static_cast<double>(predicted_box[0])) * 0.5;
    target.effective_center_y =
        (static_cast<double>(predicted_box[3])
            + static_cast<double>(predicted_box[1])) * 0.5;
    target.primary_center_x = target.effective_center_x;
    target.primary_center_y = target.effective_center_y;
    target.target_flag = lost_frames > 0;
    target.target_kind_or_age = lost_frames;
    target.predicted_center_valid = 0;

    if (lost_frames || !target.related_box_valid) {
        if (related_transform_valid) {
            const float box_width = predicted_box[2] - predicted_box[0];
            const float box_height = predicted_box[3] - predicted_box[1];
            const float related_half_width =
                (box_width * related_width_ratio) * 0.5f;
            const float related_x_offset =
                box_width * related_center_x_ratio;
            const float related_center_x = static_cast<float>(
                (static_cast<double>(predicted_box[2])
                    + static_cast<double>(predicted_box[0])) * 0.5
                + static_cast<double>(related_x_offset));
            const float related_half_height =
                (box_height * related_height_ratio) * 0.5f;
            const float related_y_offset =
                box_height * related_center_y_ratio;
            const float related_center_y = static_cast<float>(
                (static_cast<double>(predicted_box[3])
                    + static_cast<double>(predicted_box[1])) * 0.5
                + static_cast<double>(related_y_offset));
            predicted_related_box = {
                related_center_x - related_half_width,
                related_center_y - related_half_height,
                related_center_x + related_half_width,
                related_center_y + related_half_height,
            };
            target.related_box_valid = 1;
            target.related_class_id = related_class_id;
            target.related_confidence = related_confidence;
            target.related_left = predicted_related_box[0];
            target.related_top = predicted_related_box[1];
            target.related_right = predicted_related_box[2];
            target.related_bottom = predicted_related_box[3];
        } else {
            target.related_box_valid = 0;
            target.related_class_id = -1;
            target.related_confidence = 0.0f;
            target.related_left = 0.0f;
            target.related_top = 0.0f;
            target.related_right = 0.0f;
            target.related_bottom = 0.0f;
        }
    }
    return &target;
}

} // namespace cvm::recovered
