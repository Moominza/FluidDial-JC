// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "JcFrameAssembler.hpp"
#include "JcQuery.hpp"
#include "JcStreamSplitter.hpp"
#include "JcLinkTypes.hpp"
#include "JcStatusFrame.hpp"

namespace jc8048w550 {

struct JcWifiConfig {
    char ssid[33];
    char password[64];
    char host[64];
    uint8_t bssid[6];
    uint8_t channel;
    bool bssid_pinned;
    bool valid;
};

struct JcWifiNetwork {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
};

constexpr uint8_t kJcMaxScannedNetworks = 20;

class JcReadOnlyFluidNc {
public:
    using LineListener = void (*)(void* user, const char* line);

    JcReadOnlyFluidNc();

    // Text lines that are not status frames (replies such as "ok", "error:3",
    // "$/axes/x/steps_per_mm=80.000") are passed to the listener.
    void set_line_listener(LineListener listener, void* user) {
        _line_listener = listener;
        _line_user = user;
    }
    // Sends ONE allowlisted read-only query (see JcQuery.hpp). Anything that is
    // not on the allowlist is refused and counted, never transmitted.
    bool send_query_line(const char* line);
    bool tcp_connected() const { return _socket >= 0; }
    uint32_t rejected_query_lines() const { return _rejected_queries; }
    uint32_t queries_sent() const { return _queries_sent; }

    void begin();
    void poll();

    const JcWifiConfig& config() const;
    bool save_config(const JcWifiConfig& config);
    bool start_wifi_scan();
    bool wifi_scan_running() const;
    uint8_t scanned_network_count() const;
    const JcWifiNetwork* scanned_network(uint8_t index) const;
    void connect();
    void disconnect();

    bool wifi_connected() const;
    bool connect_requested() const;
    JcFluidState state() const;
    const JcLiveMachineStatus& status() const;
    bool status_is_fresh() const;
    uint32_t reconnect_attempts() const;
    uint32_t parse_errors() const;
    uint32_t blocked_transmissions() const;
    uint32_t last_status_age_ms() const;
    const char* last_error() const;
    static const char* state_name(JcFluidState state);
    static const char* reference_name(JcCoordinateReference reference);

    void begin_status_report();
    void accept_machine_state(const char* state);
    void accept_axis_positions(const float axes[4], bool machine_position);
    void end_status_report();
    void record_invalid_frame(const char* reason);
    void parser_putchar(uint8_t byte);

private:
    struct DnsRequest;
    struct DnsResult;

    static void dns_worker(void* context);
    void start_dns_worker();
    void resolve_controller();
    void begin_tcp_connect(const char* address);
    void poll_tcp_connect();
    void poll_tcp_io();
    void close_tcp(bool schedule_retry);
    void service_wifi();
    void service_wifi_scan();
    void service_reconnect();
    void service_status_poll();
    bool queue_status_frame(const char* frame, uint16_t length);
    void consume_tcp_byte(uint8_t byte);
    void handle_dns_result();
    void set_error(const char* message, bool parse_error = false);

    JcWifiConfig _config {};
    JcWifiNetwork _scanned_networks[kJcMaxScannedNetworks] {};
    uint8_t _scanned_network_count = 0;
    JcLiveMachineStatus _status {};
    JcFluidState _state = JcFluidState::Disconnected;
    char _last_error[64] {};
    char _resolved_address[16] {};
    JcStreamSplitter _assembler;
    LineListener _line_listener = nullptr;
    void* _line_user = nullptr;
    uint32_t _rejected_queries = 0;
    uint32_t _queries_sent = 0;
    JcStatusFrame _pending_frame {};
    int _socket = -1;
    int _connecting_socket = -1;
    uint32_t _connect_deadline_ms = 0;
    uint32_t _next_retry_ms = 0;
    uint32_t _next_status_poll_ms = 0;
    uint32_t _last_status_attempt_ms = 0;
    uint32_t _reconnect_attempts = 0;
    uint32_t _parse_errors = 0;
    uint32_t _blocked_transmissions = 0;
    uint32_t _dns_generation = 0;
    bool _connect_requested = false;
    bool _wifi_scan_running = false;
    bool _wifi_was_connected = false;
    bool _frame_invalid = false;
    bool _report_has_state = false;
    bool _report_has_axes = false;
    bool _report_invalid = false;
    float _report_axes[4] {};
    char _report_state[20] {};
    JcCoordinateReference _report_reference = JcCoordinateReference::Unknown;
    QueueHandle_t _dns_request_queue = nullptr;
    QueueHandle_t _dns_result_queue = nullptr;
    TaskHandle_t _dns_task = nullptr;
};

} // namespace jc8048w550
