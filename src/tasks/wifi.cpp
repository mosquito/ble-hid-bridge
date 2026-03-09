#include "wifi.h"
#include "config.h"
#include "storage.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <string>

static const char* TAG = "wifi";

Wifi::Wifi(Storage* storage)
    : storage_(storage)
{
}

void Wifi::init()
{
    queue_ = xQueueCreateStatic(
        QUEUE_SIZE,
        sizeof(WifiCmd),
        queue_storage_,
        &queue_buffer_
    );

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    LOG(TAG, "Task started on core %d", xPortGetCoreID());
}

void Wifi::run()
{
    processCommands();
    vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
}

void Wifi::startAP()
{
    // Get SSID from storage, or generate and save default
    std::string ssid = storage_->get("wifi_ssid");

    if (ssid.empty()) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char default_ssid[33];
        snprintf(default_ssid, sizeof(default_ssid), "ESP32-%02X%02X", mac[4], mac[5]);
        ssid = default_ssid;
        storage_->set("wifi_ssid", ssid);
        LOG(TAG, "Generated default SSID: %s", ssid.c_str());
    }

    // Get password from storage, or use default
    std::string password = storage_->get("wifi_pass", WIFI_AP_PASSWORD);

    startAP(ssid.c_str(), password.c_str());
}

void Wifi::startAP(const char* ssid, const char* password)
{
    if (queue_ == nullptr) {
        return;
    }
    WifiCmd cmd = {};
    cmd.type = WifiCmdType::START_AP;
    strncpy(cmd.ssid, ssid, sizeof(cmd.ssid) - 1);
    strncpy(cmd.password, password, sizeof(cmd.password) - 1);
    xQueueSend(queue_, &cmd, 0);
}

void Wifi::stopAP()
{
    if (queue_ == nullptr) {
        return;
    }
    WifiCmd cmd = {};
    cmd.type = WifiCmdType::STOP_AP;
    xQueueSend(queue_, &cmd, 0);
}

void Wifi::processCommands()
{
    WifiCmd cmd;
    while (xQueueReceive(queue_, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
            case WifiCmdType::START_AP:
                doStartAP(cmd.ssid, cmd.password);
                break;
            case WifiCmdType::STOP_AP:
                doStopAP();
                break;
        }
    }
}

void Wifi::doStartAP(const char* ssid, const char* password)
{
    if (ap_active_) {
        WARN(TAG, "AP already active");
        return;
    }

    netif_ = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char*)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(netif_);
    esp_netif_set_ip_info(netif_, &ip_info);
    esp_netif_dhcps_start(netif_);

    ap_active_ = true;
    strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    ssid_[sizeof(ssid_) - 1] = '\0';

    LOG(TAG, "AP started: %s", ssid_);
    LOG(TAG, "IP: %s", getIP());
}

void Wifi::doStopAP()
{
    if (!ap_active_) {
        return;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (netif_) {
        esp_netif_destroy(netif_);
        netif_ = nullptr;
    }

    ap_active_ = false;
    LOG(TAG, "AP stopped");
}
