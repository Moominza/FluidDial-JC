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
//   LOAD              operator starts a read-only capability read (prints started/refused)
//   NEXT              print the next allowlisted query line to send, or '-'
//   SENT              the line printed by NEXT was transmitted
//   TICK              service timeouts at the current clock
//   LOST              connection lost (also closes the link tracker)
//   LSUM              print a one-line summary of the capability read
//   QOK <line>        print 1 if the line would be accepted by the query allowlist
//
// First-live-jog commands (Instruction 006). Meaningful when the tool is built with
// -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_LIVE_JOG_TEST=1; in an ordinary build the gate
// blocks every one of them. The tool never touches a socket: bytes the session wants
// to transmit are collected and printed by JTX, and the Python harness forwards them
// to the simulator.
//   JARM              operator arms the jog test and acknowledges "no controller travel
//                     protection" for X and Y (lapses by itself)
//   JARMX             armed WITHOUT that acknowledgement
//   JOG <a> <d> <f>   request an incremental jog (axis 0..3, signed distance, feed)
//   JCANCEL           request the jog-cancel byte
//   JTX               print (and clear) the hex bytes the session transmitted, or '-'
//   JSUM              print a one-line summary of the jog session
//   JACK              operator acknowledges a failure result (prints 1/0)
//   JSTALE            the status stream went stale while the socket is still open

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/jc8048w550/JcCapsLoader.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcJogTest.hpp"
#include "../../src/jc8048w550/JcFrameAssembler.hpp"
#include "../../src/jc8048w550/JcLinkTracker.hpp"
#include "../../src/jc8048w550/JcMachineModel.hpp"
#include "../../src/jc8048w550/JcStatusFrame.hpp"
#include "../../src/jc8048w550/JcStreamSplitter.hpp"

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

static char g_tx_hex[4096];
static size_t g_tx_len = 0;

static bool tool_tx(void*, const char* bytes, size_t n) {
    for (size_t i = 0; i < n && g_tx_len + 2 < sizeof(g_tx_hex); ++i) {
        g_tx_len += static_cast<size_t>(snprintf(g_tx_hex + g_tx_len, sizeof(g_tx_hex) - g_tx_len, "%02x",
                                                 static_cast<unsigned>(static_cast<uint8_t>(bytes[i]))));
    }
    return true;
}

static const char* cmd_error_token(JcCmdError e) {
    switch (e) {
        case JcCmdError::None: return "None";
        case JcCmdError::BadAxis: return "BadAxis";
        case JcCmdError::BadValue: return "BadValue";
        case JcCmdError::BadFeed: return "BadFeed";
        case JcCmdError::Unsupported: return "Unsupported";
        case JcCmdError::BadPath: return "BadPath";
        case JcCmdError::BadIndex: return "BadIndex";
        case JcCmdError::JogDisabled: return "JogDisabled";
        case JcCmdError::TestLimit: return "TestLimit";
    }
    return "?";
}

static void print_token(const char* text) {
    for (const char* p = text; *p; ++p) putchar(*p == ' ' ? '_' : *p);
}

int main() {
    JcStreamSplitter assembler;
    JcCapsLoader loader;
    uint32_t lines_seen = 0;
    JcLinkTracker tracker;
    JcMachineModel model;
    JcJogSession session(&tool_tx, nullptr);
    JcDispatcher dispatcher;
    dispatcher.set_sink_ex(&JcJogSession::sink, &session);
    uint32_t now = 0, sequence = 0;
    char line[4096];

    auto context = [&](bool confirmed) {
        JcDispatchContext c;
        c.live = true;
        c.link_fresh = tracker.fresh(now);
        c.machine_state = model.state();
        c.operator_confirmed = confirmed;
        c.now_ms = now;
        c.limits_mask = 0x0F;     // the capability read is covered by its own scenarios
        c.envelope_mask = 0x0F;
        c.operation_pending = session.unresolved();
        return c;
    };

    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'T' && line[1] == ' ') {
            now = static_cast<uint32_t>(strtoul(line + 2, nullptr, 10));
            session.set_clock(now);
        } else if (strncmp(line, "JARMX", 5) == 0) {
            dispatcher.arm_for(now, kJcJogArmTtlMs, 0);   // armed, but "no travel protection" NOT acknowledged
        } else if (strncmp(line, "JARM", 4) == 0) {
            dispatcher.arm_for(now, kJcJogArmTtlMs, 0x03);   // armed + acknowledged for X and Y (never Z or A)
        } else if (strncmp(line, "JOG ", 4) == 0) {
            unsigned axis = 0;
            float dist = 0.0f, feed = 0.0f;
            if (sscanf(line + 4, "%u %f %f", &axis, &dist, &feed) != 3) {
                printf("usage\n");
            } else {
                JcMachineProfile p = jc_default_profile();
                jc_profile_apply_test_limits(p);
                const JcCommand c = jc_jog_incremental(p, static_cast<uint8_t>(axis), dist, feed);
                if (!c.ok) {
                    printf("rejected:%s\n", cmd_error_token(c.error));
                } else {
                    session.set_clock(now);
                    const uint32_t before = dispatcher.sent();
                    const JcAuthDecision d = dispatcher.submit(c, context(true));
                    printf("decision=");
                    print_token(jc_auth_name(d));
                    printf(" sent=%d\n", dispatcher.sent() != before ? 1 : 0);
                }
            }
        } else if (strncmp(line, "JCANCEL", 7) == 0) {
            session.set_clock(now);
            const uint32_t before = dispatcher.sent();
            const JcAuthDecision d = dispatcher.submit(jc_jog_cancel(), context(false));
            printf("decision=");
            print_token(jc_auth_name(d));
            printf(" sent=%d\n", dispatcher.sent() != before ? 1 : 0);
        } else if (strncmp(line, "JTX", 3) == 0) {
            printf("%s\n", g_tx_len ? g_tx_hex : "-");
            g_tx_hex[0] = '\0';
            g_tx_len = 0;
        } else if (strncmp(line, "JACK", 4) == 0) {
            printf("%d\n", session.acknowledge() ? 1 : 0);
        } else if (strncmp(line, "JSTALE", 6) == 0) {
            session.on_link_lost(now, true);
            dispatcher.on_link_lost();
        } else if (strncmp(line, "JSUM", 4) == 0) {
            printf("phase=");
            print_token(jc_jog_phase_name(session.phase()));
            printf(" result=");
            print_token(jc_jog_result_name(session.result()));
            printf(" code=%d ack=%d poscheck=%d delta=%.3f unresolved=%d jogs=%u cancels=%u timeouts=%u losses=%u"
                   " refused=%u unsolicited=%u unexpected=%u foreign=%u unconfirmed=%d\n",
                   session.code(), session.needs_acknowledgement() ? 1 : 0, static_cast<int>(session.pos_check()),
                   static_cast<double>(session.pos_delta()), session.unresolved() ? 1 : 0, session.jogs_sent(),
                   session.cancels_sent(), session.timeouts(), session.losses(),
                   session.refused_policy() + session.refused_busy() + session.refused_class(), session.unsolicited(),
                   session.unexpected_motion(), session.foreign_motion(), session.cancel_unconfirmed() ? 1 : 0);
        } else if (strncmp(line, "CONNECT", 7) == 0) {
            tracker.request_connect(now);
            tracker.on_connected();
        } else if (strncmp(line, "CLOSE", 5) == 0) {
            assembler.reset();
            tracker.on_closed(now);
            loader.on_link_lost(now);
            session.on_link_lost(now, false);
            dispatcher.on_link_lost();
        } else if (strncmp(line, "RETRY", 5) == 0) {
            printf("%d\n", tracker.should_retry(now) ? 1 : 0);
        } else if (strncmp(line, "TX", 2) == 0) {
            int ok = 1;
            for (const char* p = line + 3; p[0] && p[1] && p[0] != '\n'; p += 2) {
                char hex[3] = { p[0], p[1], 0 };
                if (!jc_tx_byte_allowed(static_cast<uint8_t>(strtoul(hex, nullptr, 16)))) ok = 0;
            }
            printf("%d\n", ok);
        } else if (strncmp(line, "LOAD", 4) == 0) {
            const char* blocker = jc_caps_read_blocker(true, tracker.fresh(now), model.state(), loader.running());
            const bool started = loader.start(now, blocker);
            printf("%s%s\n", started ? "started" : "refused: ", started ? "" : blocker);
        } else if (strncmp(line, "NEXT", 4) == 0) {
            const char* q = loader.pending_line(now);
            printf("%s\n", q != nullptr ? q : "-");
        } else if (strncmp(line, "SENT", 4) == 0) {
            loader.mark_sent(now);
        } else if (strncmp(line, "TICK", 4) == 0) {
            loader.tick(now);
            session.tick(now);
        } else if (strncmp(line, "LOST", 4) == 0) {
            assembler.reset();
            tracker.on_closed(now);
            loader.on_link_lost(now);
            session.on_link_lost(now, false);
            dispatcher.on_link_lost();
        } else if (strncmp(line, "QOK ", 4) == 0) {
            char text[160];
            snprintf(text, sizeof(text), "%s", line + 4);
            text[strcspn(text, "\r\n")] = '\0';
            printf("%d\n", jc_query_allowed(text) ? 1 : 0);
        } else if (strncmp(line, "LSUM", 4) == 0) {
            static const char* const names[] = { "Idle", "Running", "Complete", "Failed" };
            const JcMachineCaps& c = loader.caps();
            char fail_token[64];
            snprintf(fail_token, sizeof(fail_token), "%s", jc_load_failure_name(loader.failure()));
            for (char* t = fail_token; *t; ++t) {
                if (*t == ' ') *t = '_';
            }
            printf("lstate=%s lfail=%s step=%u ok=%u err=%u mask=%u envmask=%u musthome=%d wcs=%s tool=%d lines=%u"
                   " timeouts=%u losses=%u",
                   names[static_cast<int>(loader.state())], fail_token,
                   static_cast<unsigned>(loader.step()), static_cast<unsigned>(c.queries_ok),
                   static_cast<unsigned>(c.queries_error), static_cast<unsigned>(c.confirmed_axis_mask()),
                   static_cast<unsigned>(c.envelope_axis_mask()),
                   c.must_home_state == JcFieldState::Valid ? (c.must_home ? 1 : 0) : -1,
                   c.wcs[0] ? c.wcs : "-", c.tool, lines_seen, loader.exec().timeouts(), loader.exec().losses());
            for (uint8_t a = 0; a < 4; ++a) {
                printf(" ax%u=", static_cast<unsigned>(a));
                for (uint8_t f = 0; f < kJcQueryFieldCount; ++f) {
                    if (f) printf(",");
                    if (c.axes[a].state[f] == JcFieldState::Valid) printf("%g", static_cast<double>(c.axes[a].value[f]));
                    else printf("%s", c.axes[a].state[f] == JcFieldState::CtrlError ? "E" : "-");
                }
            }
            printf(" probe=%s\n", c.probe_pin[0] ? c.probe_pin : "-");
        } else if (strncmp(line, "DATA", 4) == 0) {
            for (const char* p = line + 5; p[0] && p[1] && p[0] != '\n'; p += 2) {
                char hex[3] = { p[0], p[1], 0 };
                switch (assembler.push(static_cast<uint8_t>(strtoul(hex, nullptr, 16)))) {
                    case JcStreamSplitter::Event::None: break;
                    case JcStreamSplitter::Event::Line:
                        ++lines_seen;
                        loader.on_line(assembler.line(), now);
                        session.on_line(assembler.line(), now);
                        break;
                    case JcStreamSplitter::Event::LineOverflow: tracker.on_invalid(); break;
                    case JcStreamSplitter::Event::InvalidFrame: tracker.on_invalid(); break;
                    case JcStreamSplitter::Event::Frame: {
                        JcStatusFrame f {};
                        if (!jc_parse_status_frame(assembler.frame(), assembler.frame_length(), f)) {
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
                        {
                            JcJogStatus view;
                            view.fresh = tracker.fresh(now);
                            view.state = s.machine_state;
                            view.sequence = s.sequence;
                            view.pos_valid = true;
                            memcpy(view.pos, s.axes, sizeof(view.pos));
                            session.on_status(view, now);
                        }
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
