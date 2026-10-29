// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Classification of the text lines a FluidNC controller sends back (Grbl-style
// protocol): "ok", "error:N", "ALARM:N", "[MSG:...]", "[GC:...]", "[VER:...]",
// "$/axes/x/steps_per_mm=80.000" setting echoes and banners. Status reports
// ('<...>') are handled by JcFrameAssembler/jc_parse_status_frame instead.
// Pure logic shared by the firmware and the host tests.

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace jc8048w550 {

enum class JcRespKind : uint8_t {
    Empty,
    Ok,
    Error,       // "error:N"
    Alarm,       // "ALARM:N"
    Message,     // "[MSG:...]"
    GCodeState,  // "[GC:...]"
    Info,        // other "[TAG:...]" lines such as [VER:...] and [OPT:...]
    Setting,     // "$<path>=<value>" echo of a configuration item
    Banner,      // "Grbl ..." / "FluidNC ..." start-up text
    Other,
};

struct JcResponse {
    JcRespKind kind = JcRespKind::Empty;
    int        code = 0;       // Error / Alarm number; 0 when none was given
    char       key[56] = "";   // Setting path (no leading '$'), or the Info tag
    char       value[72] = ""; // Setting value, or the payload of Message/GC/Info
};

namespace detail {

inline void copy_trunc(char* dst, size_t cap, const char* src, size_t n) {
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

inline bool starts_with(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

}  // namespace detail

// Returns Other (never throws away information silently) for text that does not
// match a known shape. `line` must be NUL-terminated and without the newline.
inline JcResponse jc_classify_line(const char* line) {
    using namespace detail;
    JcResponse r;
    if (line == nullptr) {
        return r;
    }
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) {
        --n;
    }
    if (n == 0) {
        return r;
    }
    if (n == 2 && line[0] == 'o' && line[1] == 'k') {
        r.kind = JcRespKind::Ok;
        return r;
    }
    if (n >= 6 && starts_with(line, "error:")) {
        r.kind = JcRespKind::Error;
        char* end = nullptr;
        const long v = strtol(line + 6, &end, 10);
        r.code = (end != line + 6 && v > 0 && v < 1000) ? static_cast<int>(v) : 0;
        return r;
    }
    if (n >= 6 && starts_with(line, "ALARM:")) {
        r.kind = JcRespKind::Alarm;
        char* end = nullptr;
        const long v = strtol(line + 6, &end, 10);
        r.code = (end != line + 6 && v > 0 && v < 1000) ? static_cast<int>(v) : 0;
        return r;
    }
    if (line[0] == '[' && line[n - 1] == ']') {
        const char* inner = line + 1;
        const size_t inner_n = n - 2;
        const char* colon = static_cast<const char*>(memchr(inner, ':', inner_n));
        const size_t tag_n = colon ? static_cast<size_t>(colon - inner) : inner_n;
        const char* payload = colon ? colon + 1 : inner + inner_n;
        const size_t payload_n = colon ? inner_n - tag_n - 1 : 0;
        if (tag_n == 3 && memcmp(inner, "MSG", 3) == 0) {
            r.kind = JcRespKind::Message;
        } else if (tag_n == 2 && memcmp(inner, "GC", 2) == 0) {
            r.kind = JcRespKind::GCodeState;
        } else {
            r.kind = JcRespKind::Info;
        }
        copy_trunc(r.key, sizeof(r.key), inner, tag_n);
        copy_trunc(r.value, sizeof(r.value), payload, payload_n);
        return r;
    }
    if (line[0] == '$') {
        const char* eq = static_cast<const char*>(memchr(line, '=', n));
        if (eq != nullptr && eq > line + 1) {
            r.kind = JcRespKind::Setting;
            copy_trunc(r.key, sizeof(r.key), line + 1, static_cast<size_t>(eq - line - 1));
            copy_trunc(r.value, sizeof(r.value), eq + 1, n - static_cast<size_t>(eq - line) - 1);
            return r;
        }
    }
    if (starts_with(line, "Grbl ") || starts_with(line, "FluidNC ")) {
        r.kind = JcRespKind::Banner;
        copy_trunc(r.value, sizeof(r.value), line, n);
        return r;
    }
    r.kind = JcRespKind::Other;
    copy_trunc(r.value, sizeof(r.value), line, n);
    return r;
}

// Short human text for the Grbl error numbers this project shows. FluidNC adds
// more codes; an unlisted number is reported as such rather than guessed.
inline const char* jc_error_text(int code) {
    switch (code) {
        case 1: return "expected command letter";
        case 2: return "bad number format";
        case 3: return "invalid $ statement";
        case 4: return "negative value";
        case 5: return "setting disabled";
        case 8: return "not idle";
        case 9: return "locked (alarm or jog state)";
        case 10: return "soft limit";
        case 11: return "line overflow";
        case 14: return "line length exceeded";
        case 15: return "jog target exceeds travel";
        case 16: return "invalid jog command";
        case 17: return "laser mode requires PWM";
        case 20: return "unsupported G-code command";
        case 21: return "modal group violation";
        case 22: return "undefined feed rate";
        case 23: return "command value not an integer";
        case 24: return "two commands need axis words";
        case 25: return "repeated G-code word";
        case 26: return "no axis words found";
        default: return code == 0 ? "no code given" : "unlisted error number";
    }
}

}  // namespace jc8048w550
