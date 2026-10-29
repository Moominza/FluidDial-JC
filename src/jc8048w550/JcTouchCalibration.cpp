// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcTouchCalibration.hpp"

#include <math.h>

namespace jc8048w550 {
namespace {

constexpr float kTargetCentreX = 400.0f;
constexpr float kTargetCentreY = 240.0f;
constexpr float kTargetHalfWidth = 340.0f;
constexpr float kTargetHalfHeight = 180.0f;
constexpr float kMaximumCornerError = 35.0f;
constexpr float kMaximumCentreError = 30.0f;

float distance(float x1, float y1, float x2, float y2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    return sqrtf(dx * dx + dy * dy);
}

bool finite(float value) {
    return isfinite(value);
}

} // namespace

bool valid_touch_map(const TouchMap& map) {
    if (map.version != 1
        || !finite(map.x_from_x) || !finite(map.x_from_y) || !finite(map.x_offset)
        || !finite(map.y_from_x) || !finite(map.y_from_y) || !finite(map.y_offset)) {
        return false;
    }

    if (fabsf(map.x_from_x) > 8.0f || fabsf(map.x_from_y) > 8.0f
        || fabsf(map.y_from_x) > 8.0f || fabsf(map.y_from_y) > 8.0f
        || fabsf(map.x_offset) > 4000.0f || fabsf(map.y_offset) > 4000.0f) {
        return false;
    }
    const float determinant = map.x_from_x * map.y_from_y
                            - map.x_from_y * map.y_from_x;
    return fabsf(determinant) > 0.0001f;
}

bool derive_touch_map(const TouchSample corners[4], const TouchSample& centre,
                      TouchCalibrationResult& result) {
    if (corners == nullptr) {
        return false;
    }

    for (uint8_t i = 0; i < 4; ++i) {
        if (!finite(corners[i].x) || !finite(corners[i].y)) {
            return false;
        }
    }
    if (!finite(centre.x) || !finite(centre.y)) {
        return false;
    }

    const float centre_x = (corners[0].x + corners[1].x
                          + corners[2].x + corners[3].x) * 0.25f;
    const float centre_y = (corners[0].y + corners[1].y
                          + corners[2].y + corners[3].y) * 0.25f;

    const float horizontal_x = (corners[1].x + corners[2].x
                              - corners[0].x - corners[3].x) * 0.5f;
    const float horizontal_y = (corners[1].y + corners[2].y
                              - corners[0].y - corners[3].y) * 0.5f;
    const float vertical_x = (corners[2].x + corners[3].x
                            - corners[0].x - corners[1].x) * 0.5f;
    const float vertical_y = (corners[2].y + corners[3].y
                            - corners[0].y - corners[1].y) * 0.5f;
    const float determinant = horizontal_x * vertical_y
                            - vertical_x * horizontal_y;
    if (!finite(determinant) || fabsf(determinant) < 10000.0f) {
        return false;
    }

    const float x_factor = (2.0f * kTargetHalfWidth) / determinant;
    const float y_factor = (2.0f * kTargetHalfHeight) / determinant;
    result.map.version = 1;
    result.map.x_from_x = x_factor * vertical_y;
    result.map.x_from_y = -x_factor * vertical_x;
    result.map.x_offset = kTargetCentreX
                        - result.map.x_from_x * centre_x
                        - result.map.x_from_y * centre_y;
    result.map.y_from_x = -y_factor * horizontal_y;
    result.map.y_from_y = y_factor * horizontal_x;
    result.map.y_offset = kTargetCentreY
                        - result.map.y_from_x * centre_x
                        - result.map.y_from_y * centre_y;
    if (!valid_touch_map(result.map)) {
        return false;
    }

    static constexpr float target_x[4] = { 60.0f, 740.0f, 740.0f, 60.0f };
    static constexpr float target_y[4] = { 60.0f, 60.0f, 420.0f, 420.0f };
    float total_error = 0.0f;
    result.maximum_corner_error = 0.0f;
    for (uint8_t i = 0; i < 4; ++i) {
        const float mapped_x = result.map.x_from_x * corners[i].x
                             + result.map.x_from_y * corners[i].y
                             + result.map.x_offset;
        const float mapped_y = result.map.y_from_x * corners[i].x
                             + result.map.y_from_y * corners[i].y
                             + result.map.y_offset;
        result.corner_errors[i] = distance(mapped_x, mapped_y,
                                           target_x[i], target_y[i]);
        total_error += result.corner_errors[i];
        if (result.corner_errors[i] > result.maximum_corner_error) {
            result.maximum_corner_error = result.corner_errors[i];
        }
    }
    result.average_corner_error = total_error * 0.25f;

    const float mapped_centre_x = result.map.x_from_x * centre.x
                                + result.map.x_from_y * centre.y
                                + result.map.x_offset;
    const float mapped_centre_y = result.map.y_from_x * centre.x
                                + result.map.y_from_y * centre.y
                                + result.map.y_offset;
    result.centre_error = distance(mapped_centre_x, mapped_centre_y,
                                   kTargetCentreX, kTargetCentreY);

    const bool swapped = fabsf(result.map.x_from_y) > fabsf(result.map.x_from_x);
    result.axis_swapped = swapped;
    result.x_inverted = swapped ? result.map.x_from_y < 0.0f
                                : result.map.x_from_x < 0.0f;
    result.y_inverted = swapped ? result.map.y_from_x < 0.0f
                                : result.map.y_from_y < 0.0f;
    return result.maximum_corner_error <= kMaximumCornerError
        && result.centre_error <= kMaximumCentreError;
}

void map_touch(const TouchMap& map, uint16_t input_x, uint16_t input_y,
               uint16_t& output_x, uint16_t& output_y) {
    const float x = map.x_from_x * input_x + map.x_from_y * input_y + map.x_offset;
    const float y = map.y_from_x * input_x + map.y_from_y * input_y + map.y_offset;
    const int32_t rounded_x = static_cast<int32_t>(lroundf(x));
    const int32_t rounded_y = static_cast<int32_t>(lroundf(y));
    output_x = static_cast<uint16_t>(rounded_x < 0 ? 0 : (rounded_x > 799 ? 799 : rounded_x));
    output_y = static_cast<uint16_t>(rounded_y < 0 ? 0 : (rounded_y > 479 ? 479 : rounded_y));
}

} // namespace jc8048w550
