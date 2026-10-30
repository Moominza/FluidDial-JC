// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Tool state as FluidNC reports it (Instruction 010): the current tool number from `$G` ([GC:... T<n> ...]) and the
// tool length offset from `$#` ([TLO:<mm>]), read with the two read-only queries. Also the small amount of tool
// bookkeeping the pendant keeps (the reference contact), which is valid only while the machine position it was
// measured in cannot have changed.
//
// What this does NOT assume: an automatic tool changer or a configured M6 macro. M6 / T words are never sent.
// Tool length compensation here is G43.1 (dynamic offset) / G49; it is separate from the work offsets (G54..G59).
//
// Pure: no I/O.

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "JcProbeCalc.hpp"
#include "JcQuery.hpp"

namespace jc8048w550 {

// "G0 G54 G17 G21 G90 G94 G43.1 M5 M9 T3 F0 S0" -> tool 3; *tlo_active tells whether G43.1 is in the modal state.
// Returns -1 when no T word is present.
inline int jc_gc_tool(const char* gc, bool* tlo_active = nullptr) {
    int tool = -1;
    bool g431 = false;
    for (const char* p = gc; p != nullptr && *p;) {
        while (*p == ' ') ++p;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        const size_t len = static_cast<size_t>(p - start);
        if (len == 0) break;
        if (start[0] == 'T' && len >= 2 && len <= 4) {
            char* end = nullptr;
            const long v = strtol(start + 1, &end, 10);
            if (end == start + len && v >= 0 && v <= 999) tool = static_cast<int>(v);
        }
        if (len == 5 && strncmp(start, "G43.1", 5) == 0) g431 = true;
    }
    if (tlo_active != nullptr) *tlo_active = g431;
    return tool;
}

struct JcToolInfo {
    bool  gc_known = false;     // `$G` answered
    int   tool = -1;            // the controller's current tool number (a T word, not a physical identification)
    bool  tlo_modal = false;    // G43.1 appears in the modal state
    bool  tlo_known = false;    // `[TLO:]` answered (the controller has the tool-offset machinery)
    float tlo = 0.0f;           // mm
};

enum class JcToolReadState : uint8_t { Idle, Reading, Done, Failed };

class JcToolReader {
public:
    using QueryFn = bool (*)(void* user, const char* line);
    void set_query(QueryFn fn, void* user) { _fn = fn; _user = user; }

    bool start(uint32_t now) {
        if (_state == JcToolReadState::Reading) return false;
        _info = JcToolInfo();
        _reason[0] = '\0';
        _state = JcToolReadState::Reading;
        _phase = 0;
        _t = now;
        return true;
    }
    void service(uint32_t now) {
        if (_state != JcToolReadState::Reading) return;
        if (_phase == 0 || _phase == 2) {
            const char* q = _phase == 0 ? kJcQueryModalState : kJcQueryOffsets;
            if (_fn == nullptr || !_fn(_user, q)) { fail("could not send the read-only query"); return; }
            _phase = static_cast<uint8_t>(_phase + 1);
            _t = now;
        } else if (now - _t > 4000u) {
            fail(_phase == 1 ? "no reply to $G" : "no reply to $#");
        }
    }
    void on_line(const char* line) {
        if (_state != JcToolReadState::Reading || (_phase != 1 && _phase != 3) || line == nullptr) return;
        if (strncmp(line, "[GC:", 4) == 0 && _phase == 1) {
            _info.tool = jc_gc_tool(line + 4, &_info.tlo_modal);
            _info.gc_known = true;
        } else if (strncmp(line, "[TLO:", 5) == 0 && _phase == 3) {
            char payload[64];
            snprintf(payload, sizeof(payload), "%s", line + 5);
            const size_t pl = strlen(payload);
            if (pl > 0 && payload[pl - 1] == ']') payload[pl - 1] = '\0';
            float v = 0.0f;
            if (jc_parse_tlo_z(payload, &v)) { _info.tlo = v; _info.tlo_known = true; }
        } else if (strcmp(line, "ok") == 0) {
            if (_phase == 1) {
                if (!_info.gc_known) { fail("$G did not report a modal state"); return; }
                _phase = 2;
            } else {
                if (!_info.tlo_known) { fail("the controller did not report [TLO:] - tool length offsets not available"); return; }
                _state = JcToolReadState::Done;
            }
        } else if (strncmp(line, "error:", 6) == 0) {
            char m[64];
            snprintf(m, sizeof(m), "the controller refused the query (%s)", line);
            fail(m);
        }
    }
    void invalidate() { _info = JcToolInfo(); _state = JcToolReadState::Idle; _reason[0] = '\0'; }
    void on_link_lost() { invalidate(); }
    // After a tool length offset was applied and verified by the workflow, keep the display consistent.
    void note_tlo(float v, bool modal) { _info.tlo = v; _info.tlo_known = true; _info.tlo_modal = modal; }

    JcToolReadState state() const { return _state; }
    const JcToolInfo& info() const { return _info; }
    const char* reason() const { return _reason; }
    bool reading() const { return _state == JcToolReadState::Reading; }

private:
    void fail(const char* why) { snprintf(_reason, sizeof(_reason), "%s", why); _state = JcToolReadState::Failed; }
    QueryFn         _fn = nullptr;
    void*           _user = nullptr;
    JcToolReadState _state = JcToolReadState::Idle;
    JcToolInfo      _info;
    uint8_t         _phase = 0;
    uint32_t        _t = 0;
    char            _reason[72] = "";
};

// The reference contact: the machine Z at which the REFERENCE tool touched. It is meaningful only while the
// machine position cannot have shifted, so any loss of the connection, alarm or reset discards it.
struct JcToolStore {
    bool  has_ref = false;
    float ref_mz = 0.0f;
    float ref_plate_thickness = 0.0f;
    int   ref_dir = -1;
    char  invalid_reason[48] = "";
    bool  tlo_verified = false;    // a G43.1 / [TLO:] round trip matched at least once this connection
    bool  simulated = false;       // measured against the SIMULATED controller (ordinary firmware rehearsal)

    void set_ref(float mz, float plate, int dir, bool sim = false) { simulated = sim; has_ref = true; ref_mz = mz; ref_plate_thickness = plate; ref_dir = dir; invalid_reason[0] = '\0'; }
    void invalidate(const char* why) {
        if (has_ref) snprintf(invalid_reason, sizeof(invalid_reason), "%s", why);
        has_ref = false;
        tlo_verified = false;
        simulated = false;
    }
};

}  // namespace jc8048w550
