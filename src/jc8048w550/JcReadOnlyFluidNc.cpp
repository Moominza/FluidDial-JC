// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcReadOnlyFluidNc.hpp"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "GrblParserC.h"
#include "JcStatusFrame.hpp"

namespace jc8048w550 {
namespace {

constexpr uint16_t kFluidNcTelnetPort = 23;
constexpr uint32_t kStatusPollIntervalMs = 1000;
constexpr uint32_t kStatusStaleMs = 3000;
constexpr uint32_t kTcpRetryDelayMs = 2000;
constexpr uint32_t kTcpConnectTimeoutMs = 4000;
constexpr uint32_t kStatusResponseTimeoutMs = 5000;
constexpr size_t kTcpReadBudget = 512;

JcReadOnlyFluidNc* g_active_link = nullptr;

struct ValidatedStatus {
    bool machine_position;
};

bool valid_state(const char* state) {
    return jc_valid_machine_state(state);
}

bool validate_status_frame(const char* frame, size_t length,
                           ValidatedStatus& validated) {
    JcStatusFrame parsed {};
    if (!jc_parse_status_frame(frame, length, parsed)) {
        return false;
    }
    validated.machine_position = parsed.machine_position;
    return true;
}

bool is_ipv4_literal(const char* host) {
    IPAddress address;
    return address.fromString(host);
}

bool valid_host(const char* host) {
    if (host == nullptr || host[0] == '\0' || strlen(host) >= 64) {
        return false;
    }
    if (is_ipv4_literal(host)) {
        return true;
    }
    for (const char* p = host; *p != '\0'; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_bssid(const uint8_t* bssid) {
    if (bssid == nullptr || (bssid[0] & 0x01) != 0) {
        return false;
    }
    bool nonzero = false;
    for (uint8_t i = 0; i < 6; ++i) {
        nonzero = nonzero || bssid[i] != 0;
    }
    return nonzero;
}

} // namespace

struct JcReadOnlyFluidNc::DnsRequest {
    char host[64];
    uint32_t generation;
};

struct JcReadOnlyFluidNc::DnsResult {
    uint32_t address;
    uint32_t generation;
    bool success;
};

JcReadOnlyFluidNc::JcReadOnlyFluidNc() {
    g_active_link = this;
}

void JcReadOnlyFluidNc::begin() {
    Preferences preferences;
    if (!preferences.begin("jcnetwork", false)) {
        set_error("NVS configuration open failed");
        Serial.println("net: failed to open local Wi-Fi settings");
        return;
    }

    const String ssid = preferences.isKey("ssid")
        ? preferences.getString("ssid", "") : "";
    const String password = preferences.isKey("pass")
        ? preferences.getString("pass", "") : "";
    const String host = preferences.isKey("host")
        ? preferences.getString("host", "") : "";
    if (preferences.isKey("bssid")
        && preferences.getBytesLength("bssid") == sizeof(_config.bssid)
        && preferences.getBytes("bssid", _config.bssid, sizeof(_config.bssid))
            == sizeof(_config.bssid)) {
        const uint8_t channel = preferences.getUChar("channel", 0);
        if (valid_bssid(_config.bssid) && channel >= 1 && channel <= 14) {
            _config.channel = channel;
            _config.bssid_pinned = true;
        }
    }
    preferences.end();

    if (ssid.length() <= 32 && password.length() <= 63 && host.length() <= 63) {
        strlcpy(_config.ssid, ssid.c_str(), sizeof(_config.ssid));
        strlcpy(_config.password, password.c_str(), sizeof(_config.password));
        strlcpy(_config.host, host.c_str(), sizeof(_config.host));
        _config.valid = ssid.length() > 0 && valid_host(_config.host);
    }
    start_dns_worker();
    Serial.printf("net: local configuration %s; auto-connect disabled\n",
                  _config.valid ? "loaded" : "not configured");
}

bool JcReadOnlyFluidNc::save_config(const JcWifiConfig& config) {
    if (config.ssid[0] == '\0' || strnlen(config.ssid, sizeof(config.ssid)) > 32
        || strnlen(config.password, sizeof(config.password)) > 63
        || strnlen(config.host, sizeof(config.host)) > 63
        || !valid_host(config.host)
        || (config.bssid_pinned
            && (!valid_bssid(config.bssid)
                || config.channel < 1 || config.channel > 14))) {
        set_error("Invalid Wi-Fi settings");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin("jcnetwork", false)) {
        set_error("NVS configuration open failed");
        Serial.println("net: failed to open local Wi-Fi settings");
        return false;
    }
    bool saved = preferences.putString("ssid", config.ssid) == strlen(config.ssid)
        && preferences.putString("pass", config.password) == strlen(config.password)
        && preferences.putString("host", config.host) == strlen(config.host);
    if (saved && config.bssid_pinned) {
        saved = preferences.putBytes("bssid", config.bssid,
                                    sizeof(config.bssid)) == sizeof(config.bssid)
            && preferences.putUChar("channel", config.channel) == sizeof(config.channel);
    } else if (saved) {
        saved = (!preferences.isKey("bssid") || preferences.remove("bssid"))
            && (!preferences.isKey("channel") || preferences.remove("channel"));
    }
    preferences.end();
    if (!saved) {
        set_error("NVS configuration write failed");
        Serial.println("net: failed to save local Wi-Fi settings");
        return false;
    }

    _config = config;
    _config.valid = true;
    _last_error[0] = '\0';
    Serial.println("net: local Wi-Fi settings saved; credentials were not logged");
    return true;
}

bool JcReadOnlyFluidNc::start_wifi_scan() {
    if (_connect_requested || _wifi_scan_running) {
        set_error("Disconnect before scanning for Wi-Fi networks");
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.scanDelete();
    _scanned_network_count = 0;
    const int16_t result = WiFi.scanNetworks(true, false);
    if (result == WIFI_SCAN_FAILED) {
        set_error("Wi-Fi scan could not start");
        return false;
    }
    _wifi_scan_running = true;
    _last_error[0] = '\0';
    Serial.println("net: asynchronous Wi-Fi scan started");
    return true;
}

bool JcReadOnlyFluidNc::wifi_scan_running() const {
    return _wifi_scan_running;
}

uint8_t JcReadOnlyFluidNc::scanned_network_count() const {
    return _scanned_network_count;
}

const JcWifiNetwork* JcReadOnlyFluidNc::scanned_network(uint8_t index) const {
    return index < _scanned_network_count ? &_scanned_networks[index] : nullptr;
}

void JcReadOnlyFluidNc::connect() {
    if (!_config.valid) {
        set_error("Configure SSID and FluidNC host first");
        Serial.println("net: connect refused; Wi-Fi settings are incomplete");
        return;
    }

    close_tcp(false);
    _connect_requested = true;
    _wifi_was_connected = false;
    _status_frame_length = 0;
    _frame_active = false;
    _frame_invalid = false;
    _resolved_address[0] = '\0';
    _last_error[0] = '\0';
    ++_dns_generation;
    ++_reconnect_attempts;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    Serial.printf("net: Wi-Fi connect attempt %lu\n",
                  static_cast<unsigned long>(_reconnect_attempts));
    WiFi.begin(_config.ssid, _config.password[0] ? _config.password : nullptr,
               _config.bssid_pinned ? _config.channel : 0,
               _config.bssid_pinned ? _config.bssid : nullptr);
    _next_retry_ms = millis() + 15000;
    _state = JcFluidState::WifiConnecting;
}

void JcReadOnlyFluidNc::disconnect() {
    _connect_requested = false;
    ++_dns_generation;
    close_tcp(false);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    _wifi_was_connected = false;
    _state = JcFluidState::Disconnected;
    _last_error[0] = '\0';
    Serial.println("net: disconnected by operator; no CNC command sent");
}

const JcWifiConfig& JcReadOnlyFluidNc::config() const {
    return _config;
}

bool JcReadOnlyFluidNc::wifi_connected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool JcReadOnlyFluidNc::connect_requested() const {
    return _connect_requested;
}

JcFluidState JcReadOnlyFluidNc::state() const {
    if (!_connect_requested) {
        return JcFluidState::Disconnected;
    }
    if (!wifi_connected()) {
        return _wifi_was_connected ? JcFluidState::Lost
                                   : JcFluidState::WifiConnecting;
    }
    if (_connecting_socket >= 0) {
        return JcFluidState::FluidNcConnecting;
    }
    if (_socket < 0) {
        if (_state == JcFluidState::Resolving) {
            return JcFluidState::Resolving;
        }
        if (_last_error[0] != '\0') {
            return JcFluidState::Error;
        }
        return _status.has_data ? JcFluidState::Lost
                                : JcFluidState::FluidNcConnecting;
    }
    if (_frame_invalid) {
        return JcFluidState::InvalidData;
    }
    if (!_status.has_data) {
        return JcFluidState::FluidNcConnecting;
    }
    if (last_status_age_ms() > kStatusStaleMs) {
        return JcFluidState::Stale;
    }
    return JcFluidState::Connected;
}

const JcLiveMachineStatus& JcReadOnlyFluidNc::status() const {
    return _status;
}

bool JcReadOnlyFluidNc::status_is_fresh() const {
    return state() == JcFluidState::Connected;
}

uint32_t JcReadOnlyFluidNc::reconnect_attempts() const {
    return _reconnect_attempts;
}

uint32_t JcReadOnlyFluidNc::parse_errors() const {
    return _parse_errors;
}

uint32_t JcReadOnlyFluidNc::blocked_transmissions() const {
    return _blocked_transmissions;
}

uint32_t JcReadOnlyFluidNc::last_status_age_ms() const {
    return _status.has_data ? millis() - _status.last_valid_ms : UINT32_MAX;
}

const char* JcReadOnlyFluidNc::last_error() const {
    return _last_error;
}

const char* JcReadOnlyFluidNc::state_name(JcFluidState state) {
    switch (state) {
        case JcFluidState::Disconnected: return "Wi-Fi disconnected";
        case JcFluidState::WifiConnecting: return "Wi-Fi connecting";
        case JcFluidState::WifiConnected: return "Wi-Fi connected";
        case JcFluidState::Resolving: return "Resolving FluidNC";
        case JcFluidState::FluidNcConnecting: return "FluidNC connecting";
        case JcFluidState::Connected: return "FluidNC connected";
        case JcFluidState::Lost: return "FluidNC connection lost";
        case JcFluidState::Stale: return "STALE machine data";
        case JcFluidState::InvalidData: return "Invalid machine data";
        case JcFluidState::Error: return "Connection error";
    }
    return "Connection unknown";
}

const char* JcReadOnlyFluidNc::reference_name(JcCoordinateReference reference) {
    switch (reference) {
        case JcCoordinateReference::Machine: return "MACHINE";
        case JcCoordinateReference::Work: return "WORK";
        case JcCoordinateReference::Unknown: return "NO POSITION";
    }
    return "NO POSITION";
}

void JcReadOnlyFluidNc::begin_status_report() {
    _report_has_state = false;
    _report_has_axes = false;
    _report_invalid = false;
    _report_reference = JcCoordinateReference::Unknown;
}

void JcReadOnlyFluidNc::accept_machine_state(const char* state) {
    if (state == nullptr || !valid_state(state)
        || strlen(state) >= sizeof(_report_state)) {
        _report_invalid = true;
        return;
    }
    strlcpy(_report_state, state, sizeof(_report_state));
    _report_has_state = true;
}

void JcReadOnlyFluidNc::accept_axis_positions(const float axes[4],
                                             bool machine_position) {
    if (axes == nullptr) {
        _report_invalid = true;
        return;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (!isfinite(axes[i])) {
            _report_invalid = true;
            return;
        }
        _report_axes[i] = axes[i];
    }
    _report_reference = machine_position ? JcCoordinateReference::Machine
                                         : JcCoordinateReference::Work;
    _report_has_axes = true;
}

void JcReadOnlyFluidNc::end_status_report() {
    if (_report_invalid || !_report_has_state || !_report_has_axes) {
        record_invalid_frame("Status report missing valid state or four-axis positions");
        return;
    }
    const uint32_t now = millis();
    if (_status.has_data) {
        _status.update_interval_ms = now - _status.last_valid_ms;
    }
    memcpy(_status.axes, _report_axes, sizeof(_status.axes));
    strlcpy(_status.machine_state, _report_state, sizeof(_status.machine_state));
    _status.coordinate_reference = _report_reference;
    _status.last_valid_ms = now;
    _status.has_data = true;
    ++_status.sequence;
    _frame_invalid = false;
    _last_error[0] = '\0';
}

void JcReadOnlyFluidNc::record_invalid_frame(const char* reason) {
    _frame_invalid = true;
    ++_parse_errors;
    if (reason != nullptr) {
        strlcpy(_last_error, reason, sizeof(_last_error));
    }
    if (_parse_errors == 1 || (_parse_errors % 10) == 0) {
        Serial.printf("net: invalid status frame count=%lu reason=%s\n",
                      static_cast<unsigned long>(_parse_errors),
                      _last_error);
    }
}

void JcReadOnlyFluidNc::parser_putchar(uint8_t byte) {
    if (byte != static_cast<uint8_t>(StatusReport)) {
        ++_blocked_transmissions;
        if (_blocked_transmissions == 1) {
            Serial.println("SAFETY: blocked non-status CNC transmission");
        }
        return;
    }
    if (_socket < 0) {
        return;
    }
    const uint8_t status_query = static_cast<uint8_t>(StatusReport);
    const ssize_t sent = ::send(_socket, &status_query, 1, MSG_DONTWAIT);
    if (sent == 1) {
        _last_status_attempt_ms = millis();
        return;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        _next_status_poll_ms = millis() + 100;
        return;
    }
    set_error("Status query send failed");
    close_tcp(true);
}

void JcReadOnlyFluidNc::start_dns_worker() {
    if (_dns_request_queue == nullptr) {
        _dns_request_queue = xQueueCreate(1, sizeof(DnsRequest));
        _dns_result_queue = xQueueCreate(1, sizeof(DnsResult));
    }
    if (_dns_request_queue == nullptr || _dns_result_queue == nullptr) {
        set_error("DNS worker queue allocation failed");
        return;
    }
    if (_dns_task == nullptr) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            dns_worker, "jc_dns", 4096, this, 1, &_dns_task, 0);
        if (created != pdPASS) {
            _dns_task = nullptr;
            set_error("DNS worker creation failed");
        }
    }
}

void JcReadOnlyFluidNc::dns_worker(void* context) {
    auto* self = static_cast<JcReadOnlyFluidNc*>(context);
    DnsRequest request {};
    for (;;) {
        if (xQueueReceive(self->_dns_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        DnsResult result {};
        result.generation = request.generation;
        IPAddress resolved;
        const size_t host_length = strlen(request.host);
        if (host_length > 6
            && strcasecmp(request.host + host_length - 6, ".local") == 0) {
            char base[64];
            memcpy(base, request.host, host_length - 6);
            base[host_length - 6] = '\0';
            esp_ip4_addr_t address {};
            result.success = mdns_query_a(base, 2000, &address) == ESP_OK;
            if (result.success) {
                result.address = address.addr;
            }
        } else {
            result.success = WiFi.hostByName(request.host, resolved) == 1;
            if (result.success) {
                result.address = static_cast<uint32_t>(resolved);
            }
        }
        xQueueOverwrite(self->_dns_result_queue, &result);
    }
}

void JcReadOnlyFluidNc::resolve_controller() {
    IPAddress address;
    if (address.fromString(_config.host)) {
        strlcpy(_resolved_address, address.toString().c_str(),
                sizeof(_resolved_address));
        begin_tcp_connect(_resolved_address);
        return;
    }
    if (_dns_request_queue == nullptr || _dns_task == nullptr) {
        set_error("DNS resolver unavailable");
        return;
    }

    DnsRequest request {};
    strlcpy(request.host, _config.host, sizeof(request.host));
    request.generation = ++_dns_generation;
    _state = JcFluidState::Resolving;
    if (xQueueSend(_dns_request_queue, &request, 0) != pdTRUE) {
        set_error("DNS resolver busy");
        _next_retry_ms = millis() + 5000;
    } else {
        Serial.println("net: resolving FluidNC host asynchronously");
    }
}

void JcReadOnlyFluidNc::begin_tcp_connect(const char* address) {
    if (!_connect_requested || !wifi_connected()) {
        return;
    }
    IPAddress ip;
    if (!ip.fromString(address)) {
        set_error("Invalid resolved FluidNC address");
        return;
    }

    ++_reconnect_attempts;
    Serial.printf("net: read-only FluidNC TCP attempt %lu to %s:%u\n",
                  static_cast<unsigned long>(_reconnect_attempts), address,
                  kFluidNcTelnetPort);
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        set_error("TCP socket creation failed");
        _next_retry_ms = millis() + kTcpRetryDelayMs;
        return;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        ::close(fd);
        set_error("TCP nonblocking setup failed");
        _next_retry_ms = millis() + kTcpRetryDelayMs;
        return;
    }

    sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kFluidNcTelnetPort);
    destination.sin_addr.s_addr = static_cast<uint32_t>(ip);
    const int result = ::connect(fd, reinterpret_cast<sockaddr*>(&destination),
                                 sizeof(destination));
    if (result == 0) {
        _socket = fd;
        _connecting_socket = -1;
        _next_status_poll_ms = millis();
        _last_status_attempt_ms = 0;
        _status_frame_length = 0;
        _frame_active = false;
        _state = JcFluidState::FluidNcConnecting;
        Serial.println("net: TCP connected; only '?' status queries are permitted");
        return;
    }
    if (errno == EINPROGRESS) {
        _connecting_socket = fd;
        _connect_deadline_ms = millis() + kTcpConnectTimeoutMs;
        _state = JcFluidState::FluidNcConnecting;
        return;
    }

    ::close(fd);
    set_error("FluidNC TCP connection failed");
    _next_retry_ms = millis() + kTcpRetryDelayMs;
}

void JcReadOnlyFluidNc::poll_tcp_connect() {
    if (_connecting_socket < 0) {
        return;
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(_connecting_socket, &writable);
    timeval timeout {};
    const int selected = ::select(_connecting_socket + 1, nullptr, &writable,
                                  nullptr, &timeout);
    if (selected > 0 && FD_ISSET(_connecting_socket, &writable)) {
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        ::getsockopt(_connecting_socket, SOL_SOCKET, SO_ERROR, &socket_error,
                     &error_length);
        if (socket_error == 0) {
            _socket = _connecting_socket;
            _connecting_socket = -1;
            _next_status_poll_ms = millis();
            _last_status_attempt_ms = 0;
            _status_frame_length = 0;
            _frame_active = false;
            _state = JcFluidState::FluidNcConnecting;
            Serial.println("net: TCP connected; only '?' status queries are permitted");
            return;
        }
        ::close(_connecting_socket);
        _connecting_socket = -1;
        set_error("FluidNC TCP connection failed");
        _next_retry_ms = millis() + kTcpRetryDelayMs;
        return;
    }
    if (static_cast<int32_t>(millis() - _connect_deadline_ms) >= 0) {
        ::close(_connecting_socket);
        _connecting_socket = -1;
        set_error("FluidNC TCP connection timed out");
        _next_retry_ms = millis() + kTcpRetryDelayMs;
    }
}

void JcReadOnlyFluidNc::poll_tcp_io() {
    if (_socket < 0) {
        return;
    }
    uint8_t buffer[128];
    size_t total = 0;
    while (total < kTcpReadBudget) {
        const ssize_t received = ::recv(_socket, buffer, sizeof(buffer),
                                        MSG_DONTWAIT);
        if (received > 0) {
            total += static_cast<size_t>(received);
            for (ssize_t i = 0; i < received; ++i) {
                consume_tcp_byte(buffer[i]);
            }
            continue;
        }
        if (received == 0) {
            set_error("FluidNC closed the connection");
            close_tcp(true);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        set_error("FluidNC receive failed");
        close_tcp(true);
        return;
    }

    if (_last_status_attempt_ms != 0
        && millis() - _last_status_attempt_ms > kStatusResponseTimeoutMs
        && !_status.has_data) {
        set_error("No valid FluidNC status received");
        close_tcp(true);
    } else if (_last_status_attempt_ms != 0 && _status.has_data
               && millis() - _status.last_valid_ms > kStatusResponseTimeoutMs) {
        set_error("FluidNC status response timed out");
        close_tcp(true);
    }
}

void JcReadOnlyFluidNc::close_tcp(bool schedule_retry) {
    if (_connecting_socket >= 0) {
        ::close(_connecting_socket);
        _connecting_socket = -1;
    }
    if (_socket >= 0) {
        ::shutdown(_socket, SHUT_RDWR);
        ::close(_socket);
        _socket = -1;
    }
    _status_frame_length = 0;
    _frame_active = false;
    _last_status_attempt_ms = 0;
    if (schedule_retry && _connect_requested && wifi_connected()) {
        _next_retry_ms = millis() + kTcpRetryDelayMs;
        _state = JcFluidState::Lost;
    }
}

void JcReadOnlyFluidNc::service_wifi() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !_wifi_was_connected) {
        _wifi_was_connected = true;
        _next_retry_ms = 0;
        _last_error[0] = '\0';
        Serial.printf("net: Wi-Fi connected; local IP %s\n",
                      WiFi.localIP().toString().c_str());
        MDNS.end();
        if (!MDNS.begin("jc8048w550")) {
            Serial.println("net: mDNS responder start failed");
        }
        resolve_controller();
    } else if (!connected && _wifi_was_connected) {
        _wifi_was_connected = false;
        ++_dns_generation;
        close_tcp(false);
        _state = JcFluidState::Lost;
        _next_retry_ms = millis() + 2000;
        set_error("Wi-Fi connection lost");
        Serial.println("net: Wi-Fi disconnected; live data is stale");
    }
    if (!connected && _connect_requested && _next_retry_ms != 0
        && static_cast<int32_t>(millis() - _next_retry_ms) >= 0) {
        _next_retry_ms = millis() + 15000;
        ++_reconnect_attempts;
        Serial.printf("net: Wi-Fi retry attempt %lu\n",
                      static_cast<unsigned long>(_reconnect_attempts));
        WiFi.disconnect(false, false);
        WiFi.begin(_config.ssid, _config.password[0] ? _config.password : nullptr,
                   _config.bssid_pinned ? _config.channel : 0,
                   _config.bssid_pinned ? _config.bssid : nullptr);
    }
}

void JcReadOnlyFluidNc::service_wifi_scan() {
    if (!_wifi_scan_running) {
        return;
    }

    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
        return;
    }
    if (result == WIFI_SCAN_FAILED) {
        _wifi_scan_running = false;
        set_error("Wi-Fi scan failed");
        WiFi.scanDelete();
        return;
    }

    _scanned_network_count = 0;
    for (int16_t i = 0; i < result
         && _scanned_network_count < kJcMaxScannedNetworks; ++i) {
        const String ssid = WiFi.SSID(i);
        const int32_t channel = WiFi.channel(i);
        uint8_t* bssid = WiFi.BSSID(i);
        if (ssid.isEmpty() || ssid.length() > 32 || bssid == nullptr
            || channel < 1 || channel > 14 || !valid_bssid(bssid)) {
            continue;
        }

        JcWifiNetwork& network = _scanned_networks[_scanned_network_count++];
        strlcpy(network.ssid, ssid.c_str(), sizeof(network.ssid));
        memcpy(network.bssid, bssid, sizeof(network.bssid));
        network.channel = static_cast<uint8_t>(channel);
        network.rssi = static_cast<int8_t>(WiFi.RSSI(i));
    }
    _wifi_scan_running = false;
    WiFi.scanDelete();
    _last_error[0] = '\0';
    Serial.printf("net: Wi-Fi scan complete; %u access point(s) available\n",
                  static_cast<unsigned>(_scanned_network_count));
}

void JcReadOnlyFluidNc::service_reconnect() {
    if (!_connect_requested || !wifi_connected()
        || _socket >= 0 || _connecting_socket >= 0
        || _state == JcFluidState::Resolving
        || _next_retry_ms == 0
        || static_cast<int32_t>(millis() - _next_retry_ms) < 0) {
        return;
    }
    _next_retry_ms = 0;
    if (_resolved_address[0] != '\0') {
        begin_tcp_connect(_resolved_address);
    } else {
        resolve_controller();
    }
}

void JcReadOnlyFluidNc::service_status_poll() {
    if (_socket < 0 || !_connect_requested
        || static_cast<int32_t>(millis() - _next_status_poll_ms) < 0) {
        return;
    }
    fnc_realtime(StatusReport);
    _next_status_poll_ms = millis() + kStatusPollIntervalMs;
}

void JcReadOnlyFluidNc::poll() {
    service_wifi_scan();
    handle_dns_result();
    if (!_connect_requested) {
        return;
    }
    service_wifi();
    if (!wifi_connected()) {
        return;
    }
    poll_tcp_connect();
    poll_tcp_io();
    service_reconnect();
    service_status_poll();
}

void JcReadOnlyFluidNc::handle_dns_result() {
    if (_dns_result_queue == nullptr) {
        return;
    }
    DnsResult result {};
    if (xQueueReceive(_dns_result_queue, &result, 0) != pdTRUE
        || result.generation != _dns_generation
        || !_connect_requested || !wifi_connected()) {
        return;
    }
    if (!result.success) {
        set_error("FluidNC host lookup failed");
        _next_retry_ms = millis() + 5000;
        Serial.println("net: FluidNC hostname lookup failed; retry scheduled");
        return;
    }
    IPAddress address(result.address);
    strlcpy(_resolved_address, address.toString().c_str(),
            sizeof(_resolved_address));
    Serial.printf("net: FluidNC host resolved to %s\n", _resolved_address);
    begin_tcp_connect(_resolved_address);
}

bool JcReadOnlyFluidNc::queue_status_frame(const char* frame, uint16_t length) {
    ValidatedStatus validated {};
    if (!validate_status_frame(frame, length, validated)) {
        record_invalid_frame("Malformed or non-four-axis status frame");
        return false;
    }

    begin_status_report();
    for (uint16_t i = 0; i < length; ++i) {
        collect(static_cast<uint8_t>(frame[i]));
    }
    collect(static_cast<uint8_t>('\n'));
    return !_report_invalid && _report_has_state && _report_has_axes;
}

void JcReadOnlyFluidNc::consume_tcp_byte(uint8_t byte) {
    if (!_frame_active) {
        if (byte == '<') {
            _frame_active = true;
            _frame_invalid = false;
            _status_frame_length = 0;
            _status_frame[_status_frame_length++] = '<';
        }
        return;
    }
    if (byte == '<') {
        record_invalid_frame("Nested status frame start");
        _status_frame_length = 0;
        _status_frame[_status_frame_length++] = '<';
        return;
    }
    if (byte < 0x20 || byte > 0x7E
        || _status_frame_length >= sizeof(_status_frame) - 1) {
        record_invalid_frame("Status frame exceeded bounds or contained control data");
        _frame_active = false;
        _status_frame_length = 0;
        return;
    }

    _status_frame[_status_frame_length++] = static_cast<char>(byte);
    if (byte == '>') {
        _frame_active = false;
        if (!queue_status_frame(_status_frame, _status_frame_length)) {
            _frame_invalid = true;
        }
        _status_frame_length = 0;
    }
}

void JcReadOnlyFluidNc::set_error(const char* message, bool parse_error) {
    if (message != nullptr) {
        strlcpy(_last_error, message, sizeof(_last_error));
    }
    _state = parse_error ? JcFluidState::InvalidData : JcFluidState::Error;
    if (message != nullptr) {
        Serial.printf("net: %s\n", message);
    }
}

} // namespace jc8048w550

extern "C" {

int fnc_getchar() {
    return -1;
}

void fnc_putchar(uint8_t byte) {
    if (jc8048w550::g_active_link != nullptr) {
        jc8048w550::g_active_link->parser_putchar(byte);
    }
}

int milliseconds() {
    return static_cast<int>(millis());
}

void begin_status_report() {
    if (jc8048w550::g_active_link != nullptr) {
        jc8048w550::g_active_link->begin_status_report();
    }
}

void show_state(const char* state) {
    if (jc8048w550::g_active_link != nullptr) {
        jc8048w550::g_active_link->accept_machine_state(state);
    }
}

void show_dro(const pos_t* axes, const pos_t*, bool is_machine_position,
              bool*, size_t axis_count) {
    if (jc8048w550::g_active_link == nullptr) {
        return;
    }
    if (axis_count != 4) {
        jc8048w550::g_active_link->record_invalid_frame(
            "FluidNC status did not report exactly four axes");
        return;
    }
    float values[4];
    for (size_t i = 0; i < 4; ++i) {
        values[i] = static_cast<float>(axes[i]);
    }
    jc8048w550::g_active_link->accept_axis_positions(values,
                                                      is_machine_position);
}

void end_status_report() {
    if (jc8048w550::g_active_link != nullptr) {
        jc8048w550::g_active_link->end_status_report();
    }
}

void show_feed_spindle(uint32_t, uint32_t) {}

} // extern "C"
