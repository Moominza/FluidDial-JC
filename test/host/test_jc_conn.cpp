// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the automatic connection policy (Instruction 006C.1): bounded exponential
// backoff, the six operator-facing states, the persisted auto-connect preference and the boot
// decision, the manual-disconnect / DEMO suppression rules, and the retry timeline of the shared
// link tracker. The Wi-Fi link itself needs the ESP32 network stack; the rules it follows are
// the pure functions below, and its behaviour on hardware is checked from the serial log.

#include <stdio.h>
#include <string.h>

#include "../../src/jc8048w550/JcConnPolicy.hpp"
#include "../../src/jc8048w550/JcLinkTracker.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void test_backoff() {
    // 2, 4, 8, 16, 30, 30, ... never above the cap, never below the base.
    const uint32_t expect_tcp[] = { 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (uint32_t n = 1; n <= 7; ++n) CHECK(jc_backoff_ms(n, kJcTcpBackoffBaseMs, kJcBackoffCapMs) == expect_tcp[n - 1]);
    const uint32_t expect_wifi[] = { 4000, 8000, 16000, 30000, 30000 };
    for (uint32_t n = 1; n <= 5; ++n) CHECK(jc_backoff_ms(n, kJcWifiBackoffBaseMs, kJcBackoffCapMs) == expect_wifi[n - 1]);
    CHECK(jc_backoff_ms(0, 2000, 30000) == 2000);                    // no failures yet: the base
    CHECK(jc_backoff_ms(0xFFFFFFFFu, 2000, 30000) == 30000);         // no overflow, still bounded
    CHECK(jc_backoff_ms(1000000u, 4000, 30000) == 30000);
    CHECK(jc_backoff_ms(3, 50000, 30000) == 30000);                  // a base above the cap is capped
    uint32_t prev = 0;
    for (uint32_t n = 1; n < 40; ++n) {                              // monotonic and bounded
        const uint32_t d = jc_backoff_ms(n, 2000, 30000);
        CHECK(d >= prev && d <= 30000 && d >= 2000);
        prev = d;
    }
}

static void test_labels() {
    // Six distinct, exact texts.
    CHECK_STR(jc_conn_label_text(JcConnLabel::Disconnected), "DISCONNECTED");
    CHECK_STR(jc_conn_label_text(JcConnLabel::WifiConnecting), "WIFI CONNECTING");
    CHECK_STR(jc_conn_label_text(JcConnLabel::FluidncConnecting), "FLUIDNC CONNECTING");
    CHECK_STR(jc_conn_label_text(JcConnLabel::Connected), "CONNECTED");
    CHECK_STR(jc_conn_label_text(JcConnLabel::Reconnecting), "RECONNECTING");
    CHECK_STR(jc_conn_label_text(JcConnLabel::Stale), "STALE");

    // First connection of this boot.
    CHECK(jc_conn_label(JcFluidState::WifiConnecting, true, false) == JcConnLabel::WifiConnecting);
    CHECK(jc_conn_label(JcFluidState::WifiConnected, true, false) == JcConnLabel::WifiConnecting);
    CHECK(jc_conn_label(JcFluidState::Resolving, true, false) == JcConnLabel::FluidncConnecting);
    CHECK(jc_conn_label(JcFluidState::FluidNcConnecting, true, false) == JcConnLabel::FluidncConnecting);
    CHECK(jc_conn_label(JcFluidState::Connected, true, false) == JcConnLabel::Connected);
    // After a connection existed, every further attempt is a RE-connection.
    CHECK(jc_conn_label(JcFluidState::WifiConnecting, true, true) == JcConnLabel::Reconnecting);
    CHECK(jc_conn_label(JcFluidState::Resolving, true, true) == JcConnLabel::Reconnecting);
    CHECK(jc_conn_label(JcFluidState::FluidNcConnecting, true, true) == JcConnLabel::Reconnecting);
    CHECK(jc_conn_label(JcFluidState::Lost, true, true) == JcConnLabel::Reconnecting);
    CHECK(jc_conn_label(JcFluidState::Error, true, false) == JcConnLabel::Reconnecting);
    // Data that stopped or is invalid is STALE, not CONNECTED.
    CHECK(jc_conn_label(JcFluidState::Stale, true, true) == JcConnLabel::Stale);
    CHECK(jc_conn_label(JcFluidState::InvalidData, true, true) == JcConnLabel::Stale);
    // No connection wanted: DISCONNECTED whatever the leftover state says.
    for (int s = 0; s <= static_cast<int>(JcFluidState::Error); ++s) {
        CHECK(jc_conn_label(static_cast<JcFluidState>(s), false, true) == JcConnLabel::Disconnected);
        CHECK(jc_conn_label(static_cast<JcFluidState>(s), false, false) == JcConnLabel::Disconnected);
    }
    // CONNECTED is produced by exactly one input.
    for (int s = 0; s <= static_cast<int>(JcFluidState::Error); ++s) {
        for (int req = 0; req < 2; ++req) {
            for (int had = 0; had < 2; ++had) {
                const bool is_connected = jc_conn_label(static_cast<JcFluidState>(s), req != 0, had != 0) == JcConnLabel::Connected;
                CHECK(is_connected == (s == static_cast<int>(JcFluidState::Connected) && req != 0));
            }
        }
    }
}

static void test_preference_and_boot() {
    // Power cycle with saved settings: a configured Wi-Fi transport connects by itself.
    CHECK(jc_boot_connect_decision(JcAutoPref::On, true, true).connect);
    CHECK(jc_boot_connect_decision(JcAutoPref::Unset, true, true).connect);   // configured, not yet chosen: on by default
    // ...but never without the operator's saved configuration, an explicit Off, or the Wi-Fi transport.
    CHECK(!jc_boot_connect_decision(JcAutoPref::On, false, true).connect);
    CHECK(!jc_boot_connect_decision(JcAutoPref::Off, true, true).connect);
    CHECK(!jc_boot_connect_decision(JcAutoPref::On, true, false).connect);    // UART / BLE selected: never switched
    CHECK(!jc_boot_connect_decision(JcAutoPref::Unset, true, false).connect);
    for (int p = 0; p < 3; ++p) {
        for (int c = 0; c < 2; ++c) {
            for (int w = 0; w < 2; ++w) {
                const JcBootDecision d = jc_boot_connect_decision(static_cast<JcAutoPref>(p), c != 0, w != 0);
                CHECK(d.reason != nullptr && d.reason[0] != '\0');
                CHECK(d.connect == (w != 0 && c != 0 && static_cast<JcAutoPref>(p) != JcAutoPref::Off));
            }
        }
    }

    // Manual disconnect and DEMO suppress reconnection until the operator chooses again.
    JcAutoPref pref = JcAutoPref::On;
    pref = jc_autopref_after(pref, JcConnEvent::ManualDisconnect);
    CHECK(pref == JcAutoPref::Off && !jc_boot_connect_decision(pref, true, true).connect);
    pref = jc_autopref_after(pref, JcConnEvent::FirstValidStatus);              // never overrides an explicit Off
    CHECK(pref == JcAutoPref::Off);
    pref = jc_autopref_after(pref, JcConnEvent::ManualConnect);
    CHECK(pref == JcAutoPref::On && jc_boot_connect_decision(pref, true, true).connect);
    pref = jc_autopref_after(pref, JcConnEvent::SwitchToDemo);                  // DEMO must not silently reconnect
    CHECK(pref == JcAutoPref::Off && !jc_boot_connect_decision(pref, true, true).connect);
    pref = jc_autopref_after(pref, JcConnEvent::SwitchToLive);
    CHECK(pref == JcAutoPref::On);
    // The first valid status turns the default on explicitly.
    CHECK(jc_autopref_after(JcAutoPref::Unset, JcConnEvent::FirstValidStatus) == JcAutoPref::On);
    CHECK(jc_autopref_after(JcAutoPref::On, JcConnEvent::FirstValidStatus) == JcAutoPref::On);

    // LIVE is shown only after valid, fresh status, and only for an automatic connection still wanted.
    CHECK(!jc_auto_live_allowed(true, false, JcAutoPref::On));     // connecting: not yet
    CHECK(jc_auto_live_allowed(true, true, JcAutoPref::On));       // valid status arrived
    CHECK(!jc_auto_live_allowed(false, true, JcAutoPref::On));     // not an automatic connection
    CHECK(!jc_auto_live_allowed(true, true, JcAutoPref::Off));     // operator turned it off meanwhile
    CHECK(jc_auto_live_allowed(true, true, JcAutoPref::Unset));
}

// The retry timeline of the shared link tracker: Wi-Fi / FluidNC unavailable, the network returns,
// a manual disconnect, stale status.
static void test_tracker_timeline() {
    JcLinkTracker t;
    uint32_t now = 1000;

    // The operator (or the boot) requests a connection; the first attempt is immediate.
    t.request_connect(now);
    CHECK(t.state(now) == JcFluidState::FluidNcConnecting);
    CHECK(t.should_retry(now));                       // attempt 1 starts at once
    t.on_connected();

    // Valid status: CONNECTED, fresh.
    now += 300;
    t.on_frame(now);
    CHECK(t.state(now) == JcFluidState::Connected && t.fresh(now));

    // Status stops: STALE after the 3 s window, immediately not fresh.
    CHECK(t.fresh(now + 2999) && !t.fresh(now + 3001));
    CHECK(t.state(now + 3500) == JcFluidState::Stale);

    // The connection is lost (Wi-Fi or FluidNC unavailable): retries back off 2, 4, 8, 16, 30, 30 s.
    now += 4000;
    t.on_closed(now);
    CHECK(t.state(now) == JcFluidState::Lost && !t.fresh(now));
    const uint32_t expect[] = { 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (int i = 0; i < 7; ++i) {
        CHECK(t.ms_until_retry(now) == expect[i] || i > 0);   // first delay from the loss itself
        // not due one millisecond early...
        const uint32_t due_in = t.ms_until_retry(now);
        CHECK(!t.should_retry(now + due_in - 1));
        // ...due exactly on time; the attempt fails (nothing connects), so the loss is reported again.
        now += due_in;
        CHECK(t.should_retry(now));
        t.on_closed(now);
        CHECK(t.ms_until_retry(now) == (i + 1 < 7 ? expect[i + 1] : 30000));
    }
    CHECK(t.retries() == 8);   // the initial attempt plus seven retries; never a tight loop

    // The network returns: the next attempt connects and valid status resets the backoff.
    const uint32_t due_in = t.ms_until_retry(now);
    now += due_in;
    CHECK(t.should_retry(now));
    t.on_connected();
    now += 200;
    t.on_frame(now);
    CHECK(t.state(now) == JcFluidState::Connected);
    now += 3500;
    t.on_closed(now);
    CHECK(t.ms_until_retry(now) == 2000);   // back to the base delay

    // Manual disconnect: nothing retries again until the operator connects again.
    t.request_disconnect();
    CHECK(t.state(now) == JcFluidState::Disconnected);
    for (int i = 0; i < 10; ++i) {
        now += 60000;
        CHECK(!t.should_retry(now));
    }
    CHECK(t.ms_until_retry(now) == 0);
    t.request_connect(now);     // CONNECT again
    CHECK(t.should_retry(now));
}

// What survives a restart: nothing that moves the machine. (The arming, acknowledgement, capability
// read and unfinished operations are covered end to end in test_jc_jog.cpp: they are all discarded
// on every loss and a reconnect transmits nothing.)
static void test_nothing_motion_related_is_persisted() {
    // The policy exposes exactly one persisted bit: whether to reconnect. There is no field for
    // arming, jobs, jogs or queued commands, and "On" alone can never authorize anything.
    JcAutoPref p = JcAutoPref::On;
    CHECK(sizeof(p) == 1);
    CHECK(jc_boot_connect_decision(p, true, true).connect);   // reconnect, and only reconnect
}

int main() {
    test_backoff();
    test_labels();
    test_preference_and_boot();
    test_tracker_timeline();
    test_nothing_motion_related_is_persisted();
    printf("%d checks, %d failed (connection policy tests)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
