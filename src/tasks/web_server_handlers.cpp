#include "web_server.h"
#include "hid_bridge.h"
#include "config.h"
#include "storage.h"
#include "index_html.h"
#include "events.h"

#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_gap_ble_api.h"
#include "cJSON.h"
#include <string.h>
#include <string>

static const char* TAG = "web";

// Helper: get WebServer instance from request
static WebServer* getServer(httpd_req_t* req)
{
    return static_cast<WebServer*>(req->user_ctx);
}

// Helper: send JSON response
static esp_err_t sendJson(httpd_req_t* req, int code, cJSON* json)
{
    char* str = cJSON_PrintUnformatted(json);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    DBG(TAG, "RESP %d %s: %s", code, req->uri, str);

    if (code == 200) {
        httpd_resp_set_status(req, HTTPD_200);
    } else if (code == 202) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (code == 400) {
        httpd_resp_set_status(req, HTTPD_400);
    } else if (code == 409) {
        httpd_resp_set_status(req, "409 Conflict");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    cJSON_free(str);
    return ESP_OK;
}

// Helper: parse JSON from request body
static cJSON* parseRequestJson(httpd_req_t* req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > WEB_MAX_REQUEST_BODY) {
        return nullptr;
    }

    char buf[WEB_MAX_REQUEST_BODY + 1];
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            return nullptr;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    return cJSON_Parse(buf);
}

// GET / - serve gzip compressed HTML
esp_err_t WebServer::handleRoot(httpd_req_t* req)
{
    DBG(TAG, "RESP 200 /: (html %d bytes)", INDEX_HTML_GZ_LEN);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    return ESP_OK;
}

// GET /api/status - comprehensive status including all device lists
esp_err_t WebServer::handleStatus(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "connected", self->connected_count_ > 0);
    cJSON_AddNumberToObject(doc, "connectionCount", (int)self->connected_count_);
    cJSON_AddBoolToObject(doc, "scanning", self->hid_bridge_ && self->hid_bridge_->isScanning());

    // Add first device info for backwards compatibility
    if (self->connected_count_ > 0) {
        cJSON_AddStringToObject(doc, "name", self->connected_devices_[0].name);
        cJSON_AddStringToObject(doc, "address", self->connected_devices_[0].address);
    }

    // Connected devices
    cJSON* connArr = cJSON_AddArrayToObject(doc, "connections");
    for (size_t i = 0; i < self->connected_count_; i++) {
        const ConnectedDevice& d = self->connected_devices_[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "address", d.address);
        cJSON_AddStringToObject(obj, "name", d.name);
        cJSON_AddNumberToObject(obj, "deviceId", (int)i);
        cJSON_AddItemToArray(connArr, obj);
    }

    // Found devices from passive scan
    cJSON* foundArr = cJSON_AddArrayToObject(doc, "foundDevices");
    if (self->hid_bridge_) {
        FoundDevice devices[BLE_MAX_FOUND_DEVICES];
        size_t count = self->hid_bridge_->getFoundDevices(devices, BLE_MAX_FOUND_DEVICES);
        for (size_t i = 0; i < count; i++) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "address", devices[i].address);
            cJSON_AddStringToObject(obj, "name", devices[i].name);
            cJSON_AddNumberToObject(obj, "rssi", devices[i].rssi);
            cJSON_AddItemToArray(foundArr, obj);
        }
    }

    // Saved devices from storage
    cJSON* savedArr = cJSON_AddArrayToObject(doc, "savedDevices");
    std::string savedJson = self->storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(savedJson.c_str());
    if (saved && cJSON_IsArray(saved)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, saved) {
            cJSON* copy = cJSON_Duplicate(item, true);
            cJSON_AddItemToArray(savedArr, copy);
        }
        cJSON_Delete(saved);
    }

    // HID pipeline stats
    if (self->hid_bridge_) {
        HidStats s = self->hid_bridge_->getStats();
        cJSON* stats = cJSON_AddObjectToObject(doc, "stats");
        cJSON_AddNumberToObject(stats, "notifyIn", s.notifyIn);
        cJSON_AddNumberToObject(stats, "notifyDrop", s.notifyDrop);
        cJSON_AddNumberToObject(stats, "notifyOut", s.notifyOut);
        cJSON_AddNumberToObject(stats, "mouseSkip", s.mouseSkip);
        cJSON_AddNumberToObject(stats, "usbOk", s.usbOk);
        cJSON_AddNumberToObject(stats, "usbBusy", s.usbBusy);
        cJSON_AddNumberToObject(stats, "usbNotReady", s.usbNotReady);
        cJSON_AddNumberToObject(stats, "usbQueueFull", s.usbQueueFull);
        // Frame timing
        cJSON_AddNumberToObject(stats, "btFrameSumUs", (double)s.btFrameSumUs);
        cJSON_AddNumberToObject(stats, "btFrameCount", s.btFrameCount);
        cJSON_AddNumberToObject(stats, "usbFrameSumUs", (double)s.usbFrameSumUs);
        cJSON_AddNumberToObject(stats, "usbFrameCount", s.usbFrameCount);
        cJSON_AddNumberToObject(stats, "usbFrameMinUs", s.usbFrameMinUs);
        cJSON_AddNumberToObject(stats, "usbFrameMaxUs", s.usbFrameMaxUs);
    }

    // Bonded devices from Bluedroid
    cJSON* bondedArr = cJSON_AddArrayToObject(doc, "bondedDevices");
    int bondCount = esp_ble_get_bond_device_num();
    if (bondCount > 0) {
        esp_ble_bond_dev_t* devList = (esp_ble_bond_dev_t*)malloc(bondCount * sizeof(esp_ble_bond_dev_t));
        if (devList && esp_ble_get_bond_device_list(&bondCount, devList) == ESP_OK) {
            for (int i = 0; i < bondCount; i++) {
                char addr[18];
                snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                    devList[i].bd_addr[0], devList[i].bd_addr[1], devList[i].bd_addr[2],
                    devList[i].bd_addr[3], devList[i].bd_addr[4], devList[i].bd_addr[5]);

                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "address", addr);
                cJSON_AddItemToArray(bondedArr, obj);
            }
        }
        free(devList);
    }

    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// GET /api/scan - get found HID devices (passive scan results)
esp_err_t WebServer::handleScanGet(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "scanning", self->hid_bridge_ && self->hid_bridge_->isScanning());

    cJSON* arr = cJSON_AddArrayToObject(doc, "devices");

    if (self->hid_bridge_) {
        FoundDevice devices[BLE_MAX_FOUND_DEVICES];
        size_t count = self->hid_bridge_->getFoundDevices(devices, BLE_MAX_FOUND_DEVICES);

        for (size_t i = 0; i < count; i++) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "address", devices[i].address);
            cJSON_AddStringToObject(obj, "name", devices[i].name);
            cJSON_AddNumberToObject(obj, "rssi", devices[i].rssi);
            cJSON_AddItemToArray(arr, obj);
        }
    }

    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// POST /api/scan - start BLE scan
esp_err_t WebServer::handleScanPost(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    if (self->ble_cmd_queue_) {
        BleCmdMsg cmd = {};
        cmd.cmd = BleCmd::SCAN;
        xQueueSend(self->ble_cmd_queue_, &cmd, 0);
        LOG(TAG, "Scan requested via API");
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    cJSON_AddStringToObject(doc, "status", "scanning");
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// GET /api/bonded - get bonded devices from Bluedroid
esp_err_t WebServer::handleBondedGet(httpd_req_t* req)
{
    cJSON* doc = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(doc, "devices");

    int count = esp_ble_get_bond_device_num();
    if (count > 0) {
        esp_ble_bond_dev_t* devList = (esp_ble_bond_dev_t*)malloc(count * sizeof(esp_ble_bond_dev_t));
        if (devList && esp_ble_get_bond_device_list(&count, devList) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                char addr[18];
                snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                    devList[i].bd_addr[0], devList[i].bd_addr[1], devList[i].bd_addr[2],
                    devList[i].bd_addr[3], devList[i].bd_addr[4], devList[i].bd_addr[5]);

                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "address", addr);
                cJSON_AddItemToArray(arr, obj);
            }
        }
        free(devList);
    }

    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// DELETE /api/bonded - remove bonded device
esp_err_t WebServer::handleBondedDelete(httpd_req_t* req)
{
    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    cJSON* addrItem = cJSON_GetObjectItem(reqJson, "address");
    if (!addrItem || !cJSON_IsString(addrItem)) {
        cJSON_Delete(reqJson);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing address");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    // Parse address string to BDA
    esp_bd_addr_t bda;
    unsigned int t[6];
    if (sscanf(addrItem->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
               &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]) == 6) {
        for (int i = 0; i < 6; i++) bda[i] = t[i];
        esp_ble_remove_bond_device(bda);
        LOG(TAG, "Removed bond: %s", addrItem->valuestring);
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    cJSON_Delete(reqJson);
    return ESP_OK;
}

// POST /api/connect
esp_err_t WebServer::handleConnect(httpd_req_t* req)
{
    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    cJSON* addrItem = cJSON_GetObjectItem(reqJson, "address");
    if (!addrItem || !cJSON_IsString(addrItem)) {
        cJSON_Delete(reqJson);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing address");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    WebServer* self = getServer(req);

    // Look up device name from found devices
    char deviceName[32] = "";
    if (self->hid_bridge_) {
        FoundDevice devices[BLE_MAX_FOUND_DEVICES];
        size_t count = self->hid_bridge_->getFoundDevices(devices, BLE_MAX_FOUND_DEVICES);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(devices[i].address, addrItem->valuestring) == 0) {
                strncpy(deviceName, devices[i].name, sizeof(deviceName) - 1);
                break;
            }
        }
    }

    // Send connect command to BLE task
    if (self->ble_cmd_queue_) {
        BleCmdMsg cmd = {};
        cmd.cmd = BleCmd::CONNECT;
        strncpy(cmd.address, addrItem->valuestring, sizeof(cmd.address) - 1);
        strncpy(cmd.name, deviceName, sizeof(cmd.name) - 1);
        xQueueSend(self->ble_cmd_queue_, &cmd, 0);
        LOG(TAG, "Sent CONNECT to BLE: %s [%s]", deviceName, addrItem->valuestring);
    } else {
        ERR(TAG, "BLE cmd queue not set");
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "connecting");
    cJSON_AddStringToObject(doc, "address", addrItem->valuestring);
    sendJson(req, 202, doc);
    cJSON_Delete(doc);
    cJSON_Delete(reqJson);
    return ESP_OK;
}

// POST /api/disconnect
esp_err_t WebServer::handleDisconnect(httpd_req_t* req)
{
    WebServer* self = getServer(req);
    cJSON* reqJson = parseRequestJson(req);

    const char* address = "";
    int8_t deviceId = -1;

    if (reqJson) {
        cJSON* addrItem = cJSON_GetObjectItem(reqJson, "address");
        if (addrItem && cJSON_IsString(addrItem)) {
            address = addrItem->valuestring;
        }
        cJSON* idItem = cJSON_GetObjectItem(reqJson, "deviceId");
        if (idItem && cJSON_IsNumber(idItem)) {
            deviceId = (int8_t)idItem->valueint;
        }
    }

    // Send disconnect command to BLE task
    if (self->ble_cmd_queue_) {
        BleCmdMsg cmd = {};
        cmd.cmd = BleCmd::DISCONNECT;
        strncpy(cmd.address, address, sizeof(cmd.address) - 1);
        cmd.deviceId = deviceId;
        xQueueSend(self->ble_cmd_queue_, &cmd, 0);
        LOG(TAG, "Sent DISCONNECT to BLE: addr=%s id=%d", address, deviceId);
    } else {
        ERR(TAG, "BLE cmd queue not set");
    }

    if (reqJson) {
        cJSON_Delete(reqJson);
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// GET /api/saved
esp_err_t WebServer::handleSaved(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* doc = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(doc, "devices");

    // Load saved devices from storage
    std::string savedJson = self->storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(savedJson.c_str());
    if (saved && cJSON_IsArray(saved)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, saved) {
            cJSON* copy = cJSON_Duplicate(item, true);
            cJSON_AddItemToArray(arr, copy);
        }
        cJSON_Delete(saved);
    }

    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// POST /api/remove
esp_err_t WebServer::handleRemove(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    cJSON* addrItem = cJSON_GetObjectItem(reqJson, "address");
    if (!addrItem || !cJSON_IsString(addrItem)) {
        cJSON_Delete(reqJson);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing address");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    const char* addrToRemove = addrItem->valuestring;

    // Remove from saved devices (NVS)
    std::string savedJson = self->storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(savedJson.c_str());
    if (saved && cJSON_IsArray(saved)) {
        int size = cJSON_GetArraySize(saved);
        for (int i = size - 1; i >= 0; i--) {
            cJSON* item = cJSON_GetArrayItem(saved, i);
            cJSON* addr = cJSON_GetObjectItem(item, "address");
            if (addr && cJSON_IsString(addr) && strcmp(addr->valuestring, addrToRemove) == 0) {
                cJSON_DeleteItemFromArray(saved, i);
                LOG(TAG, "Removed device: %s", addrToRemove);
            }
        }
        char* newJson = cJSON_PrintUnformatted(saved);
        if (newJson) {
            self->storage_->set("saved_devs", std::string(newJson));
            cJSON_free(newJson);
        }
        cJSON_Delete(saved);
    }

    // Remove Bluedroid bond
    esp_bd_addr_t bda;
    unsigned int t[6];
    if (sscanf(addrToRemove, "%02x:%02x:%02x:%02x:%02x:%02x",
               &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]) == 6) {
        for (int i = 0; i < 6; i++) bda[i] = t[i];
        esp_ble_remove_bond_device(bda);
        LOG(TAG, "Removed bond: %s", addrToRemove);
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    cJSON_Delete(reqJson);
    return ESP_OK;
}

// POST /api/restart
esp_err_t WebServer::handleRestart(httpd_req_t* req)
{
    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);

    vTaskDelay(pdMS_TO_TICKS(WIFI_TOGGLE_DELAY_MS));
    esp_restart();
    return ESP_OK;
}

// GET /api/wifi
esp_err_t WebServer::handleWifiGet(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    std::string ssid = self->storage_->get("wifi_ssid", WIFI_AP_SSID);

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "name", ssid.c_str());
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// POST /api/factory-reset
esp_err_t WebServer::handleFactoryReset(httpd_req_t* req)
{
    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    cJSON* confirm = cJSON_GetObjectItem(reqJson, "confirm");
    if (!confirm || !cJSON_IsString(confirm) || strcmp(confirm->valuestring, "RESET") != 0) {
        cJSON_Delete(reqJson);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Send {\"confirm\":\"RESET\"} to confirm");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }
    cJSON_Delete(reqJson);

    LOG(TAG, "Factory reset requested via API");

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);

    vTaskDelay(pdMS_TO_TICKS(WIFI_TOGGLE_DELAY_MS));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

// POST /api/wifi
esp_err_t WebServer::handleWifiPost(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    // Update WiFi SSID if provided
    cJSON* nameItem = cJSON_GetObjectItem(reqJson, "name");
    if (nameItem && cJSON_IsString(nameItem)) {
        self->storage_->set("wifi_ssid", nameItem->valuestring);
        LOG(TAG, "WiFi SSID updated: %s", nameItem->valuestring);
    }

    // Update WiFi password if provided
    cJSON* passItem = cJSON_GetObjectItem(reqJson, "password");
    if (passItem && cJSON_IsString(passItem)) {
        self->storage_->set("wifi_pass", passItem->valuestring);
        LOG(TAG, "WiFi password updated");
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    cJSON_AddBoolToObject(doc, "restartRequired", true);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    cJSON_Delete(reqJson);
    return ESP_OK;
}

// GET /api/remaps?address=xx:xx:xx:xx:xx:xx
// Reads remap table via Storage (mutex-protected)
esp_err_t WebServer::handleRemapsGet(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    // Parse address from query string
    char query[64] = {};
    char address[18] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "address", address, sizeof(address));
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(doc, "entries");

    if (address[0] != '\0') {
        static constexpr size_t BLOB_ADDR_SIZE = 18;
        static constexpr size_t BLOB_ENTRY_SIZE = 3;
        uint8_t buf[BLOB_ADDR_SIZE + 1 + MAX_KEY_REMAPS * BLOB_ENTRY_SIZE];

        uint32_t count = self->storage_->getUInt("rmap_cnt", 0);

        for (uint32_t i = 0; i < count && i < MAX_REMAP_DEVICES; i++) {
            char key[12];
            snprintf(key, sizeof(key), "rmap_%d", (int)i);
            size_t blobLen = self->storage_->getBlob(key, buf, sizeof(buf));
            if (blobLen < BLOB_ADDR_SIZE + 1) continue;

            // Check address match
            buf[17] = '\0';
            if (strcmp((const char*)buf, address) != 0) continue;

            uint8_t entryCount = buf[BLOB_ADDR_SIZE];
            if (entryCount > MAX_KEY_REMAPS) entryCount = MAX_KEY_REMAPS;

            const uint8_t* p = buf + BLOB_ADDR_SIZE + 1;
            for (int j = 0; j < entryCount; j++) {
                cJSON* entry = cJSON_CreateObject();
                cJSON_AddNumberToObject(entry, "from", p[0]);
                uint16_t to = p[1] | (p[2] << 8);
                cJSON_AddNumberToObject(entry, "to", to);
                cJSON_AddItemToArray(arr, entry);
                p += BLOB_ENTRY_SIZE;
            }
            break;
        }
    }

    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    return ESP_OK;
}

// POST /api/remaps — { "address": "xx:...", "entries": [{"from": 0x39, "to": 0x00E0}, ...] }
// Sends SET_REMAPS command via queue to HidBridge (Core 1)
esp_err_t WebServer::handleRemapsPost(httpd_req_t* req)
{
    WebServer* self = getServer(req);

    cJSON* reqJson = parseRequestJson(req);
    if (!reqJson) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    cJSON* addrItem = cJSON_GetObjectItem(reqJson, "address");
    if (!addrItem || !cJSON_IsString(addrItem)) {
        cJSON_Delete(reqJson);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing address");
        sendJson(req, 400, err);
        cJSON_Delete(err);
        return ESP_OK;
    }

    // Build SET_REMAPS command
    BleCmdMsg cmd = {};
    cmd.cmd = BleCmd::SET_REMAPS;
    strncpy(cmd.address, addrItem->valuestring, sizeof(cmd.address) - 1);
    cmd.remapCount = 0;

    cJSON* entries = cJSON_GetObjectItem(reqJson, "entries");
    if (entries && cJSON_IsArray(entries)) {
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, entries) {
            if (cmd.remapCount >= BLE_CMD_MAX_REMAPS) break;
            cJSON* from = cJSON_GetObjectItem(entry, "from");
            cJSON* to = cJSON_GetObjectItem(entry, "to");
            if (from && to && cJSON_IsNumber(from) && cJSON_IsNumber(to)) {
                cmd.remapEntries[cmd.remapCount].from = (uint8_t)from->valueint;
                cmd.remapEntries[cmd.remapCount].to = (uint16_t)to->valueint;
                cmd.remapCount++;
            }
        }
    }

    // Send via cmd queue to HidBridge
    if (self->ble_cmd_queue_) {
        xQueueSend(self->ble_cmd_queue_, &cmd, pdMS_TO_TICKS(100));
        LOG(TAG, "SET_REMAPS: %s (%d entries)", cmd.address, cmd.remapCount);
    }

    cJSON* doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "success", true);
    cJSON_AddNumberToObject(doc, "count", cmd.remapCount);
    sendJson(req, 200, doc);
    cJSON_Delete(doc);
    cJSON_Delete(reqJson);
    return ESP_OK;
}

