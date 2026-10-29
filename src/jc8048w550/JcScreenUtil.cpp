// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

const char* jc_axis_name(uint8_t axis) {
    static const char* const names[] = { "X", "Y", "Z", "A" };
    return names[axis < 4 ? axis : 0];
}

const char* jc_axis_unit(uint8_t axis) {
    return axis == 3 ? "deg" : "mm";
}

// Formats a live position, or "--" when it cannot be determined (no data, or
// the other reference needs a work offset the controller has not reported).
void jc_format_position(const JcAppServices& sv, uint8_t axis, JcRef ref, char* out, size_t size,
                        bool* available) {
    float value = 0.0f;
    const bool ok = sv.model.position(axis, ref, value);
    if (available) {
        *available = ok;
    }
    if (!ok) {
        snprintf(out, size, "--");
        return;
    }
    snprintf(out, size, "%.*f", static_cast<int>(sv.settings.coordinate_decimals), static_cast<double>(value));
}

void jc_format_step(char* out, size_t size, float value, uint8_t axis) {
    snprintf(out, size, "%g %s", static_cast<double>(value), jc_axis_unit(axis));
}

const char* jc_link_summary(const JcAppServices& sv) {
    return JcReadOnlyFluidNc::state_name(sv.link.state());
}

}  // namespace jc8048w550
