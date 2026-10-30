// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcBleLink.hpp"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

namespace jc8048w550 {

namespace {

constexpr size_t kScanQueue = 12;
constexpr size_t kRxStream = 1024;

struct ScanItem {
    char addr[20];
    char name[28];
    int16_t rssi;
    bool hint;
};

// State shared with the BLE stack's tasks. Everything the callbacks touch is either volatile and written by
// one side only, or guarded by g_mux.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
BLEClient* g_client = nullptr;
BLERemoteCharacteristic* g_notify_chr = nullptr;
BLERemoteCharacteristic* g_write_chr = nullptr;
StreamBufferHandle_t g_rx = nullptr;
volatile uint32_t g_rx_overflow = 0;
volatile uint8_t g_gen = 0;                 // incremented by connect()/disconnect(): stale task results are discarded
volatile uint8_t g_conn_event = 0;          // 0 none, 1 connected, 2 failed
volatile uint8_t g_conn_gen = 0;
volatile bool g_disc_ready = false;
volatile bool g_disconnected = false;
volatile bool g_task_busy = false;
volatile bool g_scan_running = false;
JcBleDiscovery g_disc;
ScanItem g_scan[kScanQueue];
volatile size_t g_scan_n = 0;
char g_target[20] = "";
uint16_t g_mtu = 23;

uint8_t props_of(BLERemoteCharacteristic* c) {
    uint8_t p = 0;
    if (c->canRead()) p |= kBleRead;
    if (c->canWriteNoResponse()) p |= kBleWriteNoRsp;
    if (c->canWrite()) p |= kBleWrite;
    if (c->canNotify()) p |= kBleNotify;
    if (c->canIndicate()) p |= kBleIndicate;
    return p;
}

void copy_uuid(char* dst, size_t cap, const std::string& s) {
    strncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

class ClientCb : public BLEClientCallbacks {
    void onConnect(BLEClient*) override {}
    void onDisconnect(BLEClient*) override { g_disconnected = true; }
};
ClientCb g_client_cb;

class ScanCb : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        ScanItem it {};
        std::string a = dev.getAddress().toString();
        strncpy(it.addr, a.c_str(), sizeof(it.addr) - 1);
        std::string n = dev.haveName() ? dev.getName() : std::string();
        strncpy(it.name, n.c_str(), sizeof(it.name) - 1);
        it.rssi = static_cast<int16_t>(dev.getRSSI());
        it.hint = dev.haveServiceUUID() && (dev.isAdvertisingService(BLEUUID(kJcHm10Service)) || dev.isAdvertisingService(BLEUUID(kJcNusService)));
        portENTER_CRITICAL(&g_mux);
        if (g_scan_n < kScanQueue) g_scan[g_scan_n++] = it;
        portEXIT_CRITICAL(&g_mux);
    }
};
ScanCb g_scan_cb;

void notify_cb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (g_rx == nullptr) return;
    const size_t sent = xStreamBufferSend(g_rx, data, len, 0);
    if (sent < len) g_rx_overflow += static_cast<uint32_t>(len - sent);
}

void scan_done(BLEScanResults) { g_scan_running = false; }

// Connects and discovers the GATT table off the main loop (both can block for seconds).
void connect_task(void* arg) {
    const uint8_t gen = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg));
    JcBleDiscovery d;
    memset(&d, 0, sizeof(d));
    bool ok = false;
    if (g_client == nullptr) {
        g_client = BLEDevice::createClient();
        g_client->setClientCallbacks(&g_client_cb);
    }
    if (g_client->connect(BLEAddress(std::string(g_target)))) {
        g_mtu = g_client->getMTU();
        std::map<std::string, BLERemoteService*>* services = g_client->getServices();
        if (services != nullptr) {
            for (auto& kv : *services) {
                if (d.n >= 6) break;
                JcBleService& s = d.services[d.n++];
                copy_uuid(s.uuid, sizeof(s.uuid), kv.first);
                s.nchars = 0;
                std::map<std::string, BLERemoteCharacteristic*>* chars = kv.second->getCharacteristics();
                if (chars == nullptr) continue;
                for (auto& cv : *chars) {
                    if (s.nchars >= 6) break;
                    copy_uuid(s.chars[s.nchars].uuid, sizeof(s.chars[s.nchars].uuid), cv.first);
                    s.chars[s.nchars].props = props_of(cv.second);
                    ++s.nchars;
                }
            }
        }
        ok = true;
    }
    portENTER_CRITICAL(&g_mux);
    if (gen == g_gen) {
        g_disc = d;
        g_disc_ready = ok;
        g_conn_gen = gen;
        g_conn_event = ok ? 1 : 2;
    }
    portEXIT_CRITICAL(&g_mux);
    if (gen != g_gen && ok && g_client != nullptr) g_client->disconnect();   // the request was withdrawn meanwhile
    g_task_busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

bool JcBleRadioEsp::ensure_stack() {
    if (_started) return true;
    BLEDevice::init("JC-Pendant");
    BLEDevice::setMTU(185);
    if (g_rx == nullptr) g_rx = xStreamBufferCreate(kRxStream, 1);
    _started = g_rx != nullptr;
    if (_started) Serial.println("ble: stack started (central)");
    return _started;
}

bool JcBleRadioEsp::scan_start(uint32_t duration_ms) {
    if (!ensure_stack()) return false;
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&g_scan_cb, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    portENTER_CRITICAL(&g_mux);
    g_scan_n = 0;
    portEXIT_CRITICAL(&g_mux);
    g_scan_running = true;
    const bool ok = scan->start((duration_ms + 999) / 1000, scan_done, false);
    if (!ok) g_scan_running = false;
    return ok;
}

void JcBleRadioEsp::scan_stop() {
    if (_started && g_scan_running) {
        BLEDevice::getScan()->stop();
        g_scan_running = false;
    }
}

bool JcBleRadioEsp::connect(const char* addr) {
    if (!ensure_stack() || g_task_busy || addr == nullptr) return false;
    scan_stop();
    strncpy(g_target, addr, sizeof(g_target) - 1);
    g_target[sizeof(g_target) - 1] = '\0';
    ++g_gen;
    g_conn_event = 0;
    g_disc_ready = false;
    g_disconnected = false;
    g_task_busy = true;
    if (xTaskCreate(connect_task, "jcble", 6144, reinterpret_cast<void*>(static_cast<uintptr_t>(g_gen)), 1, nullptr) != pdPASS) {
        g_task_busy = false;
        return false;
    }
    return true;
}

void JcBleRadioEsp::disconnect() {
    ++g_gen;                                  // any connect still in flight is withdrawn
    g_conn_event = 0;
    g_disc_ready = false;
    g_notify_chr = g_write_chr = nullptr;
    if (_started && g_client != nullptr && g_client->isConnected()) g_client->disconnect();
    if (g_rx != nullptr) xStreamBufferReset(g_rx);
}

bool JcBleRadioEsp::discover() {
    return true;   // the table was read by the connect task; poll() delivers it
}

bool JcBleRadioEsp::subscribe(const JcBleSelection& sel) {
    if (g_client == nullptr || !g_client->isConnected() || sel.svc < 0 || sel.svc >= g_disc.n) {
        if (_link != nullptr) _link->on_subscribed(false, millis());
        return true;
    }
    const JcBleService& s = g_disc.services[sel.svc];
    BLERemoteService* svc = g_client->getService(BLEUUID(s.uuid));
    bool ok = false;
    if (svc != nullptr) {
        g_write_chr = svc->getCharacteristic(BLEUUID(s.chars[sel.write_chr].uuid));
        g_notify_chr = svc->getCharacteristic(BLEUUID(s.chars[sel.notify_chr].uuid));
        if (g_write_chr != nullptr && g_notify_chr != nullptr && g_notify_chr->canNotify()) {
            g_notify_chr->registerForNotify(notify_cb);
            ok = true;
        }
    }
    if (_link != nullptr) _link->on_subscribed(ok, millis());
    return true;
}

bool JcBleRadioEsp::write(const JcBleSelection& sel, const uint8_t* data, size_t len) {
    if (g_write_chr == nullptr || g_client == nullptr || !g_client->isConnected() || data == nullptr || len == 0) return false;
    g_write_chr->writeValue(const_cast<uint8_t*>(data), len, sel.write_with_response);
    return true;
}

uint16_t JcBleRadioEsp::mtu() const { return g_mtu; }

void JcBleRadioEsp::poll(uint32_t now) {
    if (!_started || _link == nullptr) return;
    // scan results
    ScanItem items[kScanQueue];
    size_t n = 0;
    portENTER_CRITICAL(&g_mux);
    n = g_scan_n;
    memcpy(items, g_scan, n * sizeof(ScanItem));
    g_scan_n = 0;
    portEXIT_CRITICAL(&g_mux);
    for (size_t i = 0; i < n; ++i) _link->on_scan_result(items[i].addr, items[i].name, items[i].rssi, items[i].hint, now);
    // connection result and the GATT table
    uint8_t ev = 0;
    uint8_t gen = 0;
    portENTER_CRITICAL(&g_mux);
    ev = g_conn_event;
    gen = g_conn_gen;
    g_conn_event = 0;
    portEXIT_CRITICAL(&g_mux);
    if (ev != 0 && gen == g_gen) {
        _link->on_connected(ev == 1, now);
        if (ev == 1 && g_disc_ready) {
            g_disc_ready = false;
            _link->on_discovery(g_disc, now);
        }
    }
    // notifications (bounded stream buffer), then loss of the link
    if (g_rx != nullptr) {
        uint8_t buf[200];
        size_t got;
        int guard = 8;
        while (guard-- > 0 && (got = xStreamBufferReceive(g_rx, buf, sizeof(buf), 0)) > 0) _link->on_notify(buf, got, now);
        const uint32_t lost = g_rx_overflow;
        if (lost != 0) {
            g_rx_overflow = 0;
            _link->on_rx_overflow(lost);
        }
    }
    if (g_disconnected) {
        g_disconnected = false;
        _link->on_disconnected(now);
    }
}

void JcBleRadioEsp::shutdown() {
    if (!_started) return;
    scan_stop();
    disconnect();
    if (g_rx != nullptr) { vStreamBufferDelete(g_rx); g_rx = nullptr; }
    g_client = nullptr;
    BLEDevice::deinit(true);
    _started = false;
    Serial.println("ble: stack released");
}

}  // namespace jc8048w550
