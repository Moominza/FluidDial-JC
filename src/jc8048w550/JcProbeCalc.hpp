// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Probe reply parsing and the centre-finding arithmetic (Instruction 009A). Pure.
// FluidNC executes the probe; the pendant only reads its reply:
//   [PRB:x,y,z[,a]:1]   contact, machine coordinates of the contact point
//   [PRB:x,y,z[,a]:0]   no contact
//   ALARM:4 / ALARM:5   probe fail (no contact) / probe already triggered (grbl numbering)
// A centre is only ever computed from two COMPLETE contact records; failed, stale or partial
// measurements are rejected with a reason, never "repaired".

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "JcResponse.hpp"

namespace jc8048w550 {

struct JcProbeReport {
    bool  valid = false;      // a well-formed [PRB:...] record
    bool  contact = false;    // flag after the last colon: 1
    uint8_t axes = 0;         // number of coordinates present (3 or 4)
    float pos[4] = { 0, 0, 0, 0 };   // machine coordinates at contact
};

// Parses an Info response whose tag is PRB.
inline bool jc_parse_prb(const JcResponse& r, JcProbeReport& out) {
    out = JcProbeReport();
    if (r.kind != JcRespKind::Info || strcmp(r.key, "PRB") != 0) return false;
    const char* colon = strrchr(r.value, ':');
    if (colon == nullptr || (colon[1] != '0' && colon[1] != '1') || colon[2] != '\0') return false;
    char coords[72];
    const size_t n = static_cast<size_t>(colon - r.value);
    if (n == 0 || n >= sizeof(coords)) return false;
    memcpy(coords, r.value, n);
    coords[n] = '\0';
    uint8_t count = 0;
    const char* p = coords;
    while (*p != '\0') {
        char* end = nullptr;
        const float v = strtof(p, &end);
        if (end == p || !isfinite(v) || count >= 4 || fabsf(v) > 1.0e6f) return false;
        out.pos[count++] = v;
        p = end;
        if (*p == ',') ++p;
        else if (*p != '\0') return false;
    }
    if (count < 3) return false;
    out.axes = count;
    out.contact = colon[1] == '1';
    out.valid = true;
    return true;
}

// "[G54:0.000,0.000,0.000]" style offset records ($# reply). index: 0..5 = G54..G59, 6 = G28, 7 = G30, 8 = G92.
inline int jc_offset_tag_index(const char* tag) {
    if (tag == nullptr || tag[0] != 'G') return -1;
    if (strcmp(tag, "G28") == 0) return 6;
    if (strcmp(tag, "G30") == 0) return 7;
    if (strcmp(tag, "G92") == 0) return 8;
    if (tag[1] == '5' && tag[2] >= '4' && tag[2] <= '9' && tag[3] == '\0') return tag[2] - '4';
    return -1;
}

inline bool jc_parse_coords(const char* text, float out[4], uint8_t& count) {
    count = 0;
    for (int i = 0; i < 4; ++i) out[i] = 0.0f;
    const char* p = text;
    while (*p != '\0') {
        char* end = nullptr;
        const float v = strtof(p, &end);
        if (end == p || !isfinite(v) || count >= 4 || fabsf(v) > 1.0e6f) return false;
        out[count++] = v;
        p = end;
        if (*p == ',') ++p;
        else if (*p != '\0') return false;
    }
    return count >= 3;
}

// "[TLO:0.000,0.000,6.500,0.000]" (FluidNC v4 prints one value per axis) or the older single value: the Z offset.
inline bool jc_parse_tlo_z(const char* text, float* z) {
    float v[4];
    uint8_t n = 0;
    if (jc_parse_coords(text, v, n)) {
        *z = v[2];
        return true;
    }
    char* end = nullptr;
    const float one = strtof(text, &end);
    if (end != text && *end == '\0' && isfinite(one)) {
        *z = one;
        return true;
    }
    return false;
}

// Active WCS from a "[GC:G0 G54 G17 ...]" payload: 0..5, or -1.
inline int jc_wcs_from_gc(const char* gc) {
    if (gc == nullptr) return -1;
    for (const char* p = gc; *p; ++p) {
        if (p[0] == 'G' && p[1] == '5' && p[2] >= '4' && p[2] <= '9' && (p[3] == ' ' || p[3] == '\0')
            && (p == gc || p[-1] == ' ')) {
            return p[2] - '4';
        }
    }
    return -1;
}

// ---- centre arithmetic -------------------------------------------------------------------------------------

enum class JcFeatureKind : uint8_t { Inside, Outside };   // probe a bore / slot from inside, or a boss / stock from outside

struct JcCentre {
    bool  ok = false;
    bool  suspect = false;            // computed, but the measured size disagrees with the expected size
    char  why[96] = "";
    float centre = 0.0f;              // machine coordinate of the midpoint
    float size = 0.0f;                // feature width after tip-radius compensation
};

// `hi` is the contact reached probing in the + direction, `lo` the contact reached probing in the - direction
// (machine coordinates along the one axis). `tip_radius` is the probe stylus radius. `expected` (0 = none) is
// the operator's expected feature size and `tolerance` the allowed difference.
inline JcCentre jc_axis_centre(JcFeatureKind kind, bool hi_valid, float hi, bool lo_valid, float lo, float tip_radius,
                               float expected, float tolerance) {
    JcCentre c;
    if (!hi_valid || !lo_valid) { snprintf(c.why, sizeof(c.why), "missing or failed contact: no centre"); return c; }
    if (!isfinite(hi) || !isfinite(lo) || !isfinite(tip_radius) || tip_radius < 0.0f || tip_radius > 25.0f) {
        snprintf(c.why, sizeof(c.why), "probe radius not valid (0..25 mm)");
        return c;
    }
    // Inside: the stylus centre stops one radius short of each wall; outside: one radius short of each face.
    const float span = hi - lo;
    c.size = kind == JcFeatureKind::Inside ? span + 2.0f * tip_radius : span - 2.0f * tip_radius;
    if (span <= 0.0f || c.size <= 0.0f) {
        snprintf(c.why, sizeof(c.why), "contacts inconsistent (size %.3f): wrong feature type or radius", static_cast<double>(c.size));
        return c;
    }
    c.centre = 0.5f * (hi + lo);
    c.ok = true;
    if (expected > 0.0f && fabsf(c.size - expected) > tolerance) {
        c.suspect = true;
        snprintf(c.why, sizeof(c.why), "size %.3f differs from expected %.3f: check probe radius / feature",
                 static_cast<double>(c.size), static_cast<double>(expected));
    }
    return c;
}

// Rotary-axis centreline from stations along the rotation axis. Each station provides the cross-axis centre
// (from an outside pair of contacts) and the top contact of the cylinder. All values machine coordinates.
struct JcStation {
    bool  valid = false;
    float along = 0.0f;       // machine coordinate along the rotation axis where the station was probed
    float cross = 0.0f;       // centre of the cylinder on the horizontal cross axis
    float top_contact = 0.0f; // stylus-centre Z at contact on the cylinder top
    float diameter = 0.0f;    // diameter measured from the cross pair
};

struct JcCentreline {
    bool  ok = false;
    bool  suspect = false;
    char  why[72] = "";
    float cross = 0.0f;       // mean cross-axis centreline position
    float z = 0.0f;           // mean centreline height (machine Z)
    float tilt_cross = 0.0f;  // cross shift per mm along the axis (axis not parallel to the machine axis)
    float tilt_z = 0.0f;
    float diameter = 0.0f;
};

inline JcCentreline jc_rotary_centreline(const JcStation& a, const JcStation& b, float tip_radius, float cylinder_diameter,
                                         float tolerance) {
    JcCentreline r;
    if (!a.valid || !b.valid) { snprintf(r.why, sizeof(r.why), "both stations must be complete: no centreline"); return r; }
    if (!isfinite(tip_radius) || tip_radius < 0.0f || tip_radius > 25.0f || !(cylinder_diameter > 0.0f)) {
        snprintf(r.why, sizeof(r.why), "probe radius or cylinder diameter not valid");
        return r;
    }
    const float spacing = b.along - a.along;
    if (fabsf(spacing) < 1.0f) { snprintf(r.why, sizeof(r.why), "stations too close to judge alignment (< 1 mm)"); return r; }
    const float za = a.top_contact - tip_radius - 0.5f * cylinder_diameter;
    const float zb = b.top_contact - tip_radius - 0.5f * cylinder_diameter;
    r.cross = 0.5f * (a.cross + b.cross);
    r.z = 0.5f * (za + zb);
    r.tilt_cross = (b.cross - a.cross) / spacing;
    r.tilt_z = (zb - za) / spacing;
    r.diameter = 0.5f * (a.diameter + b.diameter);
    r.ok = true;
    if (fabsf(r.diameter - cylinder_diameter) > tolerance) {
        r.suspect = true;
        snprintf(r.why, sizeof(r.why), "measured diameter %.3f differs from %.3f", static_cast<double>(r.diameter),
                 static_cast<double>(cylinder_diameter));
    }
    return r;
}

}  // namespace jc8048w550
