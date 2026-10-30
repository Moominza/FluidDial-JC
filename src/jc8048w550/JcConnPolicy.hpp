// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Automatic connection policy (Instruction 006C.1). Pure logic shared by the firmware and the
// host tests; it decides WHAT should happen, the Wi-Fi link (JcReadOnlyFluidNc) does it.
//
//   * what the operator last chose (Connect / Disconnect / DEMO / LIVE) is the persisted
//     "auto-connect" preference, and is the ONLY thing that can start a connection at boot;
//   * a manual Disconnect or a switch to DEMO turns it off and it stays off until the operator
//     chooses Connect or LIVE again (nothing silently reconnects);
//   * reconnection after a loss uses bounded exponential backoff, never a tight loop;
//   * the transport is never changed automatically: only the Wi-Fi transport is auto-started,
//     and only when it is the transport the operator selected;
//   * nothing that moves the machine is ever restored: no arming, no unfinished job, no jog,
//     no command queue (JcApp / JcJogSession discard all of it on every loss).

#pragma once

#include <stdint.h>

#include "JcLinkTypes.hpp"

namespace jc8048w550 {

// ---- bounded exponential backoff ------------------------------------------------------------

constexpr uint32_t kJcTcpBackoffBaseMs = 2000;    // FluidNC endpoint unreachable
constexpr uint32_t kJcWifiBackoffBaseMs = 4000;   // Wi-Fi association lost
constexpr uint32_t kJcBackoffCapMs = 30000;       // never wait longer than this between attempts

// Delay after the n-th consecutive failure (n >= 1): base, 2*base, 4*base, ... capped.
inline uint32_t jc_backoff_ms(uint32_t failures, uint32_t base_ms, uint32_t cap_ms) {
    uint32_t delay = base_ms;
    for (uint32_t i = 1; i < failures && delay < cap_ms; ++i) {
        delay *= 2;
    }
    return delay < cap_ms ? delay : cap_ms;
}

// ---- the six states the operator sees --------------------------------------------------------------

enum class JcConnLabel : uint8_t { Disconnected, WifiConnecting, FluidncConnecting, Connected, Reconnecting, Stale };

inline const char* jc_conn_label_text(JcConnLabel l) {
    switch (l) {
        case JcConnLabel::Disconnected: return "DISCONNECTED";
        case JcConnLabel::WifiConnecting: return "WIFI CONNECTING";
        case JcConnLabel::FluidncConnecting: return "FLUIDNC CONNECTING";
        case JcConnLabel::Connected: return "CONNECTED";
        case JcConnLabel::Reconnecting: return "RECONNECTING";
        case JcConnLabel::Stale: return "STALE";
    }
    return "?";
}

// `connect_requested`: a connection is wanted (manual or automatic); `had_connection`: this boot
// has already received valid status at least once, so any further attempt is a RE-connection.
// CONNECTED means a valid, fresh status frame; anything weaker never says CONNECTED.
inline JcConnLabel jc_conn_label(JcFluidState s, bool connect_requested, bool had_connection) {
    if (!connect_requested || s == JcFluidState::Disconnected) {
        return JcConnLabel::Disconnected;
    }
    switch (s) {
        case JcFluidState::Connected: return JcConnLabel::Connected;
        case JcFluidState::Stale:
        case JcFluidState::InvalidData: return JcConnLabel::Stale;
        case JcFluidState::Lost:
        case JcFluidState::Error: return JcConnLabel::Reconnecting;
        case JcFluidState::WifiConnecting:
        case JcFluidState::WifiConnected:
            return had_connection ? JcConnLabel::Reconnecting : JcConnLabel::WifiConnecting;
        case JcFluidState::Resolving:
        case JcFluidState::FluidNcConnecting:
            return had_connection ? JcConnLabel::Reconnecting : JcConnLabel::FluidncConnecting;
        case JcFluidState::Disconnected: break;
    }
    return JcConnLabel::Disconnected;
}

// ---- persisted auto-connect preference ----------------------------------------------------------------

// Unset: the operator has not chosen yet. It behaves as On once a valid Wi-Fi / FluidNC
// configuration has been saved (the operator configured a controller), and becomes On
// explicitly after the first valid status.
enum class JcAutoPref : uint8_t { Unset, Off, On };

enum class JcConnEvent : uint8_t {
    ManualConnect,      // CONNECT on the Wi-Fi page, or LIVE requested while not connected
    ManualDisconnect,   // DISCONNECT
    SwitchToDemo,       // DEMO selected
    SwitchToLive,       // LIVE selected
    FirstValidStatus,   // the first valid, fresh status of a connection
};

inline JcAutoPref jc_autopref_after(JcAutoPref current, JcConnEvent e) {
    switch (e) {
        case JcConnEvent::ManualConnect:
        case JcConnEvent::SwitchToLive:
            return JcAutoPref::On;
        case JcConnEvent::ManualDisconnect:
        case JcConnEvent::SwitchToDemo:
            return JcAutoPref::Off;
        case JcConnEvent::FirstValidStatus:
            return current == JcAutoPref::Unset ? JcAutoPref::On : current;   // never overrides an explicit Off
    }
    return current;
}

struct JcBootDecision {
    bool        connect;
    const char* reason;
};

// What to do at power-up, after the display and touch are running. Only the saved configuration of
// the selected Wi-Fi transport is ever started; a wired UART or BLE selection is never auto-started.
inline JcBootDecision jc_boot_connect_decision(JcAutoPref pref, bool config_valid, bool wifi_transport_selected) {
    if (!wifi_transport_selected) return { false, "Wi-Fi is not the selected transport; transports are never switched automatically" };
    if (!config_valid) return { false, "no saved Wi-Fi / FluidNC configuration" };
    if (pref == JcAutoPref::Off) return { false, "auto-connect is off (manual disconnect or DEMO was chosen)" };
    return { true, pref == JcAutoPref::On ? "auto-connect is on" : "saved configuration, auto-connect not yet chosen" };
}

// Entering LIVE on its own is allowed only for an automatic connection that the operator has not
// turned off, and only once valid, fresh status has arrived.
inline bool jc_auto_live_allowed(bool live_pending, bool link_fresh, JcAutoPref pref) {
    return live_pending && link_fresh && pref != JcAutoPref::Off;
}

}  // namespace jc8048w550
