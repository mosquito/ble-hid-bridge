#pragma once

#include "task_base.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include <stdint.h>

/**
 * Web Server Task
 *
 * HTTP server for device configuration.
 * Requires WiFi AP to be started first.
 */

// Events received from other tasks (e.g., BLE events)
enum class WebEventType : uint8_t {
    DEVICE_FOUND,
    CONNECTED,
    DISCONNECTED
};

struct WebEvent {
    WebEventType type;
    char address[18];
    char name[32];
    int8_t rssi;
};

// Connected device tracking
struct ConnectedDevice {
    bool active;
    char address[18];
    char name[32];
};

class Storage;
class HidBridge;
struct FoundDevice;

class WebServer : public TaskBase {
public:
    explicit WebServer(Storage* storage);

    void init() override;
    void run() override;

    void start();
    void stop();

    bool isRunning() const { return server_ != nullptr; }
    QueueHandle_t getEventQueue() const { return event_queue_; }

    // Set BLE command queue (for sending scan/connect commands)
    void setBleCmdQueue(QueueHandle_t queue) { ble_cmd_queue_ = queue; }

    // Set HID bridge (for getting found devices and scan control)
    void setHidBridge(HidBridge* bridge) { hid_bridge_ = bridge; }

private:
    static constexpr size_t EVENT_QUEUE_SIZE = 16;

    Storage* storage_;
    httpd_handle_t server_ = nullptr;

    // Event queue
    StaticQueue_t event_queue_buffer_;
    uint8_t event_queue_storage_[EVENT_QUEUE_SIZE * sizeof(WebEvent)];
    QueueHandle_t event_queue_ = nullptr;

    // HID bridge (for getting found devices and scan control)
    HidBridge* hid_bridge_ = nullptr;

    // Connected devices tracking
    static constexpr size_t MAX_CONNECTED_DEVICES = 4;
    ConnectedDevice connected_devices_[MAX_CONNECTED_DEVICES];
    size_t connected_count_ = 0;

    // BLE command queue (for sending scan/connect commands)
    QueueHandle_t ble_cmd_queue_ = nullptr;

    void processEvents();
    void registerHandlers();
    void saveDevice(const char* address, const char* name);

    // HTTP handlers (static, get WebServer* from req->user_ctx)
    static esp_err_t handleRoot(httpd_req_t* req);
    static esp_err_t handleStatus(httpd_req_t* req);
    static esp_err_t handleScanGet(httpd_req_t* req);
    static esp_err_t handleScanPost(httpd_req_t* req);
    static esp_err_t handleConnect(httpd_req_t* req);
    static esp_err_t handleDisconnect(httpd_req_t* req);
    static esp_err_t handleBondedGet(httpd_req_t* req);
    static esp_err_t handleBondedDelete(httpd_req_t* req);
    static esp_err_t handleSaved(httpd_req_t* req);
    static esp_err_t handleRemove(httpd_req_t* req);
    static esp_err_t handleRestart(httpd_req_t* req);
    static esp_err_t handleWifiGet(httpd_req_t* req);
    static esp_err_t handleWifiPost(httpd_req_t* req);
    static esp_err_t handleFactoryReset(httpd_req_t* req);
    static esp_err_t handleRemapsGet(httpd_req_t* req);
    static esp_err_t handleRemapsPost(httpd_req_t* req);
};
