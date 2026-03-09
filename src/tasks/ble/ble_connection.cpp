#include "ble_connection.h"
#include "config.h"
#include "usb_hid_api.h"

#include "esp_gap_ble_api.h"
#include <cstring>

static const char* TAG = "ble_conn";
static uint32_t s_mouse_skip = 0;

uint32_t bleConnectionMouseSkips() { return s_mouse_skip; }

void BleConnection::reset()
{
    state_ = ConnState::IDLE;
    connId_ = 0;
    gattcIf_ = ESP_GATT_IF_NONE;
    memset(bda_, 0, sizeof(bda_));
    addrType_ = 0;
    address_[0] = '\0';
    name_[0] = '\0';
    svcStart_ = 0;
    svcEnd_ = 0;
    reportCount_ = 0;
    nextNotifyIdx_ = 0;
    parsedHid_ = {};
    lastButtons_ = 0;
    reportMapRequested_ = false;
    pendingDescrReads_ = 0;
    remapTable_ = nullptr;
}

void BleConnection::startConnect(esp_gatt_if_t gattcIf, const esp_bd_addr_t bda,
                                  uint8_t addrType, const char* address,
                                  const char* name, SecurityLevel secLevel)
{
    reset();
    gattcIf_ = gattcIf;
    memcpy(bda_, bda, 6);
    addrType_ = addrType;
    strncpy(address_, address, sizeof(address_) - 1);
    strncpy(name_, name, sizeof(name_) - 1);
    secLevel_ = secLevel;
    transitionTo(ConnState::CONNECTING);
}

void BleConnection::transitionTo(ConnState newState)
{
    DBG(TAG, "[%s] %s -> %s", address_, connStateName(state_), connStateName(newState));
    state_ = newState;
}

bool BleConnection::handleGattcEvent(const GattcEventData& evt,
                                      QueueHandle_t eventQueue)
{
    switch (evt.event) {
        case ESP_GATTC_OPEN_EVT:
            onOpen(evt, eventQueue);
            return true;

        case ESP_GATTC_CLOSE_EVT:
            onClose(evt, eventQueue);
            return true;

        case ESP_GATTC_SEARCH_RES_EVT:
            onSearchRes(evt);
            return true;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            onSearchCmpl(evt, eventQueue);
            return true;

        case ESP_GATTC_READ_CHAR_EVT:
            onReadChar(evt);
            return true;

        case ESP_GATTC_READ_DESCR_EVT:
            onReadDescr(evt);
            return true;

        case ESP_GATTC_NOTIFY_EVT:
            onNotify(evt);
            return true;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            DBG(TAG, "[%s] REG_FOR_NOTIFY h=%d status=%d",
                address_, evt.regForNotify.handle, evt.regForNotify.status);
            return true;

        case ESP_GATTC_WRITE_DESCR_EVT:
            DBG(TAG, "[%s] WRITE_DESCR h=%d status=%d",
                address_, evt.write.handle, evt.write.status);
            return true;

        default:
            return false;
    }
}

void BleConnection::onOpen(const GattcEventData& evt, QueueHandle_t eventQueue)
{
    if (state_ != ConnState::CONNECTING) return;

    if (evt.open.status == ESP_GATT_OK) {
        connId_ = evt.open.connId;
        gattcIf_ = evt.gattcIf;
        transitionTo(ConnState::DISCOVERING_SVC);

        // Request fast connection interval for responsive HID
        esp_ble_conn_update_params_t conn_params = {};
        memcpy(conn_params.bda, bda_, sizeof(esp_bd_addr_t));
        conn_params.min_int = BLE_CONN_MIN_INTERVAL;
        conn_params.max_int = BLE_CONN_MAX_INTERVAL;
        conn_params.latency = BLE_CONN_LATENCY;
        conn_params.timeout = BLE_CONN_TIMEOUT;
        esp_ble_gap_update_conn_params(&conn_params);

        // Skip encryption for devices that don't support pairing
        if (secLevel_ != SecurityLevel::NONE) {
            esp_ble_set_encryption(bda_, ESP_BLE_SEC_ENCRYPT);
        }
        esp_ble_gattc_search_service(gattcIf_, connId_, nullptr);

        LOG(TAG, "[%s] Connected (sec=%d, interval=%d-%d)",
            address_, (int)secLevel_, BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL);
    } else {
        ERR(TAG, "[%s] Connect failed: 0x%x", address_, evt.open.status);
        sendEvent(eventQueue, BleEvent::CONNECT_FAILED, address_);
        reset();
    }
}

void BleConnection::onClose(const GattcEventData& evt, QueueHandle_t eventQueue)
{
    LOG(TAG, "[%s] Disconnected (reason=0x%x)", address_, evt.close.reason);
    sendEvent(eventQueue, BleEvent::DISCONNECTED, address_);
    reset();
}

void BleConnection::onSearchRes(const GattcEventData& evt)
{
    if (state_ != ConnState::DISCOVERING_SVC) return;

    uint16_t uuid16 = 0;
    if (evt.searchRes.srvcId.uuid.len == ESP_UUID_LEN_16) {
        uuid16 = evt.searchRes.srvcId.uuid.uuid.uuid16;
    }

    if (uuid16 == HID_SVC_UUID) {
        svcStart_ = evt.searchRes.startHandle;
        svcEnd_ = evt.searchRes.endHandle;
        LOG(TAG, "[%s] Found HID service: %d-%d", address_, svcStart_, svcEnd_);
    }
}

void BleConnection::onSearchCmpl(const GattcEventData& evt, QueueHandle_t eventQueue)
{
    if (state_ != ConnState::DISCOVERING_SVC) return;

    if (svcStart_ > 0) {
        LOG(TAG, "[%s] Service search complete, HID found", address_);
        transitionTo(ConnState::READING_REPORT_MAP);
        sendEvent(eventQueue, BleEvent::CONNECTED, address_, name_);
        doReadReportMap();
    } else {
        ERR(TAG, "[%s] No HID service found, disconnecting", address_);
        esp_ble_gattc_close(gattcIf_, connId_);
    }
}

void BleConnection::onReadChar(const GattcEventData& evt)
{
    if (state_ != ConnState::READING_REPORT_MAP) return;

    if (evt.read.status != ESP_GATT_OK) {
        ERR(TAG, "[%s] Read char failed: 0x%x", address_, evt.read.status);
        transitionTo(ConnState::DISCOVERING_CHARS);
        doDiscoverChars();
        return;
    }

    if (evt.read.valueLen > 0 && parsedHid_.reportCount == 0) {
        DBG(TAG, "[%s] Parsing HID Report Map (%d bytes)", address_, evt.read.valueLen);
        if (HidParser::parse(evt.read.value, evt.read.valueLen, &parsedHid_)) {
            DBG(TAG, "[%s] HID parsed: %d reports", address_, parsedHid_.reportCount);
            for (int i = 0; i < parsedHid_.reportCount; i++) {
                auto& r = parsedHid_.reports[i];
                const char* type = "other";
                if (r.isKeyboard()) type = "kbd";
                else if (r.isMouse()) type = "mouse";
                DBG(TAG, "[%s] Report[%d] id=%d type=%s bytes=%d fields=%d",
                    address_, i, r.reportId, type, r.totalBytes(), r.fieldCount);
                for (int j = 0; j < r.fieldCount; j++) {
                    auto& f = r.fields[j];
                    DBG(TAG, "  field[%d] type=%d off=%d bits=%d",
                        j, (int)f.type, f.bitOffset, f.bitSize);
                }
            }
        } else {
            WARN(TAG, "[%s] HID parsing failed, will use fallback", address_);
        }
    }

    transitionTo(ConnState::DISCOVERING_CHARS);
    doDiscoverChars();
}

void BleConnection::onReadDescr(const GattcEventData& evt)
{
    if (state_ != ConnState::DISCOVERING_CHARS) return;

    if (pendingDescrReads_ > 0) {
        pendingDescrReads_--;
    }

    if (evt.read.status == ESP_GATT_OK && evt.read.valueLen >= 2) {
        uint8_t reportId = evt.read.value[0];
        uint8_t reportType = evt.read.value[1];
        DBG(TAG, "[%s] Report Ref: id=%d type=%d", address_, reportId, reportType);

        // Match descriptor to characteristic by handle proximity
        for (int i = 0; i < reportCount_; i++) {
            uint16_t charHandle = reports_[i].charHandle;
            if (evt.read.handle > charHandle && evt.read.handle <= charHandle + 3) {
                if (reports_[i].reportId == 0) {
                    reports_[i].reportId = reportId;
                    reports_[i].reportType = reportType;
                    DBG(TAG, "[%s] Report[%d] h=%d: id=%d type=%d",
                        address_, i, charHandle, reportId, reportType);
                    break;
                }
            }
        }
    }

    // When all descriptor reads complete, start notification registration
    if (pendingDescrReads_ <= 0) {
        DBG(TAG, "[%s] All descriptor reads complete", address_);
        nextNotifyIdx_ = 0;
        transitionTo(ConnState::REGISTERING_NOTIFY);
        doRegisterNextNotify();
    }
}

void BleConnection::onNotify(const GattcEventData& evt)
{
    if (evt.notify.valueLen == 0) return;
    processNotify(evt.notify.handle, evt.notify.value, evt.notify.valueLen);
}

void BleConnection::processNotify(uint16_t handle, const uint8_t* data, uint16_t len)
{
    if (len == 0) return;

    uint8_t reportId = getReportId(handle);

    // Check parsed HID descriptor for report type
    const HidParser::ReportFormat* fmt = parsedHid_.findReport(reportId);

    // Consumer Control report (media keys, volume)
    if (fmt && fmt->isConsumer()) {
        uint16_t usage = 0;

        // Decode consumer usage from report
        for (int i = 0; i < fmt->fieldCount; i++) {
            const auto& f = fmt->fields[i];
            if (f.type == HidParser::FieldType::CONSUMER) {
                usage = (uint16_t)HidParser::decodeField(data, f);
                break;
            }
        }

        DBG(TAG, "[%s] CONSUMER usage=0x%04x", address_, usage);
        UsbHid::sendConsumer(usage);
        return;
    }

    // Parsed mouse report (trust the parser, not length)
    if (fmt && fmt->isMouse()) {
        uint8_t prevButtons = lastButtons_;
        uint8_t buttons = lastButtons_;
        int16_t x = 0, y = 0;
        int8_t wheel = 0, hwheel = 0;
        bool hasXY = false;

        for (int i = 0; i < fmt->fieldCount; i++) {
            const auto& f = fmt->fields[i];
            int32_t val = HidParser::decodeField(data, f);
            switch (f.type) {
                case HidParser::FieldType::BUTTONS:
                    buttons = val;
                    lastButtons_ = val;
                    break;
                case HidParser::FieldType::X: x = val; hasXY = true; break;
                case HidParser::FieldType::Y: y = val; hasXY = true; break;
                case HidParser::FieldType::WHEEL: wheel = val; break;
                case HidParser::FieldType::HWHEEL: hwheel = val; break;
                default: break;
            }
        }

        // Reports without X/Y (e.g. buttons+wheel only): skip if nothing changed.
        // Avoids x=0,y=0 "stop frames" that reset host pointer acceleration.
        if (!hasXY && buttons == prevButtons && wheel == 0 && hwheel == 0) {
            s_mouse_skip++;
            return;
        }

        DBG(TAG, "[%s] MOUSE btn=%d x=%d y=%d w=%d hw=%d", address_, buttons, x, y, wheel, hwheel);
        UsbHid::sendMouse(buttons, x, y, wheel, hwheel);
        return;
    }

    // Keyboard: standard boot protocol format (modifier + reserved + 6 keycodes)
    // Use parser for type detection, raw data for decoding (Array fields aren't decodable per-field)
    if ((fmt && fmt->isKeyboard()) || (!fmt && len == HID_KEYBOARD_REPORT_SIZE)) {
        if (len < HID_KEYBOARD_REPORT_SIZE) {
            WARN(TAG, "[%s] KBD report too short: %d", address_, (int)len);
            return;
        }
        uint8_t modifier = data[0];
        uint8_t keys[HID_KEYBOARD_KEY_COUNT];
        memcpy(keys, &data[2], HID_KEYBOARD_KEY_COUNT);

        uint16_t consumerUsage = 0;
        if (remapTable_) {
            consumerUsage = applyRemap(remapTable_, modifier, keys);
        }

        DBG(TAG, "[%s] KBD mod=%d keys=%02x,%02x,%02x,%02x,%02x,%02x",
            address_, modifier,
            keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]);
        UsbHid::sendKeyboard(modifier, keys);

        if (consumerUsage != 0) {
            DBG(TAG, "[%s] KBD->CONSUMER usage=0x%04x", address_, consumerUsage);
            UsbHid::sendConsumer(consumerUsage);
        }
        return;
    }

    // Consumer fallback: 2-byte report without parsed descriptor
    if (len == HID_CONSUMER_REPORT_SIZE && !fmt) {
        uint16_t usage = data[0] | (data[1] << 8);
        DBG(TAG, "[%s] CONSUMER (fallback) usage=0x%04x", address_, usage);
        UsbHid::sendConsumer(usage);
        return;
    }

    // Mouse fallback: 3-7 byte report without parsed descriptor
    if (len >= 3 && len <= 7 && !fmt) {
        uint8_t buttons = data[0];
        lastButtons_ = buttons;
        int16_t x = (int8_t)data[1];
        int16_t y = (int8_t)data[2];
        int8_t wheel = (len >= 4) ? (int8_t)data[3] : 0;

        DBG(TAG, "[%s] MOUSE (fallback) btn=%d x=%d y=%d w=%d", address_, buttons, x, y, wheel);
        UsbHid::sendMouse(buttons, x, y, wheel);
        return;
    }

    // Unknown report
    WARN(TAG, "[%s] Unknown report len=%d id=%d: %02x %02x %02x...",
        address_, (int)len, reportId, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
}

void BleConnection::doReadReportMap()
{
    if (svcStart_ == 0 || reportMapRequested_) return;

    esp_gattc_char_elem_t chars[BLE_MAX_GATTC_CHARS];
    uint16_t count = BLE_MAX_GATTC_CHARS;
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = HID_REPORT_MAP_UUID}};

    esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(
        gattcIf_, connId_, svcStart_, svcEnd_, uuid, chars, &count);

    if (status == ESP_GATT_OK && count > 0) {
        reportMapRequested_ = true;
        esp_err_t err = esp_ble_gattc_read_char(
            gattcIf_, connId_, chars[0].char_handle, ESP_GATT_AUTH_REQ_NONE);
        DBG(TAG, "[%s] Reading Report Map h=%d err=%s",
            address_, chars[0].char_handle, esp_err_to_name(err));
    } else {
        WARN(TAG, "[%s] Report Map not found, skipping", address_);
        transitionTo(ConnState::DISCOVERING_CHARS);
        doDiscoverChars();
    }
}

void BleConnection::doDiscoverChars()
{
    if (svcStart_ == 0 || reportCount_ > 0) return;

    esp_gattc_char_elem_t chars[BLE_MAX_GATTC_CHARS];
    uint16_t count = BLE_MAX_GATTC_CHARS;
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = HID_REPORT_UUID}};

    esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(
        gattcIf_, connId_, svcStart_, svcEnd_, uuid, chars, &count);

    pendingDescrReads_ = 0;

    if (status == ESP_GATT_OK) {
        DBG(TAG, "[%s] Found %d Report characteristics", address_, count);

        for (int j = 0; j < (int)count; j++) {
            bool hasNotify = (chars[j].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY);
            int reportIdx = addReport(chars[j].char_handle, 0, 0, hasNotify);

            // Find CCC descriptor
            if (hasNotify && reportIdx >= 0) {
                esp_gattc_descr_elem_t cccDescrs[2];
                uint16_t cccCount = 2;
                esp_bt_uuid_t cccUuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = CCC_UUID}};

                if (esp_ble_gattc_get_descr_by_char_handle(
                        gattcIf_, connId_, chars[j].char_handle, cccUuid,
                        cccDescrs, &cccCount) == ESP_GATT_OK && cccCount > 0) {
                    reports_[reportIdx].cccHandle = cccDescrs[0].handle;
                }
            }

            // Read Report Reference descriptor
            esp_gattc_descr_elem_t descrs[4];
            uint16_t dcount = 4;
            esp_bt_uuid_t duuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = HID_REPORT_REF_UUID}};

            if (esp_ble_gattc_get_descr_by_char_handle(
                    gattcIf_, connId_, chars[j].char_handle, duuid,
                    descrs, &dcount) == ESP_GATT_OK && dcount > 0) {
                esp_ble_gattc_read_char_descr(
                    gattcIf_, connId_, descrs[0].handle, ESP_GATT_AUTH_REQ_NONE);
                pendingDescrReads_++;
            }
        }
    }

    // If no descriptor reads pending, immediately start notifications
    if (pendingDescrReads_ == 0) {
        nextNotifyIdx_ = 0;
        transitionTo(ConnState::REGISTERING_NOTIFY);
        doRegisterNextNotify();
    }
}

void BleConnection::doRegisterNextNotify()
{
    int registered = 0;

    while (nextNotifyIdx_ < reportCount_) {
        ReportInfo& report = reports_[nextNotifyIdx_];

        if (report.reportType == 1 && report.hasNotify && !report.notifyRegistered) {
            report.notifyRegistered = true;
            registered++;

            const HidParser::ReportFormat* fmt = parsedHid_.findReport(report.reportId);
            const char* type = "unknown";
            if (fmt) {
                if (fmt->isKeyboard()) type = "kbd";
                else if (fmt->isMouse()) type = "mouse";
            }
            DBG(TAG, "[%s] Registering notify h=%d id=%d (%s)",
                address_, report.charHandle, report.reportId, type);

            esp_ble_gattc_register_for_notify(gattcIf_, bda_, report.charHandle);

            if (report.cccHandle > 0) {
                uint8_t notifyEnable[2] = {0x01, 0x00};
                esp_ble_gattc_write_char_descr(
                    gattcIf_, connId_, report.cccHandle,
                    sizeof(notifyEnable), notifyEnable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            }
        }
        nextNotifyIdx_++;
    }

    transitionTo(ConnState::READY);
    LOG(TAG, "[%s] Connection ready (reports=%d, notifications=%d)",
        name_, reportCount_, registered);
}

int BleConnection::addReport(uint16_t charHandle, uint8_t reportId,
                              uint8_t reportType, bool hasNotify)
{
    if (reportCount_ >= MAX_REPORTS) return -1;

    int idx = reportCount_++;
    reports_[idx].charHandle = charHandle;
    reports_[idx].cccHandle = 0;
    reports_[idx].reportId = reportId;
    reports_[idx].reportType = reportType;
    reports_[idx].hasNotify = hasNotify;
    reports_[idx].notifyRegistered = false;
    return idx;
}

ReportInfo* BleConnection::findReportByHandle(uint16_t charHandle)
{
    for (int i = 0; i < reportCount_; i++) {
        if (reports_[i].charHandle == charHandle) {
            return &reports_[i];
        }
    }
    return nullptr;
}

uint8_t BleConnection::getReportId(uint16_t charHandle)
{
    for (int i = 0; i < reportCount_; i++) {
        if (reports_[i].charHandle == charHandle) {
            return reports_[i].reportId;
        }
    }
    return 0;
}

void BleConnection::sendEvent(QueueHandle_t queue, BleEvent event,
                               const char* addr, const char* devName)
{
    if (!queue) return;

    BleEventMsg msg = {};
    msg.event = event;
    strncpy(msg.address, addr, sizeof(msg.address) - 1);
    strncpy(msg.name, devName, sizeof(msg.name) - 1);

    xQueueSend(queue, &msg, 0);
}
