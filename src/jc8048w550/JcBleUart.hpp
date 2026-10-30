// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// BLE UART transport core (Instruction 010). The JC is the BLE CENTRAL; the peer is a transparent serial bridge
// wired to the FluidNC UART: an HM-10 / CC2541 style module (service FFE0, one characteristic FFE1) or a device
// speaking the Nordic UART Service. Everything radio specific sits behind JcBleRadio; this header is pure so the
// whole link (scan list, GATT validation, fragmentation, queues, reconnection) is tested with a mock radio.
//
// BLE carries the same byte stream as Wi-Fi and UART. It has NO command authority of its own:
//   * outgoing text passes jc_query_allowed() (read-only queries) or jc_command_bytes_allowed() (the ONE shared
//     command policy, reached only through the gate's sink) before it is queued - exactly like the other links;
//   * incoming bytes go through the shared JcStreamSplitter / jc_parse_status_frame, nothing BLE specific;
//   * a lost connection clears the queues and the parser; nothing queued is ever sent after a reconnect.
//
// NOT verified on hardware: the external CC2541 module has not been physically tested with this firmware.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcCommandPolicy.hpp"
#include "JcLinkTracker.hpp"
#include "JcLinkTypes.hpp"
#include "JcQuery.hpp"
#include "JcStatusApply.hpp"
#include "JcStreamSplitter.hpp"

namespace jc8048w550 {

// ---- GATT description and profile validation ---------------------------------------------------------------

constexpr const char* kJcNusService = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* kJcNusWrite = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // central -> peripheral
constexpr const char* kJcNusNotify = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // peripheral -> central
constexpr const char* kJcHm10Service = "ffe0";
constexpr const char* kJcHm10Char = "ffe1";

constexpr uint8_t kBleRead = 0x01, kBleWriteNoRsp = 0x02, kBleWrite = 0x04, kBleNotify = 0x08, kBleIndicate = 0x10;

// Case-insensitive UUID comparison; a 16-bit form ("ffe0") equals its 128-bit Bluetooth-base form.
inline bool jc_ble_uuid_eq(const char* a, const char* b) {
    auto norm = [](const char* in, char* out) {
        size_t n = 0;
        for (; *in && n < 36; ++in) out[n++] = (*in >= 'A' && *in <= 'Z') ? static_cast<char>(*in + 32) : *in;
        out[n] = '\0';
        if (n == 4) {                                           // expand 16-bit
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "0000%s-0000-1000-8000-00805f9b34fb", out);
            strcpy(out, tmp);
        } else if (n == 8) {                                    // expand 32-bit
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "%s-0000-1000-8000-00805f9b34fb", out);
            strcpy(out, tmp);
        }
    };
    char x[48], y[48];
    if (a == nullptr || b == nullptr) return false;
    norm(a, x);
    norm(b, y);
    return strcmp(x, y) == 0;
}

struct JcBleChar {
    char    uuid[40];
    uint8_t props;
};
struct JcBleService {
    char      uuid[40];
    JcBleChar chars[6];
    uint8_t   nchars;
};
struct JcBleDiscovery {
    JcBleService services[6];
    uint8_t      n;
};

enum class JcBleProfile : uint8_t { None, Nus, Hm10 };

inline const char* jc_ble_profile_name(JcBleProfile p) {
    switch (p) {
        case JcBleProfile::Nus: return "Nordic UART";
        case JcBleProfile::Hm10: return "HM-10 FFE0/FFE1";
        default: return "none";
    }
}

struct JcBleSelection {
    JcBleProfile profile;
    int8_t       svc;        // index in the discovery
    int8_t       write_chr;  // characteristic the central writes
    int8_t       notify_chr; // characteristic the peripheral notifies on (same as write_chr for HM-10)
    bool         write_with_response;
    char         why[96];    // failure reason / validation note
};

// Validates the GATT table and picks the serial characteristics. A service that is present but cannot carry
// serial data (a missing property) is a clear failure, never silently used.
inline bool jc_ble_select(const JcBleDiscovery& d, JcBleSelection& out) {
    out = JcBleSelection();
    out.profile = JcBleProfile::None;
    out.svc = out.write_chr = out.notify_chr = -1;
    bool found_any = false;
    for (uint8_t s = 0; s < d.n; ++s) {
        const JcBleService& svc = d.services[s];
        if (jc_ble_uuid_eq(svc.uuid, kJcNusService)) {
            found_any = true;
            int8_t w = -1, n = -1;
            for (uint8_t c = 0; c < svc.nchars; ++c) {
                if (jc_ble_uuid_eq(svc.chars[c].uuid, kJcNusWrite)) w = static_cast<int8_t>(c);
                if (jc_ble_uuid_eq(svc.chars[c].uuid, kJcNusNotify)) n = static_cast<int8_t>(c);
            }
            if (w < 0 || n < 0) { snprintf(out.why, sizeof(out.why), "Nordic UART service lacks its RX/TX characteristics"); continue; }
            if ((svc.chars[w].props & (kBleWrite | kBleWriteNoRsp)) == 0) { snprintf(out.why, sizeof(out.why), "Nordic UART RX characteristic is not writable"); continue; }
            if ((svc.chars[n].props & kBleNotify) == 0) { snprintf(out.why, sizeof(out.why), "Nordic UART TX characteristic cannot notify"); continue; }
            out.profile = JcBleProfile::Nus;
            out.svc = static_cast<int8_t>(s); out.write_chr = w; out.notify_chr = n;
            out.write_with_response = (svc.chars[w].props & kBleWriteNoRsp) == 0;
            out.why[0] = '\0';
            return true;
        }
    }
    for (uint8_t s = 0; s < d.n; ++s) {
        const JcBleService& svc = d.services[s];
        if (jc_ble_uuid_eq(svc.uuid, kJcHm10Service)) {
            found_any = true;
            int8_t c1 = -1;
            for (uint8_t c = 0; c < svc.nchars; ++c) {
                if (jc_ble_uuid_eq(svc.chars[c].uuid, kJcHm10Char)) c1 = static_cast<int8_t>(c);
            }
            if (c1 < 0) { snprintf(out.why, sizeof(out.why), "FFE0 service has no FFE1 characteristic"); continue; }
            const uint8_t pr = svc.chars[c1].props;
            if ((pr & (kBleWrite | kBleWriteNoRsp)) == 0) { snprintf(out.why, sizeof(out.why), "FFE1 is not writable"); continue; }
            if ((pr & kBleNotify) == 0) { snprintf(out.why, sizeof(out.why), "FFE1 cannot notify"); continue; }
            out.profile = JcBleProfile::Hm10;
            out.svc = static_cast<int8_t>(s); out.write_chr = c1; out.notify_chr = c1;
            out.write_with_response = (pr & kBleWriteNoRsp) == 0;
            out.why[0] = '\0';
            return true;
        }
    }
    if (!found_any) snprintf(out.why, sizeof(out.why), "no serial profile (needs FFE0/FFE1 or Nordic UART)");
    return false;
}

// ---- scan list ------------------------------------------------------------------------------------------------

constexpr uint8_t kJcBleMaxDevices = 16;

inline bool jc_ble_addr_ok(const char* a) {
    if (a == nullptr || strlen(a) != 17) return false;
    for (int i = 0; i < 17; ++i) {
        const char c = a[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

struct JcBleDevice {
    char    addr[20];
    char    name[28];
    int16_t rssi;
    bool    serial_hint;   // advertised FFE0 or the Nordic UART service
    uint32_t seen_ms;
};

class JcBleScanList {
public:
    void clear() { _n = 0; }
    // Adds or refreshes a device. Names are reduced to printable ASCII; an invalid address is ignored.
    bool add(const char* addr, const char* name, int rssi, bool serial_hint, uint32_t now) {
        if (!jc_ble_addr_ok(addr)) return false;
        for (uint8_t i = 0; i < _n; ++i) {
            if (same_addr(_dev[i].addr, addr)) {
                _dev[i].rssi = static_cast<int16_t>(rssi);
                _dev[i].seen_ms = now;
                _dev[i].serial_hint = _dev[i].serial_hint || serial_hint;
                if (_dev[i].name[0] == '\0' && name != nullptr) set_name(_dev[i], name);
                return true;
            }
        }
        if (_n >= kJcBleMaxDevices) return false;       // bounded: the strongest are kept by the caller re-scanning
        JcBleDevice& d = _dev[_n++];
        memset(&d, 0, sizeof(d));
        snprintf(d.addr, sizeof(d.addr), "%s", addr);
        for (char* p = d.addr; *p; ++p) if (*p >= 'a' && *p <= 'f') *p = static_cast<char>(*p - 32);
        if (name != nullptr) set_name(d, name);
        d.rssi = static_cast<int16_t>(rssi);
        d.serial_hint = serial_hint;
        d.seen_ms = now;
        return true;
    }
    uint8_t count() const { return _n; }
    // Display order: serial-capable devices first, then by signal strength.
    const JcBleDevice* sorted(uint8_t rank) const {
        if (rank >= _n) return nullptr;
        uint8_t order[kJcBleMaxDevices];
        for (uint8_t i = 0; i < _n; ++i) order[i] = i;
        for (uint8_t i = 1; i < _n; ++i) {
            const uint8_t v = order[i];
            int8_t j = static_cast<int8_t>(i) - 1;
            while (j >= 0 && better(_dev[v], _dev[order[j]])) { order[j + 1] = order[j]; --j; }
            order[j + 1] = v;
        }
        return &_dev[order[rank]];
    }
    const JcBleDevice* find(const char* addr) const {
        for (uint8_t i = 0; i < _n; ++i) if (same_addr(_dev[i].addr, addr)) return &_dev[i];
        return nullptr;
    }

    // Case-insensitive address comparison.
    static bool same_addr(const char* a, const char* b) {
        for (; *a && *b; ++a, ++b) {
            char x = *a, y = *b;
            if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
            if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
            if (x != y) return false;
        }
        return *a == *b;
    }

private:
    static bool better(const JcBleDevice& a, const JcBleDevice& b) {
        if (a.serial_hint != b.serial_hint) return a.serial_hint;
        return a.rssi > b.rssi;
    }
    static void set_name(JcBleDevice& d, const char* name) {
        size_t n = 0;
        for (; *name && n + 1 < sizeof(d.name); ++name) d.name[n++] = (*name >= 0x20 && *name <= 0x7E) ? *name : '?';
        d.name[n] = '\0';
    }
    JcBleDevice _dev[kJcBleMaxDevices];
    uint8_t     _n = 0;
};

// Saved device preference (persisted by the settings layer).
struct JcBleSaved {
    bool valid;
    char addr[20];
    char name[28];
};
inline void jc_ble_save(JcBleSaved& s, const char* addr, const char* name) {
    s.valid = jc_ble_addr_ok(addr);
    snprintf(s.addr, sizeof(s.addr), "%s", s.valid ? addr : "");
    snprintf(s.name, sizeof(s.name), "%s", s.valid && name != nullptr ? name : "");
}
inline void jc_ble_forget(JcBleSaved& s) { s.valid = false; s.addr[0] = '\0'; s.name[0] = '\0'; }

// ---- radio abstraction ---------------------------------------------------------------------------------------

// Requests are non-blocking from the link's point of view; completion is reported through the JcBleUart::on_*
// functions (they may be called from inside the request on a blocking implementation).
class JcBleRadio {
public:
    virtual ~JcBleRadio() {}
    virtual bool scan_start(uint32_t duration_ms) = 0;
    virtual void scan_stop() = 0;
    virtual bool connect(const char* addr) = 0;
    virtual void disconnect() = 0;
    virtual bool discover() = 0;
    virtual bool subscribe(const JcBleSelection& sel) = 0;
    // One GATT write of at most `len` <= mtu-3 bytes. False = the radio refused it.
    virtual bool write(const JcBleSelection& sel, const uint8_t* data, size_t len) = 0;
    virtual uint16_t mtu() const = 0;
};

enum class JcBleState : uint8_t {
    Off,          // not started
    Idle,         // started, not connected, not scanning
    Scanning,
    Connecting,
    Discovering,
    Subscribing,
    Ready,        // serial bridge usable
    Backoff,      // connection lost / failed; waiting to retry the SAVED device
    Failed,       // the peer is unusable (no serial profile); not retried automatically
};

inline const char* jc_ble_state_name(JcBleState s) {
    switch (s) {
        case JcBleState::Off: return "OFF";
        case JcBleState::Idle: return "IDLE";
        case JcBleState::Scanning: return "SCANNING";
        case JcBleState::Connecting: return "CONNECTING";
        case JcBleState::Discovering: return "DISCOVERING";
        case JcBleState::Subscribing: return "SUBSCRIBING";
        case JcBleState::Ready: return "READY";
        case JcBleState::Backoff: return "RECONNECTING";
        case JcBleState::Failed: return "FAILED";
    }
    return "?";
}

struct JcBleDiag {
    uint32_t rx_bytes, tx_bytes, notifications, partial_notifications, rx_dropped, tx_refused, tx_queue_peak;
    uint32_t write_errors, connects, disconnects, reconnect_attempts, scans, invalid_frames;
    char     last_error[96];
};

constexpr size_t kJcBleTxQueue = 256;    // bytes of queued text/commands (whole lines only)
constexpr size_t kJcBleRxChunk = 512;    // most bytes accepted from one notification

class JcBleUart {
public:
    using LineListener = void (*)(void* user, const char* line);

    void set_radio(JcBleRadio* r) { _radio = r; }
    void set_line_listener(LineListener l, void* user) { _line_listener = l; _line_user = user; }

    // ---- operator actions ----
    // Starts the BLE transport (no scan, no connection yet). With a saved device and `auto_connect` it connects to it.
    void begin(const JcBleSaved& saved, bool auto_connect, uint32_t now) {
        reset_link();
        _saved = saved;
        _state = JcBleState::Idle;
        _auto = auto_connect && saved.valid;
        if (_auto) {
            snprintf(_target, sizeof(_target), "%s", saved.addr);
            start_connect(now, true);
        }
    }
    void end() {
        if (_radio != nullptr && _state != JcBleState::Off) { _radio->scan_stop(); _radio->disconnect(); }
        lose_link();
        _tracker.request_disconnect();
        _auto = false;
        _state = JcBleState::Off;
    }
    bool scan(uint32_t now) {
        if (_radio == nullptr || _state == JcBleState::Off || _state == JcBleState::Scanning) return false;
        if (_state != JcBleState::Idle && _state != JcBleState::Failed && _state != JcBleState::Backoff) return false;
        _list.clear();
        const JcBleState before = _state == JcBleState::Backoff ? JcBleState::Backoff : JcBleState::Idle;
        _state_before_scan = before;
        _state = JcBleState::Scanning;       // before the request: a blocking radio reports results from inside it
        _state_since = now;
        if (!_radio->scan_start(kScanMs)) { _state = before; set_error("scan could not start"); return false; }
        ++_diag.scans;
        return true;
    }
    void stop_scan() {
        if (_state == JcBleState::Scanning) { _radio->scan_stop(); _state = _state_before_scan; }
    }
    // The operator picked a device from the list (or the saved one): connect, and remember it once it works.
    bool connect_to(const char* addr, const char* name, uint32_t now) {
        if (_radio == nullptr || _state == JcBleState::Off || !jc_ble_addr_ok(addr)) return false;
        if (_state == JcBleState::Scanning) { _radio->scan_stop(); }
        if (_state == JcBleState::Ready || _state == JcBleState::Connecting || _state == JcBleState::Discovering || _state == JcBleState::Subscribing) {
            _radio->disconnect();
            lose_link();
        }
        snprintf(_target, sizeof(_target), "%s", addr);
        snprintf(_target_name, sizeof(_target_name), "%s", name != nullptr ? name : "");
        _pending_save = true;
        _auto = true;
        start_connect(now, true);
        return true;
    }
    void disconnect(uint32_t now) {            // operator disconnect: no automatic reconnection until asked
        (void)now;
        if (_radio != nullptr) { _radio->scan_stop(); _radio->disconnect(); }
        lose_link();
        _auto = false;
        _tracker.request_disconnect();
        if (_state != JcBleState::Off) _state = JcBleState::Idle;
    }
    // FORGET: drops the saved preference.
    void forget() { jc_ble_forget(_saved); _pending_save = false; _save_dirty = true; }

    // ---- radio events ----
    void on_scan_result(const char* addr, const char* name, int rssi, bool serial_hint, uint32_t now) {
        if (_state == JcBleState::Scanning) _list.add(addr, name, rssi, serial_hint, now);
    }
    void on_connected(bool ok, uint32_t now) {
        if (_state != JcBleState::Connecting) return;
        if (!ok) { fail_attempt("connection failed", now); return; }
        ++_diag.connects;
        _state = JcBleState::Discovering;
        _state_since = now;
        _requested = false;
    }
    void on_disconnected(uint32_t now) {
        if (_state == JcBleState::Off || _state == JcBleState::Idle || _state == JcBleState::Failed) return;
        if (_state == JcBleState::Ready) ++_diag.disconnects;
        lose_link();
        if (_auto) {
            _state = JcBleState::Backoff;
            _tracker.on_closed(now);
        } else {
            _state = JcBleState::Idle;
        }
        set_error("link lost");
    }
    void on_discovery(const JcBleDiscovery& d, uint32_t now) {
        if (_state != JcBleState::Discovering) return;
        JcBleSelection sel;
        if (!jc_ble_select(d, sel)) {
            // The peer is not a usable serial bridge: not retried (it would fail the same way).
            set_error(sel.why);
            _radio->disconnect();
            lose_link();
            _auto = false;
            _state = JcBleState::Failed;
            return;
        }
        _sel = sel;
        _state = JcBleState::Subscribing;
        _state_since = now;
        _requested = false;
    }
    void on_subscribed(bool ok, uint32_t now) {
        if (_state != JcBleState::Subscribing) return;
        if (!ok) {
            set_error("could not enable notifications");
            _radio->disconnect();
            lose_link();
            _state = JcBleState::Backoff;
            _tracker.on_closed(now);
            return;
        }
        _state = JcBleState::Ready;
        _state_since = now;
        _tracker.request_connect(now);
        _tracker.on_connected();
        _next_query_ms = now + 200;
        _rx.reset();
        if (_pending_save) {                       // a device that actually works becomes the saved preference
            jc_ble_save(_saved, _target, _target_name);
            _pending_save = false;
            _save_dirty = true;
        }
    }
    // A notification from the peripheral. Arbitrary fragmentation is fine: the stream splitter joins pieces.
    void on_notify(const uint8_t* data, size_t n, uint32_t now) {
        if (_state != JcBleState::Ready || data == nullptr) return;
        ++_diag.notifications;
        if (n > kJcBleRxChunk) {
            _diag.rx_dropped += static_cast<uint32_t>(n - kJcBleRxChunk);
            _rx.reset();                           // a truncated stream is not trusted: re-sync at the next line
            n = kJcBleRxChunk;
            set_error("receive overflow: data dropped");
        }
        _diag.rx_bytes += static_cast<uint32_t>(n);
        if (data[n - 1] != '\n' && data[n - 1] != '>') ++_diag.partial_notifications;   // a piece of a longer line / frame
        for (size_t i = 0; i < n; ++i) {
            switch (_rx.push(data[i])) {
                case JcStreamSplitter::Event::None: break;
                case JcStreamSplitter::Event::InvalidFrame: _tracker.on_invalid(); ++_diag.invalid_frames; break;
                case JcStreamSplitter::Event::LineOverflow: _tracker.on_invalid(); ++_diag.invalid_frames; break;
                case JcStreamSplitter::Event::Frame: accept_frame(_rx.frame(), _rx.frame_length(), now); break;
                case JcStreamSplitter::Event::Line:
                    if (_line_listener != nullptr) _line_listener(_line_user, _rx.line());
                    break;
            }
        }
    }

    // The platform receive buffer overflowed: `n` bytes were lost, so the stream is not trusted until the next line.
    void on_rx_overflow(size_t n) {
        _diag.rx_dropped += static_cast<uint32_t>(n);
        _rx.reset();
        set_error("receive buffer overflow: data dropped");
    }

    // ---- transmit (same policy as the other links) ----
    bool send_query_line(const char* line) {
        if (!jc_query_allowed(line)) { ++_blocked; set_error("blocked: not an allowlisted read-only query"); return false; }
        if (_state != JcBleState::Ready) return false;
        char buf[72];
        const int n = snprintf(buf, sizeof(buf), "%s\n", line);
        return n > 0 && enqueue(buf, static_cast<size_t>(n));
    }
#if JC_FULL_CONTROL_TEST
    // EXPERIMENTAL FULL-CONTROL build only: raw framing for the unified command system; the shared policy is
    // applied here once, before queuing. The ordinary firmware has no such function.
    bool send_command_bytes(const char* bytes, size_t length) {
        if (_state != JcBleState::Ready || bytes == nullptr || length == 0 || !jc_command_bytes_allowed(bytes, length)) {
            ++_blocked;
            return false;
        }
        if (length == 1) return priority(static_cast<uint8_t>(bytes[0]));   // real-time byte: ahead of queued text
        return enqueue(bytes, length);
    }
#endif

    // ---- main loop ----
    void service(uint32_t now) {
        if (_radio == nullptr || _state == JcBleState::Off) return;
        switch (_state) {
            case JcBleState::Scanning:
                if (now - _state_since > kScanMs + 500u) { _radio->scan_stop(); _state = _state_before_scan; }
                break;
            case JcBleState::Backoff:
                if (_auto && _tracker.should_retry(now)) { ++_diag.reconnect_attempts; start_connect(now, false); }
                break;
            case JcBleState::Connecting:
                if (!_requested) {
                    _requested = true;
                    if (!_radio->connect(_target)) { fail_attempt("connect request refused", now); break; }
                } else if (now - _state_since > kConnectMs) {
                    _radio->disconnect();
                    fail_attempt("connection timed out", now);
                }
                break;
            case JcBleState::Discovering:
                if (!_requested) {
                    _requested = true;
                    if (!_radio->discover()) { set_error("service discovery refused"); _radio->disconnect(); lose_link(); _state = JcBleState::Backoff; _tracker.on_closed(now); }
                } else if (now - _state_since > kStepMs) {
                    set_error("service discovery timed out");
                    _radio->disconnect(); lose_link(); _state = JcBleState::Backoff; _tracker.on_closed(now);
                }
                break;
            case JcBleState::Subscribing:
                if (!_requested) {
                    _requested = true;
                    if (!_radio->subscribe(_sel)) { set_error("subscribe refused"); _radio->disconnect(); lose_link(); _state = JcBleState::Backoff; _tracker.on_closed(now); }
                } else if (now - _state_since > kStepMs) {
                    set_error("subscribe timed out");
                    _radio->disconnect(); lose_link(); _state = JcBleState::Backoff; _tracker.on_closed(now);
                }
                break;
            case JcBleState::Ready:
                if (static_cast<int32_t>(now - _next_query_ms) >= 0) {
                    _next_query_ms = now + 1000;
                    priority('?');
                }
                pump(now);
                break;
            default: break;
        }
    }

    // ---- observers ----
    JcBleState state() const { return _state; }
    bool ready() const { return _state == JcBleState::Ready; }
    // True once, after a usable link went away: the application invalidates machine status and pending work.
    bool take_link_lost() { const bool l = _lost_flag; _lost_flag = false; return l; }
    bool take_save_request(JcBleSaved& out) { if (!_save_dirty) return false; out = _saved; _save_dirty = false; return true; }
    const JcBleSaved& saved() const { return _saved; }
    const JcBleScanList& devices() const { return _list; }
    const JcBleSelection& selection() const { return _sel; }
    const JcBleDiag& diag() const { return _diag; }
    uint32_t blocked() const { return _blocked; }
    uint16_t chunk_size() const {
        const uint16_t m = _radio != nullptr ? _radio->mtu() : 23;
        const uint16_t c = m > 23 ? static_cast<uint16_t>(m - 3) : 20;
        return c > 100 ? 100 : c;
    }
    size_t tx_queued() const { return _txn + _pn; }
    const JcLiveMachineStatus& status() const { return _status; }
    bool status_is_fresh(uint32_t now) const { return _state == JcBleState::Ready && _tracker.fresh(now); }
    JcFluidState link_state(uint32_t now) const {
        if (_state == JcBleState::Ready) return _tracker.state(now);
        if (_state == JcBleState::Backoff) return JcFluidState::Lost;
        if (_state == JcBleState::Off || _state == JcBleState::Idle || _state == JcBleState::Scanning) return JcFluidState::Disconnected;
        if (_state == JcBleState::Failed) return JcFluidState::Error;
        return JcFluidState::FluidNcConnecting;
    }
    uint32_t frames() const { return _tracker.frames(); }
    const char* target() const { return _target; }
    uint32_t ms_until_retry(uint32_t now) const { return _tracker.ms_until_retry(now); }

private:
    static constexpr uint32_t kScanMs = 8000, kConnectMs = 8000, kStepMs = 6000;

    void reset_link() {
        _rx.reset();
        _txn = 0; _pn = 0;
        _status = JcLiveMachineStatus {};
        _lost_flag = false;
        _requested = false;
        _pending_save = false;
        _target[0] = '\0'; _target_name[0] = '\0';
        _save_dirty = false;
        _tracker = JcLinkTracker();
        _state_before_scan = JcBleState::Idle;
        memset(&_diag, 0, sizeof(_diag));
        _sel = JcBleSelection();
    }
    // The link is gone: queued bytes and the half-parsed stream are discarded and NEVER sent later.
    void lose_link() {
        const bool was_usable = _state == JcBleState::Ready || _tracker.up();
        _txn = 0; _pn = 0;
        _rx.reset();
        _write_errors_run = 0;
        _status.has_data = false;
        if (was_usable) _lost_flag = true;
    }
    void start_connect(uint32_t now, bool first) {
        _state = JcBleState::Connecting;
        _state_since = now;
        _requested = false;
        if (first) _tracker.request_connect(now);
    }
    void fail_attempt(const char* why, uint32_t now) {
        set_error(why);
        lose_link();
        _state = _auto ? JcBleState::Backoff : JcBleState::Idle;
        if (_auto) _tracker.on_closed(now);
    }
    void set_error(const char* e) { snprintf(_diag.last_error, sizeof(_diag.last_error), "%s", e); }

    bool enqueue(const char* bytes, size_t n) {
        if (_txn + n > kJcBleTxQueue) { ++_diag.tx_refused; set_error("transmit queue full: command refused"); return false; }
        memcpy(_tx + _txn, bytes, n);
        _txn += n;
        if (_txn > _diag.tx_queue_peak) _diag.tx_queue_peak = static_cast<uint32_t>(_txn);
        return true;
    }
    bool priority(uint8_t b) {
        if (_pn >= sizeof(_prio)) { ++_diag.tx_refused; return false; }
        _prio[_pn++] = b;
        return true;
    }
    void pump(uint32_t now) {
        if (now - _last_write_ms < 8u) return;                // paced: BLE stacks drop writes that come too fast
        uint8_t chunk[100];
        size_t n = 0;
        bool took_priority = false;
        if (_pn > 0) {
            took_priority = true;                                        // real-time bytes first, on their own
            chunk[0] = _prio[0];
            memmove(_prio, _prio + 1, --_pn);
            n = 1;
        } else if (_txn > 0) {
            n = _txn < chunk_size() ? _txn : chunk_size();
            memcpy(chunk, _tx, n);
        } else {
            return;
        }
        _last_write_ms = now;
        const bool from_text = !took_priority;
        if (!_radio->write(_sel, chunk, n)) {
            ++_diag.write_errors;
            set_error("GATT write failed");
            if (++_write_errors_run >= 3) { _radio->disconnect(); on_disconnected(now); }
            return;
        }
        _write_errors_run = 0;
        _diag.tx_bytes += static_cast<uint32_t>(n);
        if (from_text) {                                      // only text chunks shrink the text queue
            memmove(_tx, _tx + n, _txn - n);
            _txn -= n;
        }
    }

    void accept_frame(const char* frame, uint16_t length, uint32_t now) {
        JcStatusFrame parsed {};
        if (!jc_parse_status_frame(frame, length, parsed)) { _tracker.on_invalid(); ++_diag.invalid_frames; return; }
        jc_apply_status_frame(_status, parsed, now);
        _tracker.on_frame(now);
    }

    JcBleRadio*  _radio = nullptr;
    JcBleState   _state = JcBleState::Off, _state_before_scan = JcBleState::Idle;
    JcBleSaved   _saved {};
    JcBleScanList _list;
    JcBleSelection _sel {};
    JcBleDiag    _diag {};
    JcStreamSplitter _rx;
    JcLinkTracker _tracker;
    JcLiveMachineStatus _status {};
    LineListener _line_listener = nullptr;
    void*        _line_user = nullptr;
    char         _target[20] = "";
    char         _target_name[28] = "";
    bool         _auto = false, _requested = false, _pending_save = false, _save_dirty = false, _lost_flag = false;
    uint32_t     _state_since = 0, _next_query_ms = 0, _last_write_ms = 0, _blocked = 0;
    uint8_t      _write_errors_run = 0;
    uint8_t      _tx[kJcBleTxQueue];
    size_t       _txn = 0;
    uint8_t      _prio[8];
    size_t       _pn = 0;
};

}  // namespace jc8048w550
