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

#include "JcCommandAuth.hpp"

namespace jc8048w550 {

enum class JcSimFeature : uint8_t { None, Bore, Boss, Cylinder, Plate };

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

    // ---- SD card and job (Instruction 010) ----
    struct SimFile { char path[96]; uint32_t size; };
    static constexpr int kMaxFiles = 220;
    bool  sd_present = true;
    bool  sd_status_supported = true;      // false: "$SD/Status" is answered error:3
    bool  list_dir_lines = false;          // also emit explicit [DIR:..] lines
    bool  list_full_paths = false;         // tolerance test: the old full-path entries instead of FluidNC's relative, indented ones
    bool  list_space_after_colon = false;  // with full paths: "[FILE: /x|SIZE:n]" instead of "[FILE:/x|SIZE:n]"
    int   sd_list_error = 0;               // that error number answers $SD/List
    int   run_error = 0;                   // that error number answers $SD/Run
    bool  list_never_ends = false;         // no "ok" after the listing
    SimFile files[kMaxFiles];
    int   nfiles = 0;
    bool  job = false, job_hold = false;
    float job_pct = 0.0f;
    char  job_name[96] = "";
    uint32_t job_total_ms = 10000;
    int   job_alarm_at_pct = 0;            // >0: the job ends in ALARM:1 at that percentage
    int   resets = 0;
    int   program_start_rx = 0;            // program start / resume bytes or lines that reached the "controller" (must stay 0)

    // What the PC does (the owner starts and resumes programs there; never the pendant). Test and preview scaffolding
    // only: these are NOT reachable through send().
    bool pc_start_job(const char* path) {
        if (!sd_present || job) return false;
        bool found = false;
        for (int i = 0; i < nfiles; ++i) if (strcmp(files[i].path, path) == 0) found = true;
        if (!found) return false;
        job = true; job_hold = false; job_pct = 0.0f;
        snprintf(job_name, sizeof(job_name), "%s", path);
        set_state("Run");
        return true;
    }
    void pc_resume_job() { if (job && job_hold) { job_hold = false; set_state("Run"); } }
    void add_file(const char* path, uint32_t size) {
        if (nfiles >= kMaxFiles) return;
        snprintf(files[nfiles].path, sizeof(files[nfiles].path), "%s", path);
        files[nfiles].size = size;
        ++nfiles;
    }

    // ---- tool (Instruction 010) ----
    int   tool = 0;
    float tlo = 0.0f;
    bool  tlo_active = false;
    bool  ignore_tlo = false;              // G43.1 / G49 are acknowledged but not applied
    bool  m6_supported = false;            // false: T / M6 answered error:20 (no tool changer / macro configured)
    float plate_z = -40.0f;                // machine Z of the touch plate surface (Plate feature)
    float tool_len = 20.0f;                // how far the tool tip extends below the spindle datum (longer => contact higher)

    float work(int a) const { return m[a] - off[wcs][a] - off[8][a] - (a == 2 && tlo_active ? tlo : 0.0f); }

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
            if (bytes[0] == '!') {
                moving_ = false; npend_ = 0;
                if (job) { job_hold = true; set_state("Hold:0"); } else set_state("Hold");
            } else if (bytes[0] == '~') {
                ++program_start_rx;   // the pendant must never send this (Instruction 010A); the simulator ignores it
            } else if (static_cast<uint8_t>(bytes[0]) == 0x18) {
                ++resets;
                job = false; job_hold = false; moving_ = false; npend_ = 0;
                set_state("Idle");
            }
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

    // A '<...>' status report as FluidNC sends it (work position + FS, SD progress while a job runs).
    void status_frame(char* out, size_t size) const {
        char sd[128] = "";
        if (job) snprintf(sd, sizeof(sd), "|SD:%.2f,%s", static_cast<double>(job_pct), job_name);
        char pn[16] = "";
        if (pins[0] != '\0') snprintf(pn, sizeof(pn), "|Pn:%s", pins);
        snprintf(out, size, "<%s|WPos:%.3f,%.3f,%.3f,0.000|FS:0,%d%s%s>", state, static_cast<double>(work(0)),
                 static_cast<double>(work(1)), static_cast<double>(work(2)), static_cast<int>(spindle), pn, sd);
    }

    void update(uint32_t t) {
        if (job && !job_hold && t > now) {
            job_pct += 100.0f * static_cast<float>(t - now) / static_cast<float>(job_total_ms);
            if (job_alarm_at_pct > 0 && job_pct >= static_cast<float>(job_alarm_at_pct)) {
                job = false; job_hold = false; set_state("Alarm"); push_line("ALARM:1");
            } else if (job_pct >= 100.0f) {
                job_pct = 100.0f; job = false; set_state("Idle");
            }
        }
        now = t;
        if (moving_) {
            const float frac = t >= t1_ ? 1.0f : static_cast<float>(t - t0_) / static_cast<float>(t1_ - t0_);
            for (int a = 0; a < 3; ++a) m[a] = from_[a] + (to_[a] - from_[a]) * frac;
            if (t >= t1_) { moving_ = false; if (strcmp(state, "Run") == 0) set_state("Idle"); }
        }
        while (listing_ && t >= list_at_ && nlines_ < kLines - 2) {
            char b[kLineLen + 24];
            bool have = false;
            if (list_full_paths) {
                if (list_i_ < nfiles) {
                    snprintf(b, sizeof(b), "[FILE:%s%s|SIZE:%u]", list_space_after_colon ? " " : "", files[list_i_].path, static_cast<unsigned>(files[list_i_].size));
                    have = true;
                }
            } else {
                have = tree_line(list_i_, b, sizeof(b));
            }
            if (have) {
                push_line(b);
                ++list_i_;
            } else {
                if (list_dir_lines) push_line("[DIR:/sim_dir]");
                if (!list_never_ends) {
                    if (!list_full_paths) push_line("[/sd Free:14.57 GB Used:25.72 MB Total:14.59 GB]");   // FluidNC's card summary
                    push_line("ok");
                }
                listing_ = false;
            }
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
    // Line `target` of FluidNC's listing: a folder as "[DIR:<depth spaces>name]", then its entries; a file as
    // "[FILE:<depth + 1 spaces>name|SIZE:n]". Walks the stored full paths; false when `target` is past the end.
    bool tree_line(int target, char* out, size_t size) const {
        int counter = 0;
        return walk("", 0, counter, target, out, size);
    }
    bool walk(const char* prefix, int depth, int& counter, int target, char* out, size_t size) const {
        const size_t pl = strlen(prefix);
        for (int i = 0; i < nfiles; ++i) {
            const char* path = files[i].path;
            if (strncmp(path, prefix, pl) != 0 || path[pl] != '/') continue;
            const char* rel = path + pl + 1;
            const char* slash = strchr(rel, '/');
            if (slash == nullptr) {
                if (counter++ == target) {
                    snprintf(out, size, "[FILE:%*s%s|SIZE:%u]", depth + 1, "", rel, static_cast<unsigned>(files[i].size));
                    return true;
                }
            } else {
                const size_t nl = static_cast<size_t>(slash - rel);
                bool seen = false;
                for (int j = 0; j < i && !seen; ++j) seen = strncmp(files[j].path, path, pl + 1 + nl + 1) == 0;
                if (seen) continue;
                char sub[kLineLen];
                snprintf(sub, sizeof(sub), "%.*s", static_cast<int>(pl + 1 + nl), path);
                if (counter++ == target) {
                    snprintf(out, size, "[DIR:%*s%.*s]", depth, "", static_cast<int>(nl), rel);
                    return true;
                }
                if (walk(sub, depth + 1, counter, target, out, size)) return true;
            }
        }
        return false;
    }

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
            case JcSimFeature::Plate:
                if (axis == 2) return dir < 0 ? reach(plate_z + tool_len, false) : reach(plate_z + tool_len, true);
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
            snprintf(g, sizeof(g), "[GC:G0 G%d G17 G21 G90 G94 %sM5 M9 T%d F0 S0]", 54 + wcs, tlo_active ? "G43.1 " : "", tool);
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
                if (i == 5) {   // FluidNC v4 also prints the three extra work coordinate systems
                    later(20, "[G59.1:0.000,0.000,0.000,0.000]");
                    later(20, "[G59.2:0.000,0.000,0.000,0.000]");
                    later(20, "[G59.3:0.000,0.000,0.000,0.000]");
                }
            }
            char t[kLineLen];
            snprintf(t, sizeof(t), "[TLO:0.000,0.000,%.3f,0.000]", static_cast<double>(tlo));   // one value per axis
            later(20, t);
            later(20, "ok");
            return;
        }
        if (strcmp(line, "$SD/Status") == 0) {
            if (!sd_status_supported) { later(20, "error:3"); return; }
            later(20, sd_present ? "SD card detected" : "No SD card detected");
            later(20, "ok");
            return;
        }
        if (strcmp(line, "$SD/List") == 0) {
            if (!sd_present) { later(20, "error:60"); return; }
            if (sd_list_error != 0) { char e[24]; snprintf(e, sizeof(e), "error:%d", sd_list_error); later(20, e); return; }
            listing_ = true; list_i_ = 0; list_at_ = now + 20;      // streamed by update(): a long list never overflows the buffers
            return;
        }
        if (line[0] == '$' && jc_is_program_start_text(line)) {   // "$SD/Run", "$Localfs/Run", "$Macro/..." and relatives
            ++program_start_rx;   // never sent by the pendant (Instruction 010A): counted so tests can prove it, then refused
            later(20, "error:20");
            return;
        }
        if (strcmp(line, "G49") == 0) { if (!ignore_tlo) { tlo = 0.0f; tlo_active = false; } later(20, "ok"); return; }
        if (strncmp(line, "G43.1 Z", 7) == 0) { if (!ignore_tlo) { tlo = strtof(line + 7, nullptr); tlo_active = true; } later(20, "ok"); return; }
        if (line[0] == 'T' || strncmp(line, "M6", 2) == 0) {
            if (!m6_supported) { later(20, "error:20"); return; }
            if (line[0] == 'T') tool = atoi(line + 1);
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
                else if (absmove) tgt[a] = v + off[wcs][a] + off[8][a] + (a == 2 && tlo_active ? tlo : 0.0f);
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
    bool     listing_ = false;
    int      list_i_ = 0;
    uint32_t list_at_ = 0;
    Pending  pend_[kPending];
    uint8_t  npend_ = 0;
    char     lines_[kLines][kLineLen];
    uint8_t  head_ = 0, nlines_ = 0;
};

}  // namespace jc8048w550
