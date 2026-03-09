#pragma once

#include "task_base.h"
#include "storage.h"
#include "events.h"
#include "ble/ble_types.h"
#include "ble/ble_connection.h"
#include "ble/key_remap.h"
#include "../config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

/**
 * Found HID device from passive scan
 */
struct FoundDevice {
    char address[18];
    char name[32];
    uint8_t addrType;
    int8_t rssi;
    int64_t lastSeen;  // esp_timer_get_time() microseconds
};

/**
 * Lightweight NOTIFY event for the hot path queue.
 * BTC callback enqueues these; run() drains and processes on Core 1.
 */
struct NotifyEvent {
    uint16_t connId;
    uint16_t handle;
    uint16_t len;
    uint8_t data[BLE_MAX_NOTIFY_DATA_SIZE];
};

/**
 * Pipeline diagnostic counters (BLE NOTIFY -> HidBridge -> USB HID)
 */
struct HidStats {
    uint32_t notifyIn;      // BLE NOTIFY events enqueued
    uint32_t notifyDrop;    // Queue full drops
    uint32_t notifyOut;     // Processed by HidBridge
    uint32_t mouseSkip;     // Mouse no-change reports skipped
    uint32_t usbOk;        // USB reports sent
    uint32_t usbBusy;      // USB endpoint busy (retried next tick)
    uint32_t usbNotReady;  // USB not attached
    uint32_t usbQueueFull; // Send buffer full, report dropped
    // Frame timing (cumulative for delta computation)
    int64_t  btFrameSumUs;  // Sum of BT inter-notify gaps
    uint32_t btFrameCount;  // Number of BT frame intervals measured
    int64_t  usbFrameSumUs; // Sum of USB inter-send gaps
    uint32_t usbFrameCount; // Number of USB frame intervals measured
    uint32_t usbFrameMinUs; // USB frame min (reset-on-read)
    uint32_t usbFrameMaxUs; // USB frame max (reset-on-read)
};

enum class PairState : uint8_t {
    IDLE,
    SCANNING,
    CONNECTING
};

/**
 * HID Bridge Task
 *
 * Unified task that combines BLE HID host and USB HID output.
 * Runs on CPU1 — dedicated core, nothing else scheduled here.
 *
 * Data flow (NOTIFY - hot path, single-core ring buffer):
 *   BTC callback -> notifyPush() + xTaskNotifyGive() -> run() -> onNotifyDirect()
 *
 * Data flow (other events):
 *   BTC callback -> internal_queue_ -> run() -> handle*Event()
 *
 * Features:
 * - Manual scan via POST /api/scan (no auto-scan)
 * - Auto-connect to bonded devices when discovered
 * - Manual connect via Web UI for initial pairing
 *
 * Queues:
 * - cmd_queue_: Commands from Web/Button tasks (BleCmdMsg)
 * - event_queue_: BLE events to Web task (BleEventMsg)
 */
class HidBridge : public TaskBase {
public:
    static constexpr int MAX_CONNECTIONS = 4;
    static constexpr size_t NOTIFY_BUF_SIZE = 32;  // Ring buffer for NOTIFY hot path

    /**
     * @param storage Storage for saved devices
     * @param eventQueue Queue to send BLE events (BleEventMsg) - optional
     */
    HidBridge(Storage* storage, QueueHandle_t eventQueue = nullptr);

    void init() override;
    void run() override;

    /**
     * Get command queue - Web/Button tasks send BleCmdMsg here
     */
    QueueHandle_t getCmdQueue() const { return cmd_queue_; }

    /**
     * Start BLE scan manually
     */
    void startScan();

    /**
     * Check if passive scan is running
     */
    bool isScanning() const { return scanning_; }

    /**
     * Check if a connection attempt is in progress (for LED feedback)
     */
    bool isConnecting() const { return pending_connect_.active; }

    /**
     * Get headless pairing state (for LED feedback)
     */
    PairState getPairState() const { return pair_state_; }

    /**
     * Get number of active connections
     */
    int connectionCount() const;

    /**
     * Get found devices list (thread-safe copy)
     * @param out Output array
     * @param maxCount Maximum devices to copy
     * @return Number of devices copied
     */
    size_t getFoundDevices(FoundDevice* out, size_t maxCount);

    /**
     * Get pipeline diagnostic counters (cumulative)
     */
    HidStats getStats() const;

private:
    // Queue sizes
    static constexpr size_t CMD_QUEUE_SIZE = 8;
    static constexpr size_t INTERNAL_QUEUE_SIZE = 8;  // For non-NOTIFY events

    // Storage for saved devices
    Storage* storage_;

    // Key remap manager (initialized in constructor with storage_)
    KeyRemapManager remap_mgr_;

    // External queue (provided)
    QueueHandle_t event_queue_;

    // Found devices from passive scan (for Web UI)
    FoundDevice found_devices_[BLE_MAX_FOUND_DEVICES];
    size_t found_devices_count_ = 0;
    SemaphoreHandle_t found_mutex_ = nullptr;
    StaticSemaphore_t found_mutex_buffer_;

    // Command queue (static allocation)
    StaticQueue_t cmd_queue_buffer_;
    uint8_t cmd_queue_storage_[CMD_QUEUE_SIZE * sizeof(BleCmdMsg)];
    QueueHandle_t cmd_queue_ = nullptr;

    // Internal event queue for non-NOTIFY events (static allocation)
    StaticQueue_t internal_queue_buffer_;
    uint8_t internal_queue_storage_[INTERNAL_QUEUE_SIZE * sizeof(BleInternalEvent)];
    QueueHandle_t internal_queue_ = nullptr;

    // Task handle for xTaskNotifyGive() wakeup from BTC callback
    TaskHandle_t task_handle_ = nullptr;

    // Connections (each manages its own state machine)
    BleConnection conns_[MAX_CONNECTIONS];

    // Scan state
    bool scanning_ = false;
    int64_t next_scan_time_ = 0;      // When to start next scan (esp_timer_get_time)
    int64_t next_reconnect_ = 0;      // When to trigger next auto-reconnect scan

    // Headless pairing state (button-triggered SCAN_PAIR)
    PairState pair_state_ = PairState::IDLE;
    int64_t pair_start_us_ = 0;

    // Pending connection for security retry
    struct PendingConnect {
        char address[18];
        char name[32];
        uint8_t addrType;
        SecurityLevel secLevel;
        int retryCount;
        bool active;
        int64_t startTime;
    };
    PendingConnect pending_connect_ = {};

    // Pending connection parameter update retry (stepped negotiation)
    struct ConnParamsRetry {
        esp_bd_addr_t bda;
        int64_t retryAt;   // esp_timer_get_time() target, 0 = inactive
        int step;           // 0=idle, 1=tight, 2=medium, 3+=done
    };
    ConnParamsRetry conn_params_retry_ = {};

    // GATTC interface
    esp_gatt_if_t gattc_if_ = ESP_GATT_IF_NONE;
    bool initialized_ = false;

    // Singleton for callbacks
    static HidBridge* instance_;

    // Event processing (called from run())
    void processNotifyEvents();
    void processInternalEvents();
    void processCommands();

    // Direct NOTIFY handling from callback (hot path)
    void onNotifyDirect(uint16_t connId, uint16_t handle,
                        const uint8_t* data, uint16_t len);

    // GAP event handlers
    void handleGapEvent(const GapEventData& evt);
    void onScanParamSet(const GapEventData& evt);
    void onScanResult(const GapEventData& evt);
    void onScanStop();
    void onSecurityEvent(const GapEventData& evt);
    void onConnParamsUpdate(const GapEventData& evt);

    // GATTC event handlers
    void handleGattcEvent(const GattcEventData& evt);
    void onGattcReg(const GattcEventData& evt);

    // Command handlers
    void handleConnect(const char* address, const char* name, uint8_t addrType = 0xFF);
    void handleDisconnect(const char* address, int8_t deviceId);

    // Connection ready callback
    void onConnectionReady(int slot);

    // Security level management
    void applySecurityLevel(SecurityLevel level);
    SecurityLevel getDeviceSecurityLevel(const char* address);
    void saveDeviceSecurityLevel(const char* address, SecurityLevel level);
    void retryConnectWithLowerSecurity();

    // BLE initialization
    void initBle();
    void doStartScan();

    // Helpers
    static void bdaToStr(const esp_bd_addr_t bda, char* str);
    static void strToBda(const char* str, esp_bd_addr_t bda);
    int findFreeSlot();
    int findSlotByAddr(const char* addr);
    int findSlotByConnId(uint16_t connId);
    int findSlotByBda(const esp_bd_addr_t bda);

    // Bonded device check (uses Bluedroid bond list)
    bool isDeviceBonded(const esp_bd_addr_t bda, uint8_t* outAddrType = nullptr);

    // Saved device check (uses NVS saved_devs from WebServer)
    bool isDeviceSaved(const char* address);

    // Auto-reconnect: connect to the first disconnected saved device
    void reconnectNext();

    // Found devices management
    void updateFoundDevice(const char* address, const char* name, uint8_t addrType, int8_t rssi);
    void cleanupFoundDevices();
    int findFoundDevice(const esp_bd_addr_t bda);
    uint8_t getFoundDeviceAddrType(const esp_bd_addr_t bda);

    // Send event to web task
    void sendEvent(BleEvent event, const char* address = "",
                   const char* name = "", int8_t rssi = 0);

    // Static callbacks (enqueue events only)
    static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    static void gattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattcIf,
                              esp_ble_gattc_cb_param_t* param);
};
