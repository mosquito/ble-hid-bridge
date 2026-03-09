#pragma once

#include "task_base.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_netif.h"

/**
 * WiFi Access Point Task
 *
 * Manages WiFi AP mode for device configuration.
 * Commands received via queue - safe for inter-task use.
 */

enum class WifiCmdType : uint8_t {
    START_AP,
    STOP_AP
};

struct WifiCmd {
    WifiCmdType type;
    char ssid[33];
    char password[65];
};

class Storage;

class Wifi : public TaskBase {
public:
    explicit Wifi(Storage* storage);

    void init() override;
    void run() override;

    // Queue-based commands (thread-safe)
    void startAP();  // Uses default SSID (chip-id based) and password
    void startAP(const char* ssid, const char* password);
    void stopAP();

    bool isAPActive() const { return ap_active_; }
    const char* getIP() const { return "192.168.4.1"; }
    const char* getSSID() const { return ssid_; }

    QueueHandle_t getQueue() const { return queue_; }

private:
    static constexpr size_t QUEUE_SIZE = 4;

    Storage* storage_;
    bool ap_active_ = false;
    esp_netif_t* netif_ = nullptr;
    char ssid_[33] = {0};

    // Static queue allocation
    StaticQueue_t queue_buffer_;
    uint8_t queue_storage_[QUEUE_SIZE * sizeof(WifiCmd)];
    QueueHandle_t queue_ = nullptr;

    void processCommands();
    void doStartAP(const char* ssid, const char* password);
    void doStopAP();
};
