// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// ONE client-side command policy shared by every transport (Wi-Fi TCP, wired UART, BLE UART, the
// simulator). FluidDial-JC is an HMI and command client: FluidNC is the authority on motion, travel,
// homing, probing and acceptance. This policy therefore checks command INTEGRITY only:
//   * the text is exactly one of the lines the command builders generate (strict grammar, no free text);
//   * the line belongs to the class the gate authorized;
//   * numbers are well formed and within generous typo-guard bounds (NOT a travel envelope).
// It never inspects machine state, homing, limits or capabilities.
//
// Pure: no I/O. A transport calls jc_command_line_allowed() on the exact bytes before it writes them.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "JcCommandAuth.hpp"

namespace jc8048w550 {

// Typo guards only. FluidNC rejects anything beyond its own configuration.
constexpr float kJcSanityMaxMove = 10000.0f;     // mm or degrees in one command
constexpr float kJcSanityMaxFeed = 30000.0f;     // mm/min or deg/min

namespace policy_detail {

inline bool is_axis(char c) { return c == 'X' || c == 'Y' || c == 'Z' || c == 'A'; }

// Reads "-?digits.ddd" (exactly three decimals, as the builders format). Advances p.
inline bool read_fixed3(const char*& p, float& out) {
    const char* s = p;
    if (*s == '-') ++s;
    int int_digits = 0;
    while (*s >= '0' && *s <= '9') { ++s; if (++int_digits > 6) return false; }
    if (int_digits == 0 || *s != '.') return false;
    ++s;
    for (int i = 0; i < 3; ++i) { if (*s < '0' || *s > '9') return false; ++s; }
    char buf[24];
    const size_t n = static_cast<size_t>(s - p);
    if (n >= sizeof(buf)) return false;
    memcpy(buf, p, n);
    buf[n] = '\0';
    out = strtof(buf, nullptr);
    if (!isfinite(out)) return false;
    p = s;
    return true;
}

inline bool read_uint(const char*& p, uint32_t max_digits, uint32_t& out) {
    uint32_t v = 0, n = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + static_cast<uint32_t>(*p - '0'); ++p; if (++n > max_digits) return false; }
    out = v;
    return n > 0;
}

inline bool eat(const char*& p, const char* lit) {
    const size_t n = strlen(lit);
    if (strncmp(p, lit, n) != 0) return false;
    p += n;
    return true;
}

}  // namespace policy_detail

// Parses "$J=G91G21F<feed><axis><-?d.ddd>". Same shape as jc_parse_jog_line (JcJogTest.hpp) but all four
// axes and the sanity bounds instead of the test limits.
inline bool jc_policy_jog_line(const char* text, uint8_t* axis_out = nullptr, float* dist_out = nullptr,
                               float* feed_out = nullptr) {
    using namespace policy_detail;
    const char* p = text;
    if (!eat(p, "$J=G91G21F")) return false;
    uint32_t feed = 0;
    if (!read_uint(p, 5, feed)) return false;
    if (!is_axis(*p)) return false;
    const uint8_t axis = static_cast<uint8_t>(strchr("XYZA", *p) - "XYZA");
    ++p;
    float d = 0.0f;
    if (!read_fixed3(p, d) || *p != '\0') return false;
    if (feed < 1u || static_cast<float>(feed) > kJcSanityMaxFeed) return false;
    if (d == 0.0f || fabsf(d) > kJcSanityMaxMove) return false;
    if (axis_out) *axis_out = axis;
    if (dist_out) *dist_out = d;
    if (feed_out) *feed_out = static_cast<float>(feed);
    return true;
}

// True when `text` is a line the builders can generate for class `cls`. Realtime single bytes are
// matched to their class too. Anything else (free G-code, $ settings, chained commands) is refused.
inline bool jc_command_line_allowed(JcCommandClass cls, const char* text) {
    using namespace policy_detail;
    if (text == nullptr || text[0] == '\0') return false;
    if (text[1] == '\0') {
        const uint8_t b = static_cast<uint8_t>(text[0]);
        switch (cls) {
            case JcCommandClass::JogCancel: return b == 0x85;
            case JcCommandClass::Hold: return b == '!';
            case JcCommandClass::Reset: return b == 0x18;
            default: return false;
        }
    }
    for (const char* q = text; *q; ++q) {
        if (static_cast<unsigned char>(*q) < 0x20 || static_cast<unsigned char>(*q) > 0x7E) return false;  // no CR/LF/control
    }
    const char* p = text;
    switch (cls) {
        case JcCommandClass::Jog:
            return jc_policy_jog_line(text);
        case JcCommandClass::Unlock:
            return strcmp(text, "$X") == 0;
        case JcCommandClass::Home:
            return strcmp(text, "$H") == 0 || (text[0] == '$' && text[1] == 'H' && is_axis(text[2]) && text[3] == '\0');
        case JcCommandClass::WorkOffset: {
            if (text[0] == 'G' && text[1] == '5' && text[2] >= '4' && text[2] <= '9' && text[3] == '\0') return true;
            if (strcmp(text, "G49") == 0) return true;                      // cancel the tool length offset
            if (eat(p, "G43.1 Z")) {                                        // dynamic tool length offset, Z only
                float t = 0.0f;
                return read_fixed3(p, t) && *p == '\0' && fabsf(t) <= 1000.0f;
            }
            if (eat(p, "G10 L2 P")) {   // origin of a WCS from machine coordinates; X, Y, Z in that order, each once
                if (*p < '1' || *p > '6') return false;
                ++p;
                const char order[] = "XYZ";
                size_t next = 0;
                int words = 0;
                while (*p == ' ') {
                    ++p;
                    size_t i = next;
                    while (i < 3 && order[i] != *p) ++i;
                    if (i >= 3) return false;
                    ++p;
                    float v = 0.0f;
                    if (!read_fixed3(p, v) || fabsf(v) > 100000.0f) return false;
                    next = i + 1;
                    ++words;
                }
                return words > 0 && *p == '\0';
            }
            if (!eat(p, "G10 L20 P") || !((*p >= '0' && *p <= '6'))) return false;
            ++p;
            if (*p != ' ') return false;
            ++p;
            if (!is_axis(*p)) return false;
            ++p;
            float v = 0.0f;
            return read_fixed3(p, v) && *p == '\0' && fabsf(v) <= 100000.0f;
        }
        case JcCommandClass::Probe: {
            if (strcmp(text, "G90") == 0) return true;
            if (eat(p, "G91 G0 ")) {
                if (!is_axis(*p)) return false;
                ++p;
                float v = 0.0f;
                return read_fixed3(p, v) && *p == '\0' && fabsf(v) <= kJcSanityMaxMove;
            }
            if (!eat(p, "G21 G91 G38.2 F")) return false;
            uint32_t feed = 0;
            if (!read_uint(p, 5, feed) || feed < 1u || static_cast<float>(feed) > kJcSanityMaxFeed) return false;
            if (*p != ' ') return false;
            ++p;
            if (*p != 'X' && *p != 'Y' && *p != 'Z') return false;   // the rotary axis is never probed
            ++p;
            float v = 0.0f;
            return read_fixed3(p, v) && *p == '\0' && v != 0.0f && fabsf(v) <= kJcSanityMaxMove;
        }
        case JcCommandClass::Job:
            // Instruction 010A: FluidDial never starts ($SD/Run=) or resumes ('~') a program. No line and no byte
            // is accepted for this class, whatever the transport, the gate or the operator's confirmation.
            return false;
        case JcCommandClass::Macro: {
            // User macros ($Localfs/Run=<path>) are disabled: a macro file can run machining code, and no
            // per-macro permission is defined. Only the built-in positioning lines below belong to this class.
            // Built-in positioning: absolute work coordinates, Z alone or X and/or Y (never both, never A).
            if (!eat(p, "G21 G90 G0 ")) return false;
            float v = 0.0f;
            if (*p == 'Z') {
                ++p;
                return read_fixed3(p, v) && *p == '\0' && fabsf(v) <= 100000.0f;
            }
            bool any = false;
            if (*p == 'X') {
                ++p;
                if (!read_fixed3(p, v) || fabsf(v) > 100000.0f) return false;
                any = true;
                if (*p == ' ') ++p;
                else return *p == '\0';
            }
            if (*p == 'Y') {
                ++p;
                if (!read_fixed3(p, v) || fabsf(v) > 100000.0f) return false;
                return *p == '\0';
            }
            (void)any;
            return false;
        }
        case JcCommandClass::Spindle:
            return strcmp(text, "M5") == 0;
        default:
            return false;
    }
}

// The final check every transport applies to the exact bytes it is about to write: one real-time byte, or one
// complete builder line ending in '\n'. It is THE command policy; transports do not implement their own.
inline bool jc_command_bytes_allowed(const char* bytes, size_t n) {
    if (bytes == nullptr || n == 0) return false;
    if (n == 1) {
        const uint8_t b = static_cast<uint8_t>(bytes[0]);
        return b == 0x85 || b == '!' || b == 0x18;   // jog cancel, feed hold, reset; never '~' (cycle start / resume)
    }
    char line[160];
    if (n >= sizeof(line) || bytes[n - 1] != '\n') return false;
    memcpy(line, bytes, n - 1);
    line[n - 1] = '\0';
    for (uint8_t c = 0; c <= static_cast<uint8_t>(JcCommandClass::Other); ++c) {
        if (jc_command_line_allowed(static_cast<JcCommandClass>(c), line)) return true;
    }
    return false;
}

}  // namespace jc8048w550
