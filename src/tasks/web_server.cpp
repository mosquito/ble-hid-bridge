#include "web_server.h"
#include "config.h"
#include "storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string>

static const char* TAG = "web";

WebServer::WebServer(Storage* storage)
    : storage_(storage)
{
    // Create event queue in constructor so getEventQueue() works before init()
    event_queue_ = xQueueCreateStatic(
        EVENT_QUEUE_SIZE,
        sizeof(WebEvent),
        event_queue_storage_,
        &event_queue_buffer_
    );

    // Initialize connected devices array
    for (size_t i = 0; i < MAX_CONNECTED_DEVICES; i++) {
        connected_devices_[i].active = false;
        connected_devices_[i].address[0] = '\0';
        connected_devices_[i].name[0] = '\0';
    }
}

void WebServer::init()
{
    LOG(TAG, "Task started on core %d", xPortGetCoreID());
}

void WebServer::run()
{
    processEvents();
    vTaskDelay(pdMS_TO_TICKS(WEB_SERVER_POLL_MS));
}

void WebServer::processEvents()
{
    WebEvent event;
    while (xQueueReceive(event_queue_, &event, 0) == pdTRUE) {
        DBG(TAG, "Event: type=%d", (int)event.type);
        switch (event.type) {
            case WebEventType::DEVICE_FOUND:
                DBG(TAG, "Device found: %s [%s]", event.name, event.address);
                // Found devices are now tracked in BleHidHost
                break;

            case WebEventType::CONNECTED:
                LOG(TAG, "Connected: %s [%s]", event.name, event.address);
                saveDevice(event.address, event.name);
                // Track connection
                if (connected_count_ < MAX_CONNECTED_DEVICES) {
                    ConnectedDevice& d = connected_devices_[connected_count_++];
                    d.active = true;
                    strncpy(d.address, event.address, sizeof(d.address) - 1);
                    d.address[sizeof(d.address) - 1] = '\0';
                    strncpy(d.name, event.name, sizeof(d.name) - 1);
                    d.name[sizeof(d.name) - 1] = '\0';
                }
                break;

            case WebEventType::DISCONNECTED:
                LOG(TAG, "Disconnected: %s", event.address);
                // Remove from tracked connections
                for (size_t i = 0; i < connected_count_; i++) {
                    if (strcmp(connected_devices_[i].address, event.address) == 0) {
                        // Shift remaining devices
                        for (size_t j = i; j < connected_count_ - 1; j++) {
                            connected_devices_[j] = connected_devices_[j + 1];
                        }
                        connected_count_--;
                        break;
                    }
                }
                break;
        }
    }
}

void WebServer::saveDevice(const char* address, const char* name)
{
    if (!storage_ || !address || address[0] == '\0') return;

    // Load existing saved devices
    std::string savedJson = storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(savedJson.c_str());
    if (!saved) {
        saved = cJSON_CreateArray();
    }

    // Check if device already exists
    bool found = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, saved) {
        cJSON* addr = cJSON_GetObjectItem(item, "address");
        if (addr && cJSON_IsString(addr) && strcmp(addr->valuestring, address) == 0) {
            found = true;
            // Update name if changed
            cJSON* nameItem = cJSON_GetObjectItem(item, "name");
            if (nameItem && name && name[0]) {
                cJSON_SetValuestring(nameItem, name);
            }
            break;
        }
    }

    // Add new device if not found
    if (!found) {
        cJSON* newDev = cJSON_CreateObject();
        cJSON_AddStringToObject(newDev, "address", address);
        cJSON_AddStringToObject(newDev, "name", name ? name : "");
        cJSON_AddItemToArray(saved, newDev);
        LOG(TAG, "Saved device: %s [%s]", name, address);
    }

    // Save back to storage
    char* newJson = cJSON_PrintUnformatted(saved);
    if (newJson) {
        storage_->set("saved_devs", std::string(newJson));
        cJSON_free(newJson);
    }
    cJSON_Delete(saved);
}

void WebServer::start()
{
    if (server_ != nullptr) {
        WARN(TAG, "Server already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = WEB_MAX_URI_HANDLERS;
    config.core_id = RTOS_CORE_0;  // Pin to Core 0 (Core 1 is busy with HidBridge REALTIME)

    esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        ERR(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return;
    }

    registerHandlers();
    LOG(TAG, "Server started");
}

void WebServer::stop()
{
    if (server_ == nullptr) {
        return;
    }

    httpd_stop(server_);
    server_ = nullptr;
    LOG(TAG, "Server stopped");
}

void WebServer::registerHandlers()
{
    // Helper macro to register handler with this pointer as user_ctx
    #define REGISTER(uri_str, method_val, handler_fn) \
        { \
            httpd_uri_t uri = { \
                .uri = uri_str, \
                .method = method_val, \
                .handler = handler_fn, \
                .user_ctx = this \
            }; \
            httpd_register_uri_handler(server_, &uri); \
        }

    REGISTER("/", HTTP_GET, handleRoot);
    REGISTER("/api/status", HTTP_GET, handleStatus);
    REGISTER("/api/scan", HTTP_GET, handleScanGet);
    REGISTER("/api/scan", HTTP_POST, handleScanPost);
    REGISTER("/api/connect", HTTP_POST, handleConnect);
    REGISTER("/api/disconnect", HTTP_POST, handleDisconnect);
    REGISTER("/api/bonded", HTTP_GET, handleBondedGet);
    REGISTER("/api/bonded", HTTP_DELETE, handleBondedDelete);
    REGISTER("/api/saved", HTTP_GET, handleSaved);
    REGISTER("/api/remove", HTTP_POST, handleRemove);
    REGISTER("/api/restart", HTTP_POST, handleRestart);
    REGISTER("/api/wifi", HTTP_GET, handleWifiGet);
    REGISTER("/api/wifi", HTTP_POST, handleWifiPost);
    REGISTER("/api/factory-reset", HTTP_POST, handleFactoryReset);
    REGISTER("/api/remaps", HTTP_GET, handleRemapsGet);
    REGISTER("/api/remaps", HTTP_POST, handleRemapsPost);

    #undef REGISTER
}
