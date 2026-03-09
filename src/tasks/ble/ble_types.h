#pragma once

#include "config.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "hid_parser.h"

#include <cstring>

/**
 * BLE Security Levels for adaptive pairing
 *
 * Devices have different security capabilities. We start with MEDIUM
 * and downgrade on auth failure (0x66 = SMP_PAIR_NOT_SUPPORT).
 * The working level is saved per device for future connections.
 */
enum class SecurityLevel : uint8_t {
    HIGH = 0,    // SC + MITM + Bond (ESP_IO_CAP_IO for numeric comparison)
    MEDIUM = 1,  // SC + Bond without MITM (ESP_IO_CAP_NONE, Just Works)
    LOW = 2,     // Legacy bond (compatibility with older devices)
    NONE = 3     // No bonding (for simple devices that don't support pairing)
};

/**
 * BLE Internal Event Types for Event Queue Pattern
 *
 * All BLE callbacks (GAP, GATTC) enqueue events instead of processing directly.
 * This eliminates race conditions between BTC callback thread and BLE task.
 */

// HID Service UUIDs
static constexpr uint16_t HID_SVC_UUID = 0x1812;
static constexpr uint16_t HID_REPORT_UUID = 0x2A4D;
static constexpr uint16_t HID_REPORT_MAP_UUID = 0x2A4B;
static constexpr uint16_t HID_REPORT_REF_UUID = 0x2908;
static constexpr uint16_t CCC_UUID = 0x2902;  // Client Characteristic Configuration

/**
 * Connection state machine states
 */
enum class ConnState : uint8_t {
    IDLE,               // Slot free
    CONNECTING,         // gattc_open called, waiting for OPEN_EVT
    DISCOVERING_SVC,    // Service discovery in progress
    READING_REPORT_MAP, // Reading HID Report Map characteristic
    DISCOVERING_CHARS,  // Discovering characteristics and descriptors
    REGISTERING_NOTIFY, // Registering for notifications
    READY,              // Fully operational, receiving HID reports
    DISCONNECTING       // Closing connection
};

inline const char* connStateName(ConnState state)
{
    switch (state) {
        case ConnState::IDLE: return "IDLE";
        case ConnState::CONNECTING: return "CONNECTING";
        case ConnState::DISCOVERING_SVC: return "DISCOVERING_SVC";
        case ConnState::READING_REPORT_MAP: return "READING_REPORT_MAP";
        case ConnState::DISCOVERING_CHARS: return "DISCOVERING_CHARS";
        case ConnState::REGISTERING_NOTIFY: return "REGISTERING_NOTIFY";
        case ConnState::READY: return "READY";
        case ConnState::DISCONNECTING: return "DISCONNECTING";
        default: return "UNKNOWN";
    }
}

/**
 * Report information for a single HID report characteristic
 */
struct ReportInfo {
    uint16_t charHandle;
    uint16_t cccHandle;     // CCC descriptor handle for enabling notifications
    uint8_t reportId;
    uint8_t reportType;     // 1=Input, 2=Output, 3=Feature
    bool hasNotify;
    bool notifyRegistered;
};

/**
 * GAP event wrapper - copies all data needed for deferred processing
 */
struct GapEventData {
    esp_gap_ble_cb_event_t event;

    // Union of relevant event data (copied, not referenced)
    union {
        struct {
            esp_bt_status_t status;
        } scanParamCmpl;

        struct {
            esp_bt_status_t status;
        } scanStartCmpl;

        struct {
            esp_gap_search_evt_t searchEvt;
            esp_bd_addr_t bda;
            esp_ble_addr_type_t addrType;
            uint8_t bleAdv[ESP_BLE_ADV_DATA_LEN_MAX];
            int rssi;
        } scanResult;

        struct {
            esp_bd_addr_t bda;
        } secReq;

        struct {
            esp_bd_addr_t bda;
            uint32_t passkey;
        } keyNotif;

        struct {
            esp_bd_addr_t bda;
            bool success;
            uint8_t failReason;
        } authCmpl;

        struct {
            esp_bt_status_t status;
            esp_bd_addr_t bda;
            uint16_t conn_int;   // Current interval (units of 1.25ms)
            uint16_t latency;
            uint16_t timeout;
        } connParams;
    };
};

/**
 * GATTC event wrapper - copies all data needed for deferred processing
 */
struct GattcEventData {
    esp_gattc_cb_event_t event;
    esp_gatt_if_t gattcIf;

    // Union of relevant event data (copied, not referenced)
    union {
        struct {
            esp_gatt_status_t status;
            uint16_t appId;
        } reg;

        struct {
            esp_gatt_status_t status;
            uint16_t connId;
            esp_bd_addr_t remoteBda;
        } open;

        struct {
            uint16_t connId;
            esp_gatt_conn_reason_t reason;
        } close;

        struct {
            esp_gatt_status_t status;
            uint16_t connId;
            uint16_t mtu;
        } cfgMtu;

        struct {
            uint16_t connId;
            esp_gatt_id_t srvcId;
            uint16_t startHandle;
            uint16_t endHandle;
        } searchRes;

        struct {
            esp_gatt_status_t status;
            uint16_t connId;
        } searchCmpl;

        struct {
            esp_gatt_status_t status;
            uint16_t connId;
            uint16_t handle;
            uint16_t valueLen;
            uint8_t value[BLE_MAX_REPORT_MAP_SIZE];
        } read;

        struct {
            esp_gatt_status_t status;
            uint16_t handle;
        } regForNotify;

        struct {
            esp_gatt_status_t status;
            uint16_t connId;
            uint16_t handle;
        } write;

        struct {
            uint16_t connId;
            uint16_t handle;
            uint16_t valueLen;
            uint8_t value[BLE_MAX_NOTIFY_DATA_SIZE];
            bool isNotify;
        } notify;
    };
};

/**
 * Internal BLE event - wraps either GAP or GATTC event
 */
struct BleInternalEvent {
    enum class Type : uint8_t { GAP, GATTC } type;

    union {
        GapEventData gap;
        GattcEventData gattc;
    };
};

/**
 * Helper to create GAP event from callback parameters
 */
inline BleInternalEvent makeGapEvent(esp_gap_ble_cb_event_t event,
                                      esp_ble_gap_cb_param_t* param)
{
    BleInternalEvent evt = {};
    evt.type = BleInternalEvent::Type::GAP;
    evt.gap.event = event;

    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            evt.gap.scanParamCmpl.status = param->scan_param_cmpl.status;
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            evt.gap.scanStartCmpl.status = param->scan_start_cmpl.status;
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            evt.gap.scanResult.searchEvt = param->scan_rst.search_evt;
            memcpy(evt.gap.scanResult.bda, param->scan_rst.bda, 6);
            evt.gap.scanResult.addrType = param->scan_rst.ble_addr_type;
            memcpy(evt.gap.scanResult.bleAdv, param->scan_rst.ble_adv,
                   ESP_BLE_ADV_DATA_LEN_MAX);
            evt.gap.scanResult.rssi = param->scan_rst.rssi;
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            // No data needed
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            memcpy(evt.gap.secReq.bda, param->ble_security.ble_req.bd_addr, 6);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            memcpy(evt.gap.keyNotif.bda, param->ble_security.key_notif.bd_addr, 6);
            evt.gap.keyNotif.passkey = param->ble_security.key_notif.passkey;
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            memcpy(evt.gap.authCmpl.bda, param->ble_security.auth_cmpl.bd_addr, 6);
            evt.gap.authCmpl.success = param->ble_security.auth_cmpl.success;
            evt.gap.authCmpl.failReason = param->ble_security.auth_cmpl.fail_reason;
            break;

        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            memcpy(evt.gap.secReq.bda, param->ble_security.ble_req.bd_addr, 6);
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            evt.gap.connParams.status = param->update_conn_params.status;
            memcpy(evt.gap.connParams.bda, param->update_conn_params.bda, 6);
            evt.gap.connParams.conn_int = param->update_conn_params.conn_int;
            evt.gap.connParams.latency = param->update_conn_params.latency;
            evt.gap.connParams.timeout = param->update_conn_params.timeout;
            break;

        default:
            break;
    }

    return evt;
}

/**
 * Helper to create GATTC event from callback parameters
 */
inline BleInternalEvent makeGattcEvent(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattcIf,
                                        esp_ble_gattc_cb_param_t* param)
{
    BleInternalEvent evt = {};
    evt.type = BleInternalEvent::Type::GATTC;
    evt.gattc.event = event;
    evt.gattc.gattcIf = gattcIf;

    switch (event) {
        case ESP_GATTC_REG_EVT:
            evt.gattc.reg.status = param->reg.status;
            evt.gattc.reg.appId = param->reg.app_id;
            break;

        case ESP_GATTC_OPEN_EVT:
            evt.gattc.open.status = param->open.status;
            evt.gattc.open.connId = param->open.conn_id;
            memcpy(evt.gattc.open.remoteBda, param->open.remote_bda, 6);
            break;

        case ESP_GATTC_CLOSE_EVT:
            evt.gattc.close.connId = param->close.conn_id;
            evt.gattc.close.reason = param->close.reason;
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            evt.gattc.cfgMtu.status = param->cfg_mtu.status;
            evt.gattc.cfgMtu.connId = param->cfg_mtu.conn_id;
            evt.gattc.cfgMtu.mtu = param->cfg_mtu.mtu;
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            evt.gattc.searchRes.connId = param->search_res.conn_id;
            evt.gattc.searchRes.srvcId = param->search_res.srvc_id;
            evt.gattc.searchRes.startHandle = param->search_res.start_handle;
            evt.gattc.searchRes.endHandle = param->search_res.end_handle;
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            evt.gattc.searchCmpl.status = param->search_cmpl.status;
            evt.gattc.searchCmpl.connId = param->search_cmpl.conn_id;
            break;

        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT:
            evt.gattc.read.status = param->read.status;
            evt.gattc.read.connId = param->read.conn_id;
            evt.gattc.read.handle = param->read.handle;
            evt.gattc.read.valueLen = param->read.value_len;
            if (param->read.value_len > 0 && param->read.value_len <= sizeof(evt.gattc.read.value)) {
                memcpy(evt.gattc.read.value, param->read.value, param->read.value_len);
            }
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            evt.gattc.regForNotify.status = param->reg_for_notify.status;
            evt.gattc.regForNotify.handle = param->reg_for_notify.handle;
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
            evt.gattc.write.status = param->write.status;
            evt.gattc.write.connId = param->write.conn_id;
            evt.gattc.write.handle = param->write.handle;
            break;

        case ESP_GATTC_NOTIFY_EVT:
            evt.gattc.notify.connId = param->notify.conn_id;
            evt.gattc.notify.handle = param->notify.handle;
            evt.gattc.notify.valueLen = param->notify.value_len;
            evt.gattc.notify.isNotify = param->notify.is_notify;
            if (param->notify.value_len > 0 && param->notify.value_len <= sizeof(evt.gattc.notify.value)) {
                memcpy(evt.gattc.notify.value, param->notify.value, param->notify.value_len);
            }
            break;

        default:
            break;
    }

    return evt;
}
