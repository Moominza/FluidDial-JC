// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// SIMULATED FluidNC for positioning and probing routines (Instruction 009A). Pure RAM model, no I/O, no
// transport: it is fed command bytes and returns reply lines. Used by the host tests and by the
// non-motion preview, so the touchscreen rehearsal and the tests exercise the same behaviour.
//
// It models machine position, an active WCS and the G54..G59 / G92 offset table, G0 / G90 / G91 motion with
// FluidNC-like timing (G0 acknowledged before it finishes, G38.2 acknowledged when it finishes), a probe-able
// feature (bore, boss, rotary cylinder along X), `$G`, `$#`, `G10 L2/L20` and fault injection. It accepts any
// text; the shared command policy is applied by the transport in front of it, not here.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace jc8048w550 {

enum class JcSimFeature : uint8_t { None, Bore, Boss, Cylinder };

class JcSimNc {
public:
    static constexpr uint8_t kLines = 24;
    static constexpr uint8_t kPending = 16;
    static constexpr size_t kLineLen = 96;

    // ---- configuration (public on purpose: tests and the preview set the scene) ----
    float m[3] = { 50.0f, 50.0f, -5.0f };    // machine position
    float off[9][3] = {};                    // 0..5 = G54..G59, 6 = G28, 7 = G30, 8 = G92
    int   wcs = 0;
    char  state[16] = "Idle";
    char  pins[8] = "";
    float spindle = 0.0f;
    JcSimFeature feat = JcSimFeature::Bore;
    float xlo = 40, xhi = 60, ylo = 40, yhi = 60;          // bore / boss surfaces (machine coordinates)
    float tip = 1.0f;                                      // the REAL stylus radius
    float yc = 55.0f, zc = -30.0f, dia = 20.0f, tilt_y = 0.0f, tilt_z = 0.0f, x0 = 50.0f;   // cylinder along X
    bool  silent = false, ignore_g10 = false;
    int   alarm_on_probe = 0;      // 1-based: that probe raises ALARM:4
    int   error_on_cmd = 0;        // 1-based: that command is answered with error:9
    int   switch_wcs_after = 0;    // after this many commands the active WCS silently becomes G55
    uint32_t now = 1000;
    int   probes = 0, cmds = 0;

    float work(int a) const { return m[a] - off[wcs][a] - off[8][a]; }

    // Centre the feature on the current position (the preview has no fixed geometry).
    void centre_feature_here(JcSimFeature f, float half_width, float cyl_dia) {
        feat = f;
        xlo = m[0] - half_width; xhi = m[0] + half_width;
        ylo = m[1] - half_width; yhi = m[1] + half_width;
        if (f == JcSimFeature::Boss) { xlo = m[0] + 6.0f; xhi = m[0] + 6.0f + 2.0f * half_width; ylo = m[1] + 6.0f; yhi = m[1] + 6.0f + 2.0f * half_width; }
        if (f == JcSimFeature::Cylinder) { dia = cyl_dia; yc = m[1] + 6.0f + cyl_dia / 2.0f; zc = m[2] - 2.0f - cyl_dia / 2.0f; x0 = m[0]; tilt_y = tilt_z = 0.0f; }
    }

    // Command bytes as written by the transport: one real-time byte, or one line ending in '\n'.
    bool send(const char* bytes, size_t n) {
        if (n == 0) return false;
        if (n == 1) {
            if (bytes[0] == '!') { moving_ = false; set_state("Hold"); npend_ = 0; }
            return true;
        }
        char line[kLineLen];
        if (n >= sizeof(line)) return false;
        const size_t len = bytes[n - 1] == '\n' ? n - 1 : n;
        memcpy(line, bytes, len);
        line[len] = '\0';
        command(line);
        return true;
    }

    void update(uint32_t t) {
        now = t;
        if (moving_) {
            const float frac = t >= t1_ ? 1.0f : static_cast<float>(t - t0_) / static_cast<float>(t1_ - t0_);
            for (int a = 0; a < 3; ++a) m[a] = from_[a] + (to_[a] - from_[a]) * frac;
            if (t >= t1_) { moving_ = false; if (strcmp(state, "Run") == 0) set_state("Idle"); }
        }
        for (uint8_t i = 0; i < npend_;) {
            if (static_cast<int32_t>(t - pend_[i].at) >= 0) {
                push_line(pend_[i].text);
                if (pend_[i].alarm) set_state("Alarm");
                for (uint8_t k = i; k + 1 < npend_; ++k) pend_[k] = pend_[k + 1];
                --npend_;
            } else {
                ++i;
            }
        }
    }

    // Next reply line, if any.
    bool pop_line(char* out, size_t size) {
        if (nlines_ == 0) return false;
        snprintf(out, size, "%s", lines_[head_]);
        head_ = static_cast<uint8_t>((head_ + 1) % kLines);
        --nlines_;
        return true;
    }
    bool busy() const { return moving_ || npend_ > 0 || nlines_ > 0; }
    void set_state(const char* s) { snprintf(state, sizeof(state), "%s", s); }
    void clear_pending() { npend_ = 0; nlines_ = 0; head_ = 0; moving_ = false; }

private:
    struct Pending { uint32_t at; char text[kLineLen]; bool alarm; };

    void push_line(const char* s) {
        if (nlines_ >= kLines) return;
        snprintf(lines_[(head_ + nlines_) % kLines], kLineLen, "%s", s);
        ++nlines_;
    }
    void later(uint32_t ms, const char* text, bool alarm = false) {
        if (npend_ >= kPending) return;
        pend_[npend_].at = now + ms;
        snprintf(pend_[npend_].text, kLineLen, "%s", text);
        pend_[npend_].alarm = alarm;
        ++npend_;
    }

    void move_to(const float t[3], float mm_min, uint32_t ack_ms, bool ack_at_end, const char* extra) {
        float d = 0;
        for (int a = 0; a < 3; ++a) d += (t[a] - m[a]) * (t[a] - m[a]);
        d = sqrtf(d);
        const uint32_t dur = static_cast<uint32_t>(d / (mm_min > 1 ? mm_min : 1) * 60000.0f) + 60;
        memcpy(from_, m, sizeof(from_)); memcpy(to_, t, sizeof(to_));
        t0_ = now; t1_ = now + dur; moving_ = true; set_state("Run");
        const uint32_t at = ack_at_end ? dur : ack_ms;
        if (extra != nullptr && extra[0] != '\0') later(at, extra);
        later(at, "ok");
    }

    bool hit(int axis, int dir, float dist, float& stop) const {
        const float s = m[axis];
        auto reach = [&](float wall, bool need_below) -> bool {
            if (need_below ? !(s < wall) : !(s > wall)) return false;
            if (fabsf(wall - s) > dist) return false;
            stop = wall;
            return true;
        };
        switch (feat) {
            case JcSimFeature::None: return false;
            case JcSimFeature::Bore:
                if (axis == 0) return dir > 0 ? reach(xhi - tip, true) : reach(xlo + tip, false);
                if (axis == 1) return dir > 0 ? reach(yhi - tip, true) : reach(ylo + tip, false);
                if (axis == 2 && dir < 0) return reach(zc - 4.0f + tip, false);   // a floor, for a Z probe
                return false;
            case JcSimFeature::Boss:
                if (axis == 0) return dir > 0 ? reach(xlo - tip, true) : reach(xhi + tip, false);
                if (axis == 1) return dir > 0 ? reach(ylo - tip, true) : reach(yhi + tip, false);
                return false;
            case JcSimFeature::Cylinder: {
                const float yc_here = yc + tilt_y * (m[0] - x0), zc_here = zc + tilt_z * (m[0] - x0);
                if (axis == 1) return dir > 0 ? reach(yc_here - dia / 2 - tip, true) : reach(yc_here + dia / 2 + tip, false);
                if (axis == 2 && dir < 0 && fabsf(m[1] - yc_here) < dia / 2) return reach(zc_here + dia / 2 + tip, false);
                return false;
            }
        }
        return false;
    }

    void command(const char* line) {
        ++cmds;
        if (switch_wcs_after > 0 && cmds == switch_wcs_after) wcs = 1;
        if (silent) return;
        if (error_on_cmd == cmds) { later(30, "error:9"); return; }
        if (strcmp(line, "$G") == 0) {
            char g[kLineLen];
            snprintf(g, sizeof(g), "[GC:G0 G%d G17 G21 G90 G94 M5 M9 T0 F0 S0]", 54 + wcs);
            later(20, g);
            later(20, "ok");
            return;
        }
        if (strcmp(line, "$#") == 0) {
            static const char* const tags[9] = { "G54", "G55", "G56", "G57", "G58", "G59", "G28", "G30", "G92" };
            for (int i = 0; i < 9; ++i) {
                char b[kLineLen];
                snprintf(b, sizeof(b), "[%s:%.3f,%.3f,%.3f,0.000]", tags[i], static_cast<double>(off[i][0]),
                         static_cast<double>(off[i][1]), static_cast<double>(off[i][2]));
                later(20, b);
            }
            later(20, "[TLO:0.000]");
            later(20, "ok");
            return;
        }
        if (strcmp(line, "G90") == 0) { later(20, "ok"); return; }
        if (line[0] == 'G' && line[1] == '5' && line[2] >= '4' && line[2] <= '9' && line[3] == '\0') { wcs = line[2] - '4'; later(20, "ok"); return; }
        if (strncmp(line, "G10 L2 P", 8) == 0 || strncmp(line, "G10 L20 P", 9) == 0) {
            const bool l20 = line[6] == '0';
            const int n = (l20 ? line[9] : line[8]) - '1';
            if (!ignore_g10 && n >= 0 && n < 6) {
                const char* p = line + (l20 ? 10 : 9);
                while (*p) {
                    if (*p == ' ') { ++p; continue; }
                    const char ax = *p++;
                    const float v = strtof(p, nullptr);
                    while (*p && *p != ' ') ++p;
                    const int a = ax == 'X' ? 0 : (ax == 'Y' ? 1 : 2);
                    // L2: origin = value (machine coordinates). L20: the current position becomes `value`.
                    off[n][a] = l20 ? (m[a] - off[8][a] - v) : v;
                }
            }
            later(25, "ok");
            return;
        }
        const bool probe = strstr(line, "G38.2") != nullptr;
        const bool rel = strstr(line, "G91") != nullptr;
        const bool absmove = strstr(line, "G90") != nullptr;
        float tgt[3] = { m[0], m[1], m[2] };
        const char* fp = strstr(line, " F");
        const float f = fp != nullptr ? strtof(fp + 2, nullptr) : 3000.0f;
        int axis = -1;
        float val = 0;
        for (const char* p = line; *p; ++p) {
            if ((*p == 'X' || *p == 'Y' || *p == 'Z') && (p == line || p[-1] == ' ')) {
                const int a = *p == 'X' ? 0 : (*p == 'Y' ? 1 : 2);
                const float v = strtof(p + 1, nullptr);
                axis = a;
                val = v;
                if (rel) tgt[a] = m[a] + v;
                else if (absmove) tgt[a] = v + off[wcs][a] + off[8][a];
            }
        }
        if (probe) {
            ++probes;
            if (alarm_on_probe == probes) { later(30, "ALARM:4", true); return; }
            if (strchr(pins, 'P') != nullptr) { later(30, "ALARM:5", true); return; }
            const int dir = val > 0 ? 1 : -1;
            float stop = 0;
            if (axis >= 0 && hit(axis, dir, fabsf(val), stop)) {
                tgt[axis] = stop;
                char prb[kLineLen];
                snprintf(prb, sizeof(prb), "[PRB:%.3f,%.3f,%.3f:1]", static_cast<double>(axis == 0 ? stop : m[0]),
                         static_cast<double>(axis == 1 ? stop : m[1]), static_cast<double>(axis == 2 ? stop : m[2]));
                move_to(tgt, f, 0, true, prb);
            } else {
                later(400, "ALARM:4", true);
            }
            return;
        }
        move_to(tgt, f, 30, false, nullptr);
    }

    float    from_[3] = {}, to_[3] = {};
    uint32_t t0_ = 0, t1_ = 0;
    bool     moving_ = false;
    Pending  pend_[kPending];
    uint8_t  npend_ = 0;
    char     lines_[kLines][kLineLen];
    uint8_t  head_ = 0, nlines_ = 0;
};

}  // namespace jc8048w550
