// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the BLE UART transport core and the transport manager (Instruction 010) with a MOCK radio and
// the SIMULATED FluidNC. No real BLE hardware or CC2541 module is involved - nothing here proves that a real
// module works. Built with -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_FULL_CONTROL_TEST=1.

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcBleUart.hpp"
#include "../../src/jc8048w550/JcCommandSystem.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcSimNc.hpp"
#include "../../src/jc8048w550/JcTransportManager.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- GATT tables -----------------------------------------------------------------------------------------------

static JcBleDiscovery hm10(uint8_t props = kBleNotify | kBleWriteNoRsp | kBleRead) {
    JcBleDiscovery d; memset(&d, 0, sizeof(d));
    d.n = 1;
    strcpy(d.services[0].uuid, "0000ffe0-0000-1000-8000-00805f9b34fb");
    d.services[0].nchars = 1;
    strcpy(d.services[0].chars[0].uuid, "0000ffe1-0000-1000-8000-00805f9b34fb");
    d.services[0].chars[0].props = props;
    return d;
}
static JcBleDiscovery nus(uint8_t rx_props = kBleWrite | kBleWriteNoRsp, uint8_t tx_props = kBleNotify) {
    JcBleDiscovery d; memset(&d, 0, sizeof(d));
    d.n = 1;
    strcpy(d.services[0].uuid, "6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    d.services[0].nchars = 2;
    strcpy(d.services[0].chars[0].uuid, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
    d.services[0].chars[0].props = rx_props;
    strcpy(d.services[0].chars[1].uuid, "6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
    d.services[0].chars[1].props = tx_props;
    return d;
}

static void test_profiles() {
    CHECK(jc_ble_uuid_eq("ffe0", "0000FFE0-0000-1000-8000-00805F9B34FB") && jc_ble_uuid_eq("FFE1", "ffe1") && !jc_ble_uuid_eq("ffe0", "ffe1"));
    CHECK(!jc_ble_uuid_eq(nullptr, "ffe0"));
    JcBleSelection s;
    CHECK(jc_ble_select(hm10(), s) && s.profile == JcBleProfile::Hm10 && !s.write_with_response && s.write_chr == s.notify_chr);
    CHECK(jc_ble_select(hm10(kBleNotify | kBleWrite), s) && s.write_with_response);                  // write with response only
    CHECK(!jc_ble_select(hm10(kBleWrite), s) && strstr(s.why, "notify") != nullptr);                // cannot notify
    CHECK(!jc_ble_select(hm10(kBleNotify | kBleRead), s) && strstr(s.why, "writable") != nullptr);   // cannot be written
    CHECK(jc_ble_select(nus(), s) && s.profile == JcBleProfile::Nus && s.write_chr != s.notify_chr && !s.write_with_response);
    CHECK(!jc_ble_select(nus(kBleRead, kBleNotify), s) && strstr(s.why, "not writable") != nullptr);
    CHECK(!jc_ble_select(nus(kBleWrite, kBleRead), s) && strstr(s.why, "cannot notify") != nullptr);
    JcBleDiscovery bat; memset(&bat, 0, sizeof(bat)); bat.n = 1; strcpy(bat.services[0].uuid, "180f");
    CHECK(!jc_ble_select(bat, s) && strstr(s.why, "no serial profile") != nullptr && s.profile == JcBleProfile::None);
    JcBleDiscovery none; memset(&none, 0, sizeof(none));
    CHECK(!jc_ble_select(none, s));
    JcBleDiscovery both = nus(); const JcBleDiscovery h = hm10();
    both.services[1] = h.services[0]; both.n = 2;
    CHECK(jc_ble_select(both, s) && s.profile == JcBleProfile::Nus);                                  // Nordic UART preferred
    JcBleDiscovery half = nus(); half.services[0].nchars = 1;                                          // RX only
    CHECK(!jc_ble_select(half, s));
    CHECK(strcmp(jc_ble_profile_name(JcBleProfile::Hm10), "HM-10 FFE0/FFE1") == 0);
}

static void test_scan_list_and_saved() {
    JcBleScanList l;
    CHECK(l.add("AA:BB:CC:DD:EE:01", "HM-10", -70, true, 1));
    CHECK(l.add("aa:bb:cc:dd:ee:01", "", -50, false, 2));          // same device (case-insensitive): refreshed
    CHECK(l.count() == 1 && l.sorted(0)->rssi == -50 && l.sorted(0)->serial_hint && strcmp(l.sorted(0)->addr, "AA:BB:CC:DD:EE:01") == 0);
    CHECK(!l.add("not-an-address", "x", -1, true, 3) && !l.add(nullptr, "x", -1, true, 3) && !l.add("AA:BB:CC:DD:EE", "x", -1, true, 3));
    l.add("AA:BB:CC:DD:EE:02", "Phone", -30, false, 4);
    l.add("AA:BB:CC:DD:EE:03", "Bridge\x01\xFF", -80, true, 5);
    CHECK(strcmp(l.sorted(0)->addr, "AA:BB:CC:DD:EE:01") == 0);    // serial-capable first, strongest first
    CHECK(strcmp(l.sorted(1)->addr, "AA:BB:CC:DD:EE:03") == 0 && strcmp(l.sorted(2)->addr, "AA:BB:CC:DD:EE:02") == 0);
    CHECK(strcmp(l.sorted(1)->name, "Bridge??") == 0);              // control / non-ASCII bytes never reach the display
    CHECK(l.sorted(3) == nullptr);
    for (int i = 0; i < 30; ++i) { char a[20]; snprintf(a, sizeof(a), "11:22:33:44:55:%02X", i); l.add(a, "n", -60, false, 6); }
    CHECK(l.count() == kJcBleMaxDevices);                           // bounded
    CHECK(l.find("aa:bb:cc:dd:ee:02") != nullptr && l.find("00:00:00:00:00:00") == nullptr);
    l.clear(); CHECK(l.count() == 0);

    JcBleSaved s; memset(&s, 0, sizeof(s));
    jc_ble_save(s, "AA:BB:CC:DD:EE:01", "HM-10");
    CHECK(s.valid && strcmp(s.addr, "AA:BB:CC:DD:EE:01") == 0 && strcmp(s.name, "HM-10") == 0);
    jc_ble_save(s, "garbage", "x");
    CHECK(!s.valid && s.addr[0] == '\0');
    jc_ble_save(s, "AA:BB:CC:DD:EE:01", "HM-10"); jc_ble_forget(s);
    CHECK(!s.valid && s.name[0] == '\0');
}

// ---- mock radio ---------------------------------------------------------------------------------------------------

struct Adv { std::string addr, name; int rssi; bool hint; };

struct MockRadio : JcBleRadio {
    JcBleUart* link = nullptr;
    uint32_t* clock = nullptr;
    std::vector<Adv> adverts;
    JcBleDiscovery disc = hm10();
    bool connect_ok = true, hang_connect = false, refuse_connect = false, subscribe_ok = true, discover_ok = true;
    bool write_ok = true;
    int  fail_writes = 0;
    bool connected = false, scanning = false;
    uint16_t mtu_ = 23;
    int connects = 0, disconnects = 0;
    std::string last_addr;
    std::vector<std::string> writes;       // one entry per GATT write
    std::string all_written() const { std::string s; for (const std::string& w : writes) s += w; return s; }
    // The text that was written, without the '?' status keep-alives that may be interleaved between pieces.
    std::string text_only() const { std::string s; for (char c : all_written()) if (c != '?') s += c; return s; }

    bool scan_start(uint32_t) override {
        scanning = true;
        for (const Adv& a : adverts) link->on_scan_result(a.addr.c_str(), a.name.c_str(), a.rssi, a.hint, *clock);
        return true;
    }
    void scan_stop() override { scanning = false; }
    bool connect(const char* addr) override {
        ++connects; last_addr = addr;
        if (refuse_connect) return false;
        if (hang_connect) return true;
        connected = connect_ok;
        link->on_connected(connect_ok, *clock);
        return true;
    }
    void disconnect() override { if (connected) ++disconnects; connected = false; }
    bool discover() override { if (!discover_ok) return false; link->on_discovery(disc, *clock); return true; }
    bool subscribe(const JcBleSelection&) override { link->on_subscribed(subscribe_ok, *clock); return true; }
    bool write(const JcBleSelection&, const uint8_t* d, size_t n) override {
        if (!connected) return false;
        if (fail_writes > 0) { --fail_writes; return false; }
        if (!write_ok) return false;
        writes.emplace_back(reinterpret_cast<const char*>(d), n);
        return true;
    }
    uint16_t mtu() const override { return mtu_; }
    // The peer drops the link (out of range, power loss).
    void drop() { connected = false; link->on_disconnected(*clock); }
};

struct Rig {
    uint32_t now = 1000;
    MockRadio radio;
    JcBleUart ble;
    std::vector<std::string> lines;
    Rig() { radio.link = &ble; radio.clock = &now; ble.set_radio(&radio); ble.set_line_listener(&Rig::on_line, this); }
    static void on_line(void* u, const char* l) { static_cast<Rig*>(u)->lines.push_back(l); }
    void run(uint32_t ms) { const uint32_t end = now + ms; while (now < end) { now += 5; ble.service(now); } }
    // scan, pick the first serial device, connect, finish setup
    void connect_saved_flow() {
        radio.adverts.push_back({ "AA:BB:CC:DD:EE:01", "HM-10", -60, true });
        JcBleSaved none; memset(&none, 0, sizeof(none));
        ble.begin(none, false, now);
        CHECK(ble.scan(now));
        CHECK(ble.state() == JcBleState::Scanning);
        CHECK(ble.connect_to("AA:BB:CC:DD:EE:01", "HM-10", now));
        run(200);
    }
    void notify(const std::string& s) { ble.on_notify(reinterpret_cast<const uint8_t*>(s.data()), s.size(), now); }
};

static void test_connect_flow_and_save() {
    Rig r;
    r.connect_saved_flow();
    CHECK(r.ble.state() == JcBleState::Ready && r.ble.ready());
    CHECK(r.ble.selection().profile == JcBleProfile::Hm10);
    CHECK(r.radio.last_addr == "AA:BB:CC:DD:EE:01" && r.radio.connects == 1);
    JcBleSaved s;
    CHECK(r.ble.take_save_request(s) && s.valid && strcmp(s.addr, "AA:BB:CC:DD:EE:01") == 0 && strcmp(s.name, "HM-10") == 0);
    CHECK(!r.ble.take_save_request(s));                                    // reported once
    CHECK(r.ble.devices().count() == 1 && strcmp(r.ble.devices().sorted(0)->name, "HM-10") == 0);
    CHECK(r.ble.diag().connects == 1 && r.ble.diag().scans == 1);
    // a '?' keep-alive is sent by itself, one byte on its own
    r.run(1500);
    CHECK(!r.radio.writes.empty() && r.radio.writes[0] == "?");

    // saved device: reconnects without any scan
    Rig q;
    JcBleSaved sv; jc_ble_save(sv, "AA:BB:CC:DD:EE:01", "HM-10");
    q.ble.begin(sv, true, q.now);
    CHECK(q.ble.state() == JcBleState::Connecting);
    q.run(300);
    CHECK(q.ble.ready() && q.radio.connects == 1 && q.ble.diag().scans == 0);
    CHECK(!q.ble.take_save_request(sv));                                   // nothing new to save
    // a saved device that is not wanted automatically stays idle
    Rig n;
    n.ble.begin(sv, false, n.now);
    n.run(500);
    CHECK(n.ble.state() == JcBleState::Idle && n.radio.connects == 0);
    n.ble.forget();
    JcBleSaved gone;
    CHECK(n.ble.take_save_request(gone) && !gone.valid);
}

static void test_unusable_peers() {
    {   Rig r;                                                              // peer without a serial profile
        r.radio.disc = hm10(kBleRead);
        r.connect_saved_flow();
        CHECK(r.ble.state() == JcBleState::Failed && strstr(r.ble.diag().last_error, "writable") != nullptr);
        r.run(60000);
        CHECK(r.ble.state() == JcBleState::Failed && r.radio.connects == 1);   // not retried: it would fail the same way
        CHECK(!r.ble.send_query_line("$G"));
    }
    {   Rig r; r.radio.subscribe_ok = false;
        r.connect_saved_flow();
        CHECK(r.ble.state() == JcBleState::Backoff && strstr(r.ble.diag().last_error, "notifications") != nullptr);
    }
    {   Rig r; r.radio.connect_ok = false;
        r.connect_saved_flow();
        CHECK(r.ble.state() == JcBleState::Backoff);
        r.radio.connect_ok = true;
        r.run(70000);
        CHECK(r.ble.ready() && r.ble.diag().reconnect_attempts >= 1);       // the saved/last device is retried with backoff
    }
    {   Rig r; r.radio.hang_connect = true;                                 // never answers
        r.connect_saved_flow();
        r.run(9000);
        CHECK(r.ble.state() == JcBleState::Backoff && strstr(r.ble.diag().last_error, "timed out") != nullptr);
    }
    {   Rig r; r.radio.refuse_connect = true;
        r.connect_saved_flow();
        CHECK(r.ble.state() == JcBleState::Backoff);
    }
    {   Rig r;                                                              // bad address is refused before the radio is touched
        JcBleSaved none; memset(&none, 0, sizeof(none));
        r.ble.begin(none, false, r.now);
        CHECK(!r.ble.connect_to("nonsense", "x", r.now) && r.radio.connects == 0);
    }
    {   Rig r;                                                              // no radio / not started
        JcBleUart u;
        CHECK(!u.scan(0) && !u.send_query_line("$G") && !u.send_command_bytes("!", 1));
    }
}

static const char* kFrame = "<Idle|WPos:1.000,2.000,3.000,4.000|FS:0,0|Pn:P>";

static void test_receive_fragmentation() {
    Rig r;
    r.connect_saved_flow();
    // a status frame in single-byte pieces
    const std::string f = std::string(kFrame) + "\n";
    for (char c : f) r.notify(std::string(1, c));
    CHECK(r.ble.status().has_data && strcmp(r.ble.status().machine_state, "Idle") == 0);
    CHECK(r.ble.status().axes[0] == 1.0f && r.ble.status().axes[3] == 4.0f && strcmp(r.ble.status().pins, "P") == 0);
    CHECK(r.ble.frames() == 1 && r.ble.diag().partial_notifications > 10);
    // lines, frames and a split in the middle of "error:9"
    r.notify("ok\r\nerr");
    r.notify("or:9\n[PRB:1.000,2.000,3.000:1]\n");
    CHECK(r.lines.size() == 3 && r.lines[0] == "ok" && r.lines[1] == "error:9" && r.lines[2] == "[PRB:1.000,2.000,3.000:1]");
    // two frames and a line in one notification
    r.notify(std::string(kFrame) + std::string(kFrame) + "ok\n");
    CHECK(r.ble.frames() == 3 && r.lines.size() == 4);
    // random fragmentation of a whole conversation always yields the same result
    const std::string conv = std::string(kFrame) + "\nok\n" + kFrame + "\n[MSG:hello]\nerror:20\n";
    for (int seed = 1; seed <= 20; ++seed) {
        Rig x; x.connect_saved_flow();
        size_t i = 0; uint32_t rnd = static_cast<uint32_t>(seed) * 2654435761u;
        while (i < conv.size()) {
            rnd = rnd * 1664525u + 1013904223u;
            const size_t n = 1 + (rnd >> 24) % 19;                           // 1..19 bytes: a BLE notification payload
            x.notify(conv.substr(i, n)); i += n;
        }
        CHECK(x.ble.frames() == 2 && x.lines.size() == 3 && x.lines[0] == "ok" && x.lines[2] == "error:20");
    }
    // a corrupt frame is counted and does not poison the next one
    r.notify("<Idle|WPos:x|FS:0,0>\n");
    r.notify(std::string(kFrame) + "\n");
    CHECK(r.ble.diag().invalid_frames >= 1 && r.ble.frames() == 4);
    // oversize notification: the excess is dropped and the stream re-synchronises on the next full line
    r.notify(std::string(700, 'x'));
    CHECK(r.ble.diag().rx_dropped == 700 - kJcBleRxChunk);
    r.notify("\nok\n");
    r.notify(std::string(kFrame) + "\n");
    CHECK(r.ble.frames() == 5);
    // data that arrives while not connected is ignored
    r.ble.disconnect(r.now);
    const uint32_t before = r.ble.diag().notifications;
    r.notify(std::string(kFrame) + "\n");
    CHECK(r.ble.diag().notifications == before);
}

static void test_transmit_queue_and_policy() {
    Rig r;
    r.connect_saved_flow();
    r.radio.writes.clear();
    // read-only queries only: anything else is refused and counted
    CHECK(r.ble.send_query_line("$G") && r.ble.send_query_line("$SD/List") && r.ble.send_query_line("$#"));
    CHECK(!r.ble.send_query_line("$X") && !r.ble.send_query_line("G0 X1") && !r.ble.send_query_line("$SD/Run=/a.nc") && !r.ble.send_query_line(nullptr));
    CHECK(r.ble.blocked() >= 4);
    // commands: only what the shared policy accepts
    CHECK(!r.ble.send_command_bytes("G1 X500 F1000\n", 14));
    CHECK(!r.ble.send_command_bytes("$H\n$X\n", 6));
    CHECK(!r.ble.send_command_bytes("$J=G91G21F100X1.000", 19));              // no terminating newline
    CHECK(!r.ble.send_command_bytes("M6 T3\n", 6) && !r.ble.send_command_bytes("$Localfs/Delete=/a\n", 19));
    CHECK(r.ble.send_command_bytes("G21 G90 G0 Z5.000\n", 18));
    r.run(400);
    // the text arrives in order, cut into ATT-sized (20 byte) pieces, and each query is a whole line
    CHECK(r.radio.text_only().find("$G\n$SD/List\n$#\nG21 G90 G0 Z5.000\n") != std::string::npos);
    for (const std::string& w : r.radio.writes) CHECK(w.size() <= 20);
    CHECK(r.ble.tx_queued() == 0);

    // a larger MTU allows bigger pieces (capped), still the same bytes
    Rig m; m.radio.mtu_ = 185; m.connect_saved_flow(); m.radio.writes.clear();
    CHECK(m.ble.chunk_size() == 100);
    m.ble.send_command_bytes("$J=G91G21F100X1.000\n", 20);
    m.run(100);
    CHECK(m.radio.text_only().find("$J=G91G21F100X1.000\n") != std::string::npos);
    bool whole = false; for (const std::string& w : m.radio.writes) whole = whole || w == "$J=G91G21F100X1.000\n";
    CHECK(whole);

    // real-time bytes jump ahead of queued text (never inserted INSIDE another write)
    Rig p; p.connect_saved_flow(); p.radio.writes.clear();
    for (int i = 0; i < 6; ++i) p.ble.send_query_line("$#");
    p.ble.send_command_bytes("!", 1);
    p.run(10);
    CHECK(!p.radio.writes.empty() && p.radio.writes[0] == "!");
    p.run(300);
    int ctrl = 0; for (const std::string& w : p.radio.writes) if (w.size() == 1 && w[0] == '!') ++ctrl;
    CHECK(ctrl == 1);

    // queue bound: a command that does not fit is refused WHOLE, never half-queued
    Rig q; q.connect_saved_flow(); q.radio.writes.clear();
    q.radio.write_ok = false;                                               // the radio stops draining
    int accepted = 0;
    for (int i = 0; i < 200; ++i) { if (q.ble.send_query_line("$/axes/x/steps_per_mm")) ++accepted; }
    CHECK(accepted > 0 && accepted < 200);
    CHECK(q.ble.tx_queued() <= kJcBleTxQueue && q.ble.diag().tx_refused > 0 && q.ble.diag().tx_queue_peak <= kJcBleTxQueue);
    CHECK(q.ble.tx_queued() % 22 == 0);                                     // whole lines only (21 characters + newline)

    // not connected: nothing is queued
    Rig n;
    JcBleSaved none; memset(&none, 0, sizeof(none)); n.ble.begin(none, false, n.now);
    CHECK(!n.ble.send_query_line("$G") && !n.ble.send_command_bytes("G90\n", 4) && n.ble.tx_queued() == 0);
}

static void test_loss_reconnect_no_replay() {
    Rig r;
    r.connect_saved_flow();
    r.notify(std::string(kFrame) + "\n");
    CHECK(r.ble.status_is_fresh(r.now));
    r.radio.writes.clear();
    // queue a command and make the radio refuse it, then lose the link
    r.radio.write_ok = false;
    CHECK(r.ble.send_command_bytes("G21 G90 G0 Z5.000\n", 18));
    CHECK(r.ble.tx_queued() == 18);
    r.radio.drop();
    CHECK(r.ble.state() == JcBleState::Backoff && r.ble.tx_queued() == 0);   // queued text is discarded with the link
    CHECK(r.ble.take_link_lost() && !r.ble.take_link_lost());
    CHECK(!r.ble.status_is_fresh(r.now) && !r.ble.status().has_data);       // stale status never survives
    CHECK(r.ble.diag().disconnects == 1);
    // reconnect by itself
    r.radio.write_ok = true;
    r.run(70000);
    CHECK(r.ble.ready() && r.ble.diag().reconnect_attempts >= 1 && r.radio.connects == 2);
    r.run(2000);
    // after the reconnection ONLY keep-alives were written: the old command was not replayed
    const std::string w = r.radio.all_written();
    CHECK(w.find("G21") == std::string::npos);
    for (char c : w) CHECK(c == '?');
    CHECK(!r.ble.take_link_lost());
    CHECK(!r.ble.status().has_data);                                         // until a NEW frame arrives
    r.notify(std::string(kFrame) + "\n");
    CHECK(r.ble.status().has_data && r.ble.status_is_fresh(r.now));

    // a half-received frame at the moment of loss does not leak into the new connection
    Rig x; x.connect_saved_flow();
    x.notify("<Idle|WPos:1.000,2.000");
    x.radio.drop(); x.run(70000);
    CHECK(x.ble.ready());
    x.notify(",3.000,4.000|FS:0,0>\n");
    CHECK(!x.ble.status().has_data);
    x.notify(std::string(kFrame) + "\n");
    CHECK(x.ble.status().has_data);

    // three failed GATT writes in a row are treated as a lost link
    Rig w3; w3.connect_saved_flow();
    w3.radio.fail_writes = 100;
    w3.ble.send_query_line("$G");
    w3.run(200);
    CHECK(w3.ble.state() == JcBleState::Backoff && w3.ble.diag().write_errors >= 3 && w3.ble.take_link_lost());

    // an operator disconnect never reconnects by itself
    Rig d; d.connect_saved_flow();
    d.ble.disconnect(d.now);
    CHECK(d.ble.state() == JcBleState::Idle && d.ble.take_link_lost());
    d.run(120000);
    CHECK(d.ble.state() == JcBleState::Idle && d.radio.connects == 1);
    // end() turns the transport off completely
    d.ble.end();
    CHECK(d.ble.state() == JcBleState::Off && !d.ble.send_query_line("$G"));
}

// ---- BLE under the unified command system, against the simulated FluidNC --------------------------------------

struct E2E {
    Rig rig;
    JcSimNc nc;
    JcCommandSystem sys;
    JcJogSession jogs;
    JcDispatcher disp;
    std::string pending_reply;
    int notif = 0;
    static bool tx(void* u, const char* b, size_t n) { return static_cast<E2E*>(u)->rig.ble.send_command_bytes(b, n); }
    E2E() : sys(&E2E::tx, this), jogs(&JcCommandSystem::session_tx, &sys) {
        sys.attach_jogs(&jogs);
        disp.set_sink_ex(&JcCommandSystem::sink, &sys);
        disp.arm();
        rig.connect_saved_flow();
        rig.ble.set_line_listener(&E2E::line, this);
    }
    static void line(void* u, const char* l) { static_cast<E2E*>(u)->sys.on_line(l); }
    JcAuthDecision submit(const JcCommand& c) {
        JcDispatchContext x; x.live = true; x.link_fresh = true; x.machine_state = "Idle"; x.operator_confirmed = true; x.now_ms = rig.now;
        x.operation_pending = sys.busy();
        return disp.submit(c, x);
    }
    void run(uint32_t ms) {
        const uint32_t end = rig.now + ms;
        while (rig.now < end) {
            rig.now += 10;
            nc.update(rig.now);
            rig.ble.service(rig.now);
            sys.set_clock(rig.now); sys.tick();
            // GATT writes reach the simulated controller; its replies come back in 7-byte notifications
            for (const std::string& w : rig.radio.writes) { if (w != "?") nc.send(w.data(), w.size()); }
            rig.radio.writes.clear();
            char reply[160];
            while (nc.pop_line(reply, sizeof(reply))) pending_reply += std::string(reply) + "\n";
            while (!pending_reply.empty()) {
                const size_t n = pending_reply.size() < 7 ? pending_reply.size() : 7;
                rig.ble.on_notify(reinterpret_cast<const uint8_t*>(pending_reply.data()), n, rig.now);
                pending_reply.erase(0, n); ++notif;
            }
            sys.on_machine_state(nc.state);
        }
    }
};

static void test_e2e_command_system() {
    E2E e;
    e.nc.m[2] = 0.0f;
    CHECK(e.submit(jc_goto_work_z(5.0f)) == JcAuthDecision::Allowed);
    e.run(600);
    JcCommandResult r;
    CHECK(e.sys.take_result(r) && r.outcome == JcExecOutcome::Acked);          // the acknowledgement crossed BLE in fragments
    CHECK(e.notif >= 1 && e.nc.cmds >= 1);
    // a rejection comes back the same way
    e.nc.error_on_cmd = e.nc.cmds + 1;
    CHECK(e.submit(jc_goto_work_z(6.0f)) == JcAuthDecision::Allowed);
    e.run(600);
    CHECK(e.sys.take_result(r) && r.outcome == JcExecOutcome::ControllerError && r.code == 9);
    // the single policy applies even if something tries to bypass the gate: raw text is refused by the link itself
    const size_t cmds = static_cast<size_t>(e.nc.cmds);
    CHECK(!e.rig.ble.send_command_bytes("G0 X100000\n", 11));
    e.run(300);
    CHECK(static_cast<size_t>(e.nc.cmds) == cmds);
    // link loss with a command in flight: LinkLost is reported once and nothing is sent after the reconnection
    e.nc.silent = true;
    CHECK(e.submit(jc_goto_work_z(7.0f)) == JcAuthDecision::Allowed);
    e.run(100);
    e.rig.radio.drop();
    e.sys.on_link_lost();
    e.sys.tick();
    CHECK(e.sys.take_result(r) && r.outcome == JcExecOutcome::LinkLost);
    const int before = e.nc.cmds;
    e.nc.silent = false;
    e.run(70000);
    CHECK(e.rig.ble.ready());
    CHECK(e.nc.cmds == before);                                                // no replay
    CHECK(!e.sys.take_result(r));
}

// ---- transport manager ------------------------------------------------------------------------------------------

struct TmLog {
    std::vector<std::string> events;
    static void stop(void* u, JcTransportKind k) { static_cast<TmLog*>(u)->events.push_back(std::string("stop:") + jc_transport_name(k)); }
    static void inval(void* u) { static_cast<TmLog*>(u)->events.push_back("invalidate"); }
};

static void test_transport_manager() {
    TmLog log;
    JcTransportManager tm;
    tm.set_callbacks(&TmLog::stop, &TmLog::inval, &log);
    CHECK(tm.active() == JcTransportKind::Wifi && tm.may_send(JcTransportKind::Wifi) && !tm.may_send(JcTransportKind::Ble) && !tm.may_send(JcTransportKind::Uart));
    CHECK(tm.select(JcTransportKind::Wifi, false) == nullptr && log.events.empty() && tm.generation() == 0);   // already active: no churn
    CHECK(tm.select(JcTransportKind::Ble, true) != nullptr && tm.active() == JcTransportKind::Wifi && log.events.empty());   // refused in motion
    CHECK(tm.select(JcTransportKind::Ble, false) == nullptr);
    CHECK(tm.active() == JcTransportKind::Ble && tm.generation() == 1);
    CHECK(log.events.size() == 3 && log.events[0] == "stop:Wi-Fi TCP" && log.events[1] == "stop:Wired UART" && log.events[2] == "invalidate");
    CHECK(tm.may_send(JcTransportKind::Ble) && !tm.may_send(JcTransportKind::Wifi) && !tm.may_send(JcTransportKind::Uart));
    CHECK(tm.select(JcTransportKind::Uart, false) == nullptr && tm.generation() == 2 && tm.active() == JcTransportKind::Uart);
    CHECK(log.events.back() == "invalidate" && log.events[log.events.size() - 3] == "stop:Wi-Fi TCP" && log.events[log.events.size() - 2] == "stop:BLE UART");

    // switching with a command in flight: it ends as LinkLost and is NOT replayed on the new transport
    JcSimNc nc;
    std::vector<std::string> wifi, ble;
    struct Ctx { JcTransportManager* tm; std::vector<std::string>* wifi; std::vector<std::string>* ble; JcSimNc* nc; } ctx { &tm, &wifi, &ble, &nc };
    tm.select(JcTransportKind::Wifi, false);
    auto tx = [](void* u, const char* b, size_t n) -> bool {
        Ctx* c = static_cast<Ctx*>(u);
        if (c->tm->may_send(JcTransportKind::Wifi)) { c->wifi->emplace_back(b, n); return true; }
        if (c->tm->may_send(JcTransportKind::Ble)) { c->ble->emplace_back(b, n); return true; }
        return false;
    };
    JcCommandSystem sys(tx, &ctx);
    JcJogSession jogs(&JcCommandSystem::session_tx, &sys);
    sys.attach_jogs(&jogs);
    JcDispatcher disp; disp.set_sink_ex(&JcCommandSystem::sink, &sys); disp.arm();
    JcDispatchContext x; x.live = true; x.link_fresh = true; x.machine_state = "Idle"; x.operator_confirmed = true; x.now_ms = 1000;
    sys.set_clock(1000);
    CHECK(disp.submit(jc_goto_work_z(5.0f), x) == JcAuthDecision::Allowed);
    CHECK(wifi.size() == 1 && sys.busy());
    struct Inval { JcCommandSystem* sys; } iv { &sys };
    tm.set_callbacks([](void*, JcTransportKind) {}, [](void* u) { static_cast<Inval*>(u)->sys->on_link_lost(); }, &iv);
    CHECK(tm.select(JcTransportKind::Ble, false) == nullptr);
    sys.tick();
    JcCommandResult res;
    CHECK(sys.take_result(res) && res.outcome == JcExecOutcome::LinkLost);
    CHECK(!sys.busy());
    for (uint32_t t = 1000; t < 40000; t += 10) { sys.set_clock(t); sys.tick(); }
    CHECK(wifi.size() == 1 && ble.empty());                                    // the old command went nowhere new
    // an explicit new command may use the new transport
    x.now_ms = 41000; sys.set_clock(41000);
    CHECK(disp.submit(jc_goto_work_z(6.0f), x) == JcAuthDecision::Allowed && ble.size() == 1 && wifi.size() == 1);
}

int main() {
    test_profiles();
    test_scan_list_and_saved();
    test_connect_flow_and_save();
    test_unusable_peers();
    test_receive_fragmentation();
    test_transmit_queue_and_policy();
    test_loss_reconnect_no_replay();
    test_e2e_command_system();
    test_transport_manager();
    printf("ble: %d checks, %d failed (MOCK radio; no hardware)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
