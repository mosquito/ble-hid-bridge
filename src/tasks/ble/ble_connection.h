#pragma once

#include "ble_types.h"
#include "config.h"
#include "events.h"

// Diagnostic counter: mouse no-change reports skipped
uint32_t bleConnectionMouseSkips();
#include "key_remap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * BLE Connection State Machine
 *
 * Manages a single BLE HID device connection through its lifecycle:
 * IDLE -> CONNECTING -> DISCOVERING_SVC -> READING_REPORT_MAP ->
 * DISCOVERING_CHARS -> REGISTERING_NOTIFY -> READY
 *
 * All state transitions happen in response to GATTC events.
 * Each connection has its own independent state machine.
 *
 * HID reports are sent directly to USB via UsbHid API (no queues).
 */
class BleConnection {
public:
    static constexpr int MAX_REPORTS = 8;

    BleConnection() = default;

    /**
     * Reset connection to IDLE state
     */
    void reset();

    /**
     * Start connecting to a device
     * @param gattcIf GATTC interface
     * @param bda Device Bluetooth address
     * @param addrType Address type (public/random)
     * @param address String representation of address
     * @param name Device name
     * @param secLevel Security level for this connection
     */
    void startConnect(esp_gatt_if_t gattcIf, const esp_bd_addr_t bda,
                      uint8_t addrType, const char* address, const char* name,
                      SecurityLevel secLevel = SecurityLevel::MEDIUM);

    /**
     * Handle GATTC event for this connection
     * @param evt Event data
     * @param eventQueue Queue to send BLE events (optional)
     * @return true if event was handled
     */
    bool handleGattcEvent(const GattcEventData& evt,
                          QueueHandle_t eventQueue);

    /**
     * Process HID NOTIFY directly (called from BTC callback context)
     * Hot path - minimal processing, sends directly to USB HID
     * @param handle Characteristic handle
     * @param data Notify data
     * @param len Data length
     */
    void processNotify(uint16_t handle, const uint8_t* data, uint16_t len);

    // State queries
    bool isActive() const { return state_ != ConnState::IDLE; }
    bool isReady() const { return state_ == ConnState::READY; }
    ConnState state() const { return state_; }
    uint16_t connId() const { return connId_; }
    const esp_bd_addr_t& bda() const { return bda_; }
    const char* address() const { return address_; }
    const char* name() const { return name_; }
    uint8_t addrType() const { return addrType_; }

    void setRemapTable(const DeviceRemapTable* table) { remapTable_ = table; }

private:
    // Connection state
    ConnState state_ = ConnState::IDLE;
    uint16_t connId_ = 0;
    esp_gatt_if_t gattcIf_ = ESP_GATT_IF_NONE;
    esp_bd_addr_t bda_ = {};
    uint8_t addrType_ = 0;
    char address_[BLE_ADDR_STR_LEN] = {};
    char name_[BLE_MAX_DEVICE_NAME_LEN] = {};

    // HID service info
    uint16_t svcStart_ = 0;
    uint16_t svcEnd_ = 0;

    // Reports
    ReportInfo reports_[MAX_REPORTS] = {};
    int reportCount_ = 0;
    int nextNotifyIdx_ = 0;

    // Parsed HID descriptor
    HidParser::ParsedDevice parsedHid_ = {};
    uint8_t lastButtons_ = 0;

    // Async operation tracking
    bool reportMapRequested_ = false;
    int pendingDescrReads_ = 0;

    // Security level for this connection
    SecurityLevel secLevel_ = SecurityLevel::MEDIUM;

    // Key remap table (owned by KeyRemapManager, read-only here)
    const DeviceRemapTable* remapTable_ = nullptr;

    // State transitions
    void transitionTo(ConnState newState);

    // Event handlers (called from handleGattcEvent)
    void onOpen(const GattcEventData& evt, QueueHandle_t eventQueue);
    void onClose(const GattcEventData& evt, QueueHandle_t eventQueue);
    void onSearchRes(const GattcEventData& evt);
    void onSearchCmpl(const GattcEventData& evt, QueueHandle_t eventQueue);
    void onReadChar(const GattcEventData& evt);
    void onReadDescr(const GattcEventData& evt);
    void onNotify(const GattcEventData& evt);

    // State machine actions
    void doReadReportMap();
    void doDiscoverChars();
    void doRegisterNextNotify();

    // Helpers
    int addReport(uint16_t charHandle, uint8_t reportId, uint8_t reportType, bool hasNotify);
    ReportInfo* findReportByHandle(uint16_t charHandle);
    uint8_t getReportId(uint16_t charHandle);

    // Send event to web task
    void sendEvent(QueueHandle_t queue, BleEvent event,
                   const char* addr = "", const char* devName = "");
};
