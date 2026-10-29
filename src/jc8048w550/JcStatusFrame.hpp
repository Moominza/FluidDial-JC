// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Pure, dependency-free FluidNC status report parser. Used by the firmware
// and by the host unit tests (test/host). It accepts only complete frames with
// a known machine state, exactly four finite MPos/WPos values and an FS pair.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace jc8048w550 {

struct JcStatusFrame {
    char  state[20];
    bool  machine_position;  // true: MPos, false: WPos
    float pos[4];
    bool  has_wco;
    float wco[4];
    float feed;
    float spindle;
    char  pins[12];  // "Pn:" field (e.g. "P" = probe), empty if absent
};

inline bool jc_valid_machine_state(const char* state) {
    static constexpr const char* states[] = {
        "Idle", "Alarm", "Hold:0", "Hold:1", "Run", "Jog", "Home",
        "Door:0", "Door:1", "Check", "Sleep"
    };
    if (state == nullptr) {
        return false;
    }
    for (const char* known : states) {
        if (strcmp(state, known) == 0) {
            return true;
        }
    }
    return false;
}

// Parses exactly `expected` comma-separated finite floats, destroying `value`.
inline bool jc_parse_float_list(char* value, size_t expected, float* out) {
    size_t count = 0;
    char*  cursor = value;
    while (cursor != nullptr) {
        char* comma = strchr(cursor, ',');
        if (comma != nullptr) {
            *comma = '\0';
        }
        char*       end = nullptr;
        const float parsed = strtof(cursor, &end);
        if (cursor == end || *end != '\0' || !isfinite(parsed) || count >= expected) {
            return false;
        }
        out[count++] = parsed;
        if (comma == nullptr) {
            break;
        }
        cursor = comma + 1;
    }
    return count == expected;
}

inline bool jc_parse_status_frame(const char* frame, size_t length, JcStatusFrame& out) {
    if (frame == nullptr || length < 8 || length >= 256 || frame[0] != '<'
        || frame[length - 1] != '>') {
        return false;
    }
    char body[256];
    memcpy(body, frame + 1, length - 2);
    body[length - 2] = '\0';

    JcStatusFrame parsed {};
    char*         first_field_end = strchr(body, '|');
    if (first_field_end == nullptr || first_field_end == body) {
        return false;
    }
    *first_field_end = '\0';
    if (!jc_valid_machine_state(body) || strlen(body) >= sizeof(parsed.state)) {
        return false;
    }
    strcpy(parsed.state, body);

    bool  has_position = false;
    bool  has_feed_spindle = false;
    char* field = first_field_end + 1;
    while (field != nullptr && *field != '\0') {
        char* next = strchr(field, '|');
        if (next != nullptr) {
            *next = '\0';
            ++next;
        }
        char* colon = strchr(field, ':');
        if (colon == nullptr || colon == field) {
            return false;
        }
        *colon = '\0';
        char* value = colon + 1;

        if (strcmp(field, "MPos") == 0 || strcmp(field, "WPos") == 0) {
            if (has_position || !jc_parse_float_list(value, 4, parsed.pos)) {
                return false;
            }
            has_position = true;
            parsed.machine_position = strcmp(field, "MPos") == 0;
        } else if (strcmp(field, "WCO") == 0) {
            if (!jc_parse_float_list(value, 4, parsed.wco)) {
                return false;
            }
            parsed.has_wco = true;
        } else if (strcmp(field, "FS") == 0) {
            float fs[2];
            if (!jc_parse_float_list(value, 2, fs)) {
                return false;
            }
            parsed.feed = fs[0];
            parsed.spindle = fs[1];
            has_feed_spindle = true;
        } else if (strcmp(field, "Pn") == 0) {
            if (strlen(value) >= sizeof(parsed.pins)) {
                return false;
            }
            strcpy(parsed.pins, value);
        }
        field = next;
    }
    if (!has_position || !has_feed_spindle) {
        return false;
    }
    out = parsed;
    return true;
}

// Machine position = work position + work coordinate offset (FluidNC reports
// either MPos or WPos, plus WCO periodically). Returns false if the offset is
// not known, so callers never silently substitute one reference for another.
inline bool jc_convert_position(const JcStatusFrame& frame, const float* known_wco,
                                bool want_machine, float out[4]) {
    if (frame.machine_position == want_machine) {
        memcpy(out, frame.pos, sizeof(frame.pos));
        return true;
    }
    const float* wco = frame.has_wco ? frame.wco : known_wco;
    if (wco == nullptr) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = want_machine ? frame.pos[i] + wco[i] : frame.pos[i] - wco[i];
    }
    return true;
}

}  // namespace jc8048w550
