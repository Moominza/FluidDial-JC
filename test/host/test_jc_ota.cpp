// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the Wi-Fi OTA policy: the update state machine (no manual arming: offered while the controller is Idle), the controller-state
// interlocks (a missing status is never Idle), authentication handling, the image-target check, the
// machine-command lockout at the ONE shared transmit point (so it holds on Wi-Fi, UART and BLE alike), and that
// the lockout does not weaken the existing command policy (no program start / resume / macros).
//
// What this does NOT test: the ArduinoOTA protocol itself, flash writes, or the bootloader rollback. Those are
// device behaviour (see docs/OTA_UPDATES.md for what was tested on hardware and what was not).
//
// Built with -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_FULL_CONTROL_TEST=1.

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCommandSystem.hpp"
#include "../../src/jc8048w550/JcOta.hpp"
#include "jc_forged.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static JcOtaInputs quiet() {
    JcOtaInputs in;
    in.wifi_up = true;
    in.password_set = true;
    in.password_applied = true;
    in.status_fresh = true;
    in.machine_state = "Idle";
    return in;
}

static void test_interlocks() {
    CHECK(jc_ota_blocker(quiet()) == nullptr);
    JcOtaInputs in = JcOtaInputs();                       // the default is the UNSAFE answer on every field
    CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.wifi_up = false;                     CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.password_set = false;                CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.password_applied = false;            CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.status_fresh = false;                CHECK(jc_ota_blocker(in) != nullptr);   // stale / lost link
    in = quiet(); in.machine_state = "";                  CHECK(jc_ota_blocker(in) != nullptr);   // missing status is NOT Idle
    in = quiet(); in.machine_state = nullptr;             CHECK(jc_ota_blocker(in) != nullptr);
    for (const char* s : { "Run", "Jog", "Hold:0", "Hold:1", "Home", "Alarm", "Door:0", "Check", "Sleep", "idle", "IDLE", "Idle " }) {
        in = quiet(); in.machine_state = s;
        CHECK(jc_ota_blocker(in) != nullptr);             // only the exact word Idle qualifies
    }
    in = quiet(); in.workflow_active = true;              CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.jog_unresolved = true;               CHECK(jc_ota_blocker(in) != nullptr);
    in = quiet(); in.command_pending = true;              CHECK(jc_ota_blocker(in) != nullptr);
}

// A controller that has seen a quiet pass is offering the service.
static JcOtaController ready_controller(uint32_t now = 100) {
    JcOtaController c;
    c.tick(quiet(), now);
    return c;
}

static void test_password_rules() {
    CHECK(!jc_ota_password_valid(nullptr));
    CHECK(!jc_ota_password_valid(""));
    CHECK(!jc_ota_password_valid("short"));
    CHECK(!jc_ota_password_valid("1234567"));             // 7
    CHECK(jc_ota_password_valid("12345678"));             // 8
    CHECK(jc_ota_password_valid("abcdefghijklmnopqrstuvwxyz012345"));   // 32
    CHECK(!jc_ota_password_valid("abcdefghijklmnopqrstuvwxyz0123456")); // 33
    CHECK(!jc_ota_password_valid("has space 123"));
    CHECK(!jc_ota_password_valid("tab\there123"));
    CHECK(!jc_ota_password_valid("caf\xC3\xA9-password"));
    // No built-in password exists: a pendant that was never given one never offers the service.
    JcOtaController c;
    JcOtaInputs in = quiet(); in.password_set = false;
    c.tick(in, 100);
    CHECK(c.state() == JcOtaState::Off && !c.locked() && !c.service_wanted());
    in.password_set = true; in.password_applied = false;   // a password changed since the library started
    c.tick(in, 200);
    CHECK(c.state() == JcOtaState::Off && !c.service_wanted());
}

static void test_offered_only_when_quiet_and_never_locks_by_itself() {
    JcOtaController c;
    CHECK(c.state() == JcOtaState::Off && !c.locked() && !c.service_wanted());
    c.tick(quiet(), 1000);
    CHECK(c.state() == JcOtaState::Ready && c.service_wanted());
    CHECK(!c.locked());                                       // READY does not touch the operator's commands
    for (uint32_t t = 2000; t < 2000 + 3600u * 1000u; t += 60000u) c.tick(quiet(), t);
    CHECK(c.state() == JcOtaState::Ready);                    // no timeout: it stays offered while the controller is quiet
    // Every unsafe condition keeps it unavailable.
    for (int why = 0; why < 8; ++why) {
        JcOtaController d;
        JcOtaInputs in = quiet();
        switch (why) {
            case 0: in.machine_state = "Run"; break;
            case 1: in.machine_state = ""; break;
            case 2: in.status_fresh = false; break;
            case 3: in.workflow_active = true; break;
            case 4: in.command_pending = true; break;
            case 5: in.wifi_up = false; break;
            case 6: in.jog_unresolved = true; break;
            default: in.machine_state = "Alarm"; break;
        }
        d.tick(in, 100);
        CHECK(d.state() == JcOtaState::Off && !d.service_wanted() && !d.locked());
    }
}

static void test_withdrawn_when_controller_not_quiet() {
    for (int why = 0; why < 6; ++why) {
        JcOtaController c = ready_controller();
        CHECK(c.state() == JcOtaState::Ready);
        JcOtaInputs in = quiet();
        switch (why) {
            case 0: in.machine_state = "Run"; break;
            case 1: in.machine_state = ""; break;
            case 2: in.status_fresh = false; break;
            case 3: in.workflow_active = true; break;
            case 4: in.command_pending = true; break;
            default: in.wifi_up = false; break;
        }
        c.tick(in, 200);
        CHECK(c.state() == JcOtaState::Off && !c.locked() && !c.service_wanted());   // closed at once
        c.tick(quiet(), 300);
        CHECK(c.state() == JcOtaState::Ready);                                       // and back when quiet again
    }
    // A password changed while the service is offered does not withdraw it.
    JcOtaController c = ready_controller();
    JcOtaInputs in = quiet(); in.password_applied = false;
    c.tick(in, 200);
    CHECK(c.state() == JcOtaState::Ready);
}

static void test_transfer_lifecycle() {
    JcOtaController c;
    // Events while the service is not being offered are ignored: an unsolicited transfer cannot move the state,
    // and the glue is told to refuse it.
    CHECK(!c.on_start());
    c.on_progress(5, 10); c.on_transfer_complete(); c.on_validated(true, 0);
    CHECK(c.state() == JcOtaState::Off && !c.locked());
    c.tick(quiet(), 100);
    CHECK(c.on_start());
    CHECK(c.state() == JcOtaState::Receiving && c.locked() && c.percent() == 0);
    c.tick(quiet(), 101);
    CHECK(c.state() == JcOtaState::Receiving);                // ticks never abandon a flash write half way
    JcOtaInputs run = quiet(); run.machine_state = "Run";
    c.tick(run, 102);
    CHECK(c.state() == JcOtaState::Receiving && c.locked());  // even if the controller reports something else
    c.on_progress(250, 1000);
    CHECK(c.percent() == 25);
    c.on_progress(1000, 1000);
    CHECK(c.percent() == 100);
    c.on_transfer_complete();
    CHECK(c.state() == JcOtaState::Validating && c.locked());
    c.on_validated(true, 5000);
    CHECK(c.state() == JcOtaState::PendingRestart && c.locked());
    CHECK(!c.restart_due(5000 + kJcOtaRestartDelayMs - 1));
    CHECK(c.restart_due(5000 + kJcOtaRestartDelayMs));
    c.tick(quiet(), 6000);
    CHECK(c.state() == JcOtaState::PendingRestart);            // nothing re-offers the service while a restart is pending
    // Only a PendingRestart asks for a restart.
    JcOtaController d;
    CHECK(!d.restart_due(0xFFFFFFFFu));
    // The start while the controller is not quiet is refused (the service had been withdrawn).
    JcOtaController e = ready_controller();
    e.tick(run, 150);
    CHECK(!e.on_start() && !e.locked());
}

static void test_wrong_target_image_rejected() {
    JcOtaController c = ready_controller();
    CHECK(c.on_start());
    c.on_progress(10, 10);
    c.on_transfer_complete();
    c.on_validated(false, 200);                            // image without the JC marker
    CHECK(c.state() == JcOtaState::Failed && !c.locked() && !c.restart_due(100000));
    CHECK(strstr(c.message(), "REJECTED") != nullptr && strstr(c.message(), "kept") != nullptr);
    c.tick(quiet(), 300);
    CHECK(c.state() == JcOtaState::Failed && !c.service_wanted());   // a failure stays on screen...
    c.tick(quiet(), 300 + kJcOtaFailureShowMs - 1);
    CHECK(c.state() == JcOtaState::Failed);
    c.tick(quiet(), 300 + kJcOtaFailureShowMs);                      // ...then clears by itself
    CHECK(c.state() == JcOtaState::Ready);
    CHECK(c.on_start());
    c.on_transfer_complete(); c.on_validated(false, 400);
    CHECK(c.state() == JcOtaState::Failed);
    c.acknowledge_failure();                                          // or the operator dismisses it
    CHECK(c.state() == JcOtaState::Off);
    c.tick(quiet(), 400);
    CHECK(c.state() == JcOtaState::Ready);
}

static void test_errors() {
    struct { JcOtaError e; } cases[] = { { JcOtaError::Begin }, { JcOtaError::Connect }, { JcOtaError::Receive }, { JcOtaError::End } };
    for (auto& k : cases) {
        JcOtaController c = ready_controller();
        CHECK(c.on_start());
        c.on_progress(3, 10);
        c.on_error(k.e);
        CHECK(c.state() == JcOtaState::Failed && !c.locked() && !c.restart_due(1u << 30));
        CHECK(strstr(c.message(), "kept") != nullptr);     // the operator is told the old firmware is untouched
    }
    // A refused image (too large) is reported before any transfer began.
    JcOtaController b = ready_controller();
    b.on_error(JcOtaError::Begin);
    CHECK(b.state() == JcOtaState::Failed && !b.locked());
    // An interrupted transfer cannot be restarted by replaying events.
    JcOtaController c = ready_controller();
    CHECK(c.on_start()); c.on_error(JcOtaError::Receive);
    c.on_progress(9, 10); c.on_transfer_complete(); c.on_validated(true, 1);
    CHECK(c.state() == JcOtaState::Failed);
}

static void test_authentication() {
    JcOtaController c = ready_controller(100);
    for (int i = 1; i < kJcOtaMaxAuthFailures; ++i) {
        c.on_error(JcOtaError::Auth);
        CHECK(c.state() == JcOtaState::Ready && c.auth_failures() == i);   // refused, still offered, commands untouched
        CHECK(!c.locked());
    }
    c.on_error(JcOtaError::Auth);
    CHECK(c.state() == JcOtaState::Off && !c.locked() && c.paused());      // brute force: the service pauses
    CHECK(strstr(c.message(), "wrong passwords") != nullptr);
    // Paused: not re-offered even though the controller is quiet.
    c.tick(quiet(), 1000);
    c.tick(quiet(), 1000 + kJcOtaCooldownMs - 1);
    CHECK(c.state() == JcOtaState::Off && !c.service_wanted());
    // The cool-down ends by itself and the count starts again.
    c.tick(quiet(), 1000 + kJcOtaCooldownMs);
    CHECK(!c.paused() && c.auth_failures() == 0);
    c.tick(quiet(), 1001 + kJcOtaCooldownMs);
    CHECK(c.state() == JcOtaState::Ready);
    // A correct login resets the count (a transfer starts).
    c.on_error(JcOtaError::Auth); c.on_error(JcOtaError::Auth);
    CHECK(c.on_start() && c.auth_failures() == 0);
    // The cool-down survives a tick-counter wrap.
    JcOtaController w = ready_controller(0xFFFFFF00u);
    for (int i = 0; i < kJcOtaMaxAuthFailures; ++i) w.on_error(JcOtaError::Auth);
    w.tick(quiet(), 0xFFFFFF10u);
    w.tick(quiet(), 0x00000100u);
    CHECK(w.paused());
}

static void test_marker_scanner() {
    const std::string marker = JC_OTA_IMAGE_MARKER;
    std::string image(5000, '\xAA');
    image.replace(3000, marker.size(), marker);
    for (size_t chunk : { size_t(1), size_t(7), size_t(64), size_t(512), size_t(4096) }) {
        JcOtaMarkerScanner s;
        for (size_t off = 0; off < image.size(); off += chunk) {
            s.feed(reinterpret_cast<const uint8_t*>(image.data()) + off, std::min(chunk, image.size() - off));
        }
        CHECK(s.found());                                  // found however the stream is chunked, including across boundaries
    }
    JcOtaMarkerScanner none;
    std::string other(8000, '\x11');
    none.feed(reinterpret_cast<const uint8_t*>(other.data()), other.size());
    CHECK(!none.found());
    // A near miss (one byte different) is not a match; neither is a truncated marker.
    std::string near = marker; near[10] ^= 1;
    JcOtaMarkerScanner s2;
    s2.feed(reinterpret_cast<const uint8_t*>(near.data()), near.size());
    CHECK(!s2.found());
    JcOtaMarkerScanner s3;
    s3.feed(reinterpret_cast<const uint8_t*>(marker.data()), marker.size() - 1);
    CHECK(!s3.found());
    // The marker names this hardware, so an M5Dial / CYD image (which does not contain it) cannot pass.
    CHECK(marker.find("JC8048W550C_I") != std::string::npos && marker.find("ESP32-S3") != std::string::npos);
}

// ---- lockout at the one transmit point ----------------------------------------------------------------------

struct Wire {
    std::vector<std::string> sent;
    static bool send(void* self, const char* bytes, size_t n) {
        static_cast<Wire*>(self)->sent.emplace_back(bytes, n);
        return true;
    }
};

static void test_lockout_at_shared_transmit_point() {
    Wire w;
    JcCommandSystem sys(&Wire::send, &w);
    JcJogSession jogs(&JcCommandSystem::session_tx, &sys);
    sys.attach_jogs(&jogs);
    // Unlocked: the ordinary lines pass the shared policy and leave.
    CHECK(JcCommandSystem::session_tx(&sys, "$J=G91G21F100X1.000\n", 20));
    CHECK(w.sent.size() == 1);
    sys.set_locked(true);
    // Locked: not a jog, not a home, not a hold, not a reset, not an unlock - on the common path every transport uses.
    const char* lines[] = { "$J=G91G21F100X1.000\n", "$HX\n", "G10 L20 P1 X0.000\n", "$X\n", "G21 G90 G0 Z0.000\n", "M5\n" };
    for (const char* l : lines) CHECK(!JcCommandSystem::session_tx(&sys, l, strlen(l)));
    const char bytes[] = { '!', 0x18, static_cast<char>(0x85) };
    for (char b : bytes) CHECK(!JcCommandSystem::session_tx(&sys, &b, 1));
    CHECK(JcCommandSystem::sink(&sys, JcCommandClass::Hold, "!") == false);
    CHECK(JcCommandSystem::sink(&sys, JcCommandClass::Home, "$HX") == false);
    CHECK(w.sent.size() == 1);                              // nothing more reached the transport
    CHECK(sys.refused_locked() >= 11);
    sys.set_locked(false);
    CHECK(JcCommandSystem::session_tx(&sys, "$J=G91G21F100Y1.000\n", 20));   // released: normal again
    CHECK(w.sent.size() == 2);
}

static void test_lockout_follows_ota_state() {
    // The application sets the lock from JcOtaController::locked() every pass. Walk the whole life cycle.
    Wire w;
    JcCommandSystem sys(&Wire::send, &w);
    JcOtaController c;
    auto pass = [&](const JcOtaInputs& in, uint32_t now) { c.tick(in, now); sys.set_locked(c.locked()); };
    pass(quiet(), 10);
    CHECK(c.state() == JcOtaState::Ready);
    CHECK(!sys.locked());                                   // offering the service does NOT lock the operator out
    CHECK(JcCommandSystem::session_tx(&sys, "$J=G91G21F100X1.000\n", 20));   // jogging still works while READY
    CHECK(w.sent.size() == 1);
    CHECK(c.on_start()); pass(quiet(), 22);
    CHECK(sys.locked());                                    // the transfer locks everything
    CHECK(!JcCommandSystem::session_tx(&sys, "$X\n", 3));
    c.on_transfer_complete(); pass(quiet(), 23); CHECK(sys.locked());
    c.on_validated(false, 24); pass(quiet(), 25);
    CHECK(!sys.locked());                                   // a failed update gives the pendant back
    c.acknowledge_failure(); pass(quiet(), 26);
    CHECK(c.state() == JcOtaState::Ready && !sys.locked());
    JcOtaInputs run = quiet(); run.machine_state = "Run";
    pass(run, 32);
    CHECK(c.state() == JcOtaState::Off && !sys.locked());   // controller started something: service closed, nothing locked
}

// ---- the lockout never opens a program-start path ----------------------------------------------------------

static void test_command_policy_unchanged() {
    for (const char* t : { "$SD/Run=/job.gcode", "$Localfs/Run=/x.nc", "$Macro/Run0", "~", "$SD/Resume" }) {
        CHECK(jc_is_program_start_text(t));
        CHECK(!jc_command_line_allowed(JcCommandClass::Other, t));
        CHECK(!jc_command_line_allowed(JcCommandClass::Job, t));
        CHECK(!jc_command_line_allowed(JcCommandClass::Macro, t));
    }
    CHECK(!jc_command_bytes_allowed("~", 1));
    CHECK(!jc_command_bytes_allowed("$SD/Run=/job.gcode\n", 19));
    // The OTA control surface adds no command class and no text the policy accepts.
    CHECK(!jc_command_line_allowed(JcCommandClass::Other, "$OTA"));
    CHECK(!jc_command_line_allowed(JcCommandClass::Other, "$Bye"));
    CHECK(!jc_command_line_allowed(JcCommandClass::Other, "$Firmware/Update"));
}

int main() {
    test_interlocks();
    test_password_rules();
    test_offered_only_when_quiet_and_never_locks_by_itself();
    test_withdrawn_when_controller_not_quiet();
    test_transfer_lifecycle();
    test_wrong_target_image_rejected();
    test_errors();
    test_authentication();
    test_marker_scanner();
    test_lockout_at_shared_transmit_point();
    test_lockout_follows_ota_state();
    test_command_policy_unchanged();
    printf("%d checks, %d failed (OTA policy)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
