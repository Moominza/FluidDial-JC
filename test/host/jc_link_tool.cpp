// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Command-driven host tool wrapping the REAL JC frame assembler, status parser,
// machine model and link tracker. tools/run_sim_integration.py feeds it events
// captured from a real TCP session with tools/sim_fluidnc_server.py.
//
// stdin commands (one per line):
//   T <ms>            set the clock
//   CONNECT           operator requests a connection (link comes up)
//   DATA <hex>        bytes received from the transport
//   CLOSE             unexpected connection closure
//   RETRY             ask whether a reconnect attempt is due now (prints 0/1)
//   TX <hex>          report whether each outgoing byte would be permitted
//   Q                 print a one-line state summary

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/jc8048w550/JcFrameAssembler.hpp"
#include "../../src/jc8048w550/JcLinkTracker.hpp"
#include "../../src/jc8048w550/JcMachineModel.hpp"
#include "../../src/jc8048w550/JcStatusFrame.hpp"

using namespace jc8048w550;

static const char* state_name(JcFluidState s) {
    switch (s) {
        case JcFluidState::Disconnected: return "Disconnected";
        case JcFluidState::FluidNcConnecting: return "Connecting";
        case JcFluidState::Connected: return "Connected";
        case JcFluidState::Lost: return "Lost";
        case JcFluidState::Stale: return "Stale";
        case JcFluidState::InvalidData: return "InvalidData";
        default: return "Other";
    }
}

int main() {
    JcFrameAssembler assembler;
    JcLinkTracker tracker;
    JcMachineModel model;
    uint32_t now = 0, sequence = 0;
    char line[4096];

    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'T' && line[1] == ' ') {
            now = static_cast<uint32_t>(strtoul(line + 2, nullptr, 10));
        } else if (strncmp(line, "CONNECT", 7) == 0) {
            tracker.request_connect(now);
            tracker.on_connected();
        } else if (strncmp(line, "CLOSE", 5) == 0) {
            assembler.reset();
            tracker.on_closed(now);
        } else if (strncmp(line, "RETRY", 5) == 0) {
            printf("%d\n", tracker.should_retry(now) ? 1 : 0);
        } else if (strncmp(line, "TX", 2) == 0) {
            int ok = 1;
            for (const char* p = line + 3; p[0] && p[1] && p[0] != '\n'; p += 2) {
                char hex[3] = { p[0], p[1], 0 };
                if (!jc_tx_byte_allowed(static_cast<uint8_t>(strtoul(hex, nullptr, 16)))) ok = 0;
            }
            printf("%d\n", ok);
        } else if (strncmp(line, "DATA", 4) == 0) {
            for (const char* p = line + 5; p[0] && p[1] && p[0] != '\n'; p += 2) {
                char hex[3] = { p[0], p[1], 0 };
                switch (assembler.push(static_cast<uint8_t>(strtoul(hex, nullptr, 16)))) {
                    case JcFrameAssembler::Result::None: break;
                    case JcFrameAssembler::Result::Invalid: tracker.on_invalid(); break;
                    case JcFrameAssembler::Result::Frame: {
                        JcStatusFrame f {};
                        if (!jc_parse_status_frame(assembler.frame(), assembler.length(), f)) {
                            tracker.on_invalid();
                            break;
                        }
                        JcLiveMachineStatus s {};
                        s.has_data = true;
                        strcpy(s.machine_state, f.state);
                        s.coordinate_reference = f.machine_position ? JcCoordinateReference::Machine
                                                                    : JcCoordinateReference::Work;
                        memcpy(s.axes, f.pos, sizeof(s.axes));
                        s.feed = f.feed;
                        s.spindle = f.spindle;
                        s.has_wco = f.has_wco;
                        memcpy(s.wco, f.wco, sizeof(s.wco));
                        s.last_valid_ms = now;
                        s.sequence = ++sequence;
                        model.on_status(s);
                        tracker.on_frame(now);
                        break;
                    }
                }
            }
        } else if (line[0] == 'Q') {
            float m[4] = { 0, 0, 0, 0 }, w[4] = { 0, 0, 0, 0 };
            bool have_m = true, have_w = true;
            for (uint8_t i = 0; i < 4; ++i) {
                have_m = have_m && model.position(i, JcRef::Machine, m[i]);
                have_w = have_w && model.position(i, JcRef::Work, w[i]);
            }
            printf("state=%s fresh=%d frames=%u invalid=%u lost=%u machine=%s", state_name(tracker.state(now)),
                   tracker.fresh(now) ? 1 : 0, tracker.frames(), tracker.invalid_count(), tracker.lost_count(),
                   model.state());
            if (have_m) printf(" M=%.3f,%.3f,%.3f,%.3f", m[0], m[1], m[2], m[3]);
            else printf(" M=n/a");
            if (have_w) printf(" W=%.3f,%.3f,%.3f,%.3f", w[0], w[1], w[2], w[3]);
            else printf(" W=n/a");
            printf(" feed=%.0f spindle=%.0f\n", model.feed(), model.spindle());
        }
        fflush(stdout);
    }
    return 0;
}
