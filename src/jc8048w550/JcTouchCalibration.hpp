// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#pragma once

#include <stdint.h>

namespace jc8048w550 {

struct TouchSample {
    float x;
    float y;
};

struct TouchMap {
    uint16_t version;
    float x_from_x;
    float x_from_y;
    float x_offset;
    float y_from_x;
    float y_from_y;
    float y_offset;
};

struct TouchCalibrationResult {
    TouchMap map;
    float corner_errors[4];
    float average_corner_error;
    float maximum_corner_error;
    float centre_error;
    bool axis_swapped;
    bool x_inverted;
    bool y_inverted;
};

bool derive_touch_map(const TouchSample corners[4], const TouchSample& centre,
                      TouchCalibrationResult& result);
bool valid_touch_map(const TouchMap& map);
void map_touch(const TouchMap& map, uint16_t input_x, uint16_t input_y,
               uint16_t& output_x, uint16_t& output_y);

} // namespace jc8048w550
