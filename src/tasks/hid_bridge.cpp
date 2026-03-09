#include "hid_bridge.h"
#include "config.h"
#include "usb_hid_api.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_coexist.h"
#include "cJSON.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

static const char* TAG = "hid";

// Diagnostic counters (cumulative, never reset)
static uint32_t s_notify_enqueued = 0;
static uint32_t s_notify_dropped = 0;  // buffer full
static uint32_t s_notify_processed = 0;

// BT frame timing (inter-notify gap tracking)
static int64_t  s_bt_last_notify_us = 0;
static int64_t  s_bt_frame_sum_us = 0;
static uint32_t s_bt_frame_count = 0;

// Single-core ring buffer for NOTIFY hot path (BTC callback → run(), both Core 1)
static NotifyEvent s_notify_buf[HidBridge::NOTIFY_BUF_SIZE];
static size_t s_notify_head = 0;
static size_t s_notify_tail = 0;
static size_t s_notify_count = 0;

static bool notifyPush(const NotifyEvent& evt)
{
    if (s_notify_count >= HidBridge::NOTIFY_BUF_SIZE) return false;
    s_notify_buf[s_notify_head] = evt;
    s_notify_head = (s_notify_head + 1) % HidBridge::NOTIFY_BUF_SIZE;
    s_notify_count++;
    return true;
}

static bool notifyPop(NotifyEvent& evt)
{
    if (s_notify_count == 0) return false;
    evt = s_notify_buf[s_notify_tail];
    s_notify_tail = (s_notify_tail + 1) % HidBridge::NOTIFY_BUF_SIZE;
    s_notify_count--;
    return true;
}

// Singleton instance for callbacks
HidBridge* HidBridge::instance_ = nullptr;

HidBridge::HidBridge(Storage* storage, QueueHandle_t eventQueue)
    : storage_(storage)
    , remap_mgr_(storage)
    , event_queue_(eventQueue)
{
    instance_ = this;

    // Create command queue early so getCmdQueue() works before init()
    cmd_queue_ = xQueueCreateStatic(
        CMD_QUEUE_SIZE,
        sizeof(BleCmdMsg),
        cmd_queue_storage_,
        &cmd_queue_buffer_
    );

    // Create internal event queue for non-NOTIFY events
    internal_queue_ = xQueueCreateStatic(
        INTERNAL_QUEUE_SIZE,
        sizeof(BleInternalEvent),
        internal_queue_storage_,
        &internal_queue_buffer_
    );

    // Create mutex for found devices array
    found_mutex_ = xSemaphoreCreateMutexStatic(&found_mutex_buffer_);

    // Reset all connections
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conns_[i].reset();
    }
}

void HidBridge::init()
{
    // Save task handle for xTaskNotifyGive() from BTC callback
    task_handle_ = xTaskGetCurrentTaskHandle();

    // Initialize USB HID first
    UsbHid::init();

    // Load key remaps from NVS
    remap_mgr_.loadAll();

    // Then BLE
    scanning_ = false;
    initBle();

    // Schedule first auto-reconnect scan after initial scan finishes
    next_reconnect_ = esp_timer_get_time() + (BLE_RECONNECT_SEC * US_PER_SEC);

    LOG(TAG, "HID Bridge started on core %d", xPortGetCoreID());
}

void HidBridge::initBle()
{
    LOG(TAG, "Initializing BLE...");

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Prefer BLE over WiFi when both contend for the radio
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gapCallback));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattcCallback));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    // Security configuration - set static params, dynamic ones via applySecurityLevel()
    uint8_t key_size = BLE_KEY_SIZE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support));

    // Apply default security level (MEDIUM = SC + Bond, Just Works)
    applySecurityLevel(SecurityLevel::MEDIUM);

    // Initial scan to find and auto-connect bonded devices
    startScan();

    initialized_ = true;
    LOG(TAG, "BLE initialized");
}

void HidBridge::startScan()
{
    // Schedule scan to start (will be picked up in run())
    next_scan_time_ = esp_timer_get_time();
}

void HidBridge::doStartScan()
{
    // Don't start scan if we have a pending connection
    if (pending_connect_.active) {
        DBG(TAG, "Skip scan start - connection pending");
        return;
    }

    // Don't start if already scanning
    if (scanning_) {
        return;
    }

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = BLE_SCAN_INTERVAL,
        .scan_window = BLE_SCAN_WINDOW,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    esp_ble_gap_set_scan_params(&scan_params);
}

void HidBridge::run()
{
    // Hot path first: drain NOTIFY queue (BTC callback -> Core 1)
    processNotifyEvents();

    // Drain buffered USB reports (up to 4 per tick; processOne() enforces 1ms pacing)
    for (int i = 0; i < 4; i++) {
        if (!UsbHid::processOne()) break;
    }

    // Process internal BLE events (GAP/GATTC except NOTIFY)
    processInternalEvents();

    // Process external commands
    processCommands();

    // Handle security retry if pending
    if (pending_connect_.active && pending_connect_.retryCount > 0) {
        // Check if connection slot is free (previous connect failed)
        int slot = findSlotByAddr(pending_connect_.address);
        if (slot < 0) {
            // Slot is free, retry with new security level
            retryConnectWithLowerSecurity();
        }
    }

    // Connection timeout - abandon if stuck
    if (pending_connect_.active && pending_connect_.retryCount == 0) {
        int64_t elapsed_s = (esp_timer_get_time() - pending_connect_.startTime) / US_PER_SEC;
        if (elapsed_s >= BLE_CONNECT_TIMEOUT_SEC) {
            ERR(TAG, "Connection timeout: %s (%llds)", pending_connect_.address, elapsed_s);
            int slot = findSlotByAddr(pending_connect_.address);
            if (slot >= 0 && conns_[slot].isActive()) {
                esp_ble_gattc_close(gattc_if_, conns_[slot].connId());
            }
            if (pair_state_ == PairState::CONNECTING) {
                pair_state_ = PairState::IDLE;
            }
            pending_connect_.active = false;
            startScan();
        }
    }

    // Check if it's time to start a new scan
    if (!scanning_ && next_scan_time_ > 0 && esp_timer_get_time() >= next_scan_time_) {
        next_scan_time_ = 0;
        doStartScan();
    }

    // SCAN_PAIR timeout
    if (pair_state_ != PairState::IDLE) {
        int64_t elapsed_s = (esp_timer_get_time() - pair_start_us_) / US_PER_SEC;
        if (elapsed_s >= BLE_PAIR_TIMEOUT_SEC) {
            LOG(TAG, "Pair timeout after %llds", elapsed_s);
            pair_state_ = PairState::IDLE;
        }
    }

    // Auto-reconnect: periodically try to connect disconnected saved devices
    if (esp_timer_get_time() >= next_reconnect_) {
        next_reconnect_ = esp_timer_get_time() + (BLE_RECONNECT_SEC * US_PER_SEC);
        if (!scanning_ && !pending_connect_.active) {
            reconnectNext();
        }
    }

    // Cleanup old found devices periodically
    cleanupFoundDevices();

    // Stepped connection parameter negotiation
    if (conn_params_retry_.retryAt != 0 &&
        esp_timer_get_time() >= conn_params_retry_.retryAt) {
        conn_params_retry_.retryAt = 0;

        // Step 1: prefer range, Step 2: wider fallback
        uint16_t maxInt = (conn_params_retry_.step == 1)
            ? BLE_CONN_PREFER_MAX : (BLE_CONN_PREFER_MAX + BLE_CONN_MAX_INTERVAL) / 2;
        LOG(TAG, "Conn params step %d: requesting %d-%d (%.1f-%.1fms)",
            conn_params_retry_.step,
            BLE_CONN_MIN_INTERVAL, maxInt,
            BLE_CONN_MIN_INTERVAL * 1.25f, maxInt * 1.25f);

        esp_ble_conn_update_params_t params = {};
        memcpy(params.bda, conn_params_retry_.bda, sizeof(esp_bd_addr_t));
        params.min_int = BLE_CONN_MIN_INTERVAL;
        params.max_int = maxInt;
        params.latency = BLE_CONN_LATENCY;
        params.timeout = BLE_CONN_TIMEOUT;
        esp_ble_gap_update_conn_params(&params);
    }

    // Sleep until BTC callback wakes us via xTaskNotifyGive(), or 1ms timeout
    // for housekeeping (scan, cleanup). Instant wakeup on NOTIFY.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
}

void HidBridge::processNotifyEvents()
{
    NotifyEvent evt;
    while (notifyPop(evt)) {
        s_notify_processed++;
        onNotifyDirect(evt.connId, evt.handle, evt.data, evt.len);
    }
}

void HidBridge::processInternalEvents()
{
    BleInternalEvent evt;

    // Process all queued events
    while (xQueueReceive(internal_queue_, &evt, 0) == pdTRUE) {
        if (evt.type == BleInternalEvent::Type::GAP) {
            DBG(TAG, "Processing GAP event: %d", evt.gap.event);
            handleGapEvent(evt.gap);
        } else {
            DBG(TAG, "Processing GATTC event: %d", evt.gattc.event);
            handleGattcEvent(evt.gattc);
        }
    }
}

void HidBridge::onNotifyDirect(uint16_t connId, uint16_t handle,
                                const uint8_t* data, uint16_t len)
{
    int slot = findSlotByConnId(connId);
    if (slot >= 0 && conns_[slot].isReady()) {
        conns_[slot].processNotify(handle, data, len);
    }
}

void HidBridge::handleGapEvent(const GapEventData& evt)
{
    switch (evt.event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            onScanParamSet(evt);
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (evt.scanStartCmpl.status == ESP_BT_STATUS_SUCCESS) {
                LOG(TAG, "BLE scan started");
            } else {
                ERR(TAG, "BLE scan start failed: %d", evt.scanStartCmpl.status);
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            onScanResult(evt);
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            onScanStop();
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
        case ESP_GAP_BLE_NC_REQ_EVT:
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            onSecurityEvent(evt);
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            onConnParamsUpdate(evt);
            break;

        default:
            DBG(TAG, "GAP event: %d", evt.event);
            break;
    }
}

void HidBridge::onScanParamSet(const GapEventData& evt)
{
    DBG(TAG, "SCAN_PARAM_SET status=%d", evt.scanParamCmpl.status);
    if (evt.scanParamCmpl.status == ESP_BT_STATUS_SUCCESS) {
        esp_ble_gap_start_scanning(BLE_SCAN_DURATION_SEC);
        scanning_ = true;
        DBG(TAG, "Scan started (%d sec)", BLE_SCAN_DURATION_SEC);
    } else {
        ERR(TAG, "Scan params failed: %d", evt.scanParamCmpl.status);
        // Schedule retry
        next_scan_time_ = esp_timer_get_time() + (BLE_SCAN_PAUSE_SEC * US_PER_SEC);
    }
}

void HidBridge::onScanResult(const GapEventData& evt)
{
    if (evt.scanResult.searchEvt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        // Scan completed - no automatic restart (manual scan only)
        if (scanning_) {
            scanning_ = false;
            DBG(TAG, "Scan complete");
        }
        if (pair_state_ == PairState::SCANNING) {
            pair_state_ = PairState::IDLE;
            LOG(TAG, "Pair scan: no device found");
        }
        return;
    }

    if (evt.scanResult.searchEvt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    // Parse device name
    uint8_t* advName = nullptr;
    uint8_t advNameLen = 0;
    advName = esp_ble_resolve_adv_data(
        const_cast<uint8_t*>(evt.scanResult.bleAdv),
        ESP_BLE_AD_TYPE_NAME_CMPL, &advNameLen);
    if (!advName) {
        advName = esp_ble_resolve_adv_data(
            const_cast<uint8_t*>(evt.scanResult.bleAdv),
            ESP_BLE_AD_TYPE_NAME_SHORT, &advNameLen);
    }

    char name[32] = {0};
    if (advName && advNameLen > 0) {
        int len = advNameLen < 31 ? advNameLen : 31;
        memcpy(name, advName, len);
    }

    if (!name[0]) return;

    char addr[18];
    bdaToStr(evt.scanResult.bda, addr);

    // Auto-connect known devices (bonded via Bluedroid OR saved via Web UI)
    uint8_t bondedAddrType = 0xFF;
    bool bonded = isDeviceBonded(evt.scanResult.bda, &bondedAddrType);
    bool saved = !bonded && isDeviceSaved(addr);

    if (bonded || saved) {
        if (findSlotByBda(evt.scanResult.bda) < 0 && !pending_connect_.active) {
            // Bonded: use identity addr type from Bluedroid (avoids type mismatch hang)
            // Saved: use scan addr type (no bond info available)
            uint8_t connectType = bonded ? bondedAddrType : (uint8_t)evt.scanResult.addrType;
            LOG(TAG, "Known device found: %s [%s] - auto-connecting (type=%d, %s)",
                name, addr, connectType, bonded ? "bonded" : "saved");
            esp_ble_gap_stop_scanning();
            handleConnect(addr, name, connectType);
        }
        return;
    }

#if BLE_FILTER_HID_DEVICES
    // Non-bonded: filter by HID service UUID
    uint8_t* svcData = esp_ble_resolve_adv_data(
        const_cast<uint8_t*>(evt.scanResult.bleAdv),
        ESP_BLE_AD_TYPE_16SRV_CMPL, &advNameLen);
    if (!svcData) {
        svcData = esp_ble_resolve_adv_data(
            const_cast<uint8_t*>(evt.scanResult.bleAdv),
            ESP_BLE_AD_TYPE_16SRV_PART, &advNameLen);
    }

    bool isHid = false;
    if (svcData && advNameLen >= 2) {
        for (int i = 0; i < advNameLen; i += 2) {
            uint16_t uuid = svcData[i] | (svcData[i + 1] << 8);
            if (uuid == HID_SVC_UUID) { isHid = true; break; }
        }
    }
    if (!isHid) return;
#endif

    // Update found devices list (for Web UI)
    updateFoundDevice(addr, name, evt.scanResult.addrType, evt.scanResult.rssi);

    if (pair_state_ == PairState::SCANNING) {
        // Headless pairing: auto-connect first found HID device
        pair_state_ = PairState::CONNECTING;
        LOG(TAG, "Pair scan: auto-connecting %s [%s]", name, addr);
        handleConnect(addr, name, evt.scanResult.addrType);
        esp_ble_gap_stop_scanning();
    } else {
        // New device - just notify Web UI
        sendEvent(BleEvent::DEVICE_FOUND, addr, name, evt.scanResult.rssi);
    }
}

void HidBridge::onScanStop()
{
    if (scanning_) {
        scanning_ = false;
        DBG(TAG, "Scan stopped");
    }
}

void HidBridge::onSecurityEvent(const GapEventData& evt)
{
    char addr[18];

    switch (evt.event) {
        case ESP_GAP_BLE_SEC_REQ_EVT:
            bdaToStr(evt.secReq.bda, addr);
            // For NONE security level, decline security request to avoid SMP
            if (pending_connect_.active && pending_connect_.secLevel == SecurityLevel::NONE) {
                DBG(TAG, "SEC_REQ from %s - declining (NONE security)", addr);
                esp_ble_gap_security_rsp(const_cast<uint8_t*>(evt.secReq.bda), false);
            } else {
                DBG(TAG, "SEC_REQ from %s - accepting", addr);
                esp_ble_gap_security_rsp(const_cast<uint8_t*>(evt.secReq.bda), true);
            }
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            bdaToStr(evt.keyNotif.bda, addr);
            DBG(TAG, "NC_REQ from %s passkey=%lu - auto-confirming", addr, evt.keyNotif.passkey);
            esp_ble_confirm_reply(const_cast<uint8_t*>(evt.keyNotif.bda), true);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            bdaToStr(evt.authCmpl.bda, addr);
            if (evt.authCmpl.success) {
                LOG(TAG, "Auth complete: %s", addr);
                // Save working security level for this device
                if (pending_connect_.active && strcmp(pending_connect_.address, addr) == 0) {
                    saveDeviceSecurityLevel(addr, pending_connect_.secLevel);
                    pending_connect_.active = false;
                }
            } else {
                ERR(TAG, "Auth failed: %s reason=0x%x", addr, evt.authCmpl.failReason);

                // Remove failed bond
                esp_ble_remove_bond_device(const_cast<uint8_t*>(evt.authCmpl.bda));

                // Retry with lower security if this is our pending connection
                if (pending_connect_.active && strcmp(pending_connect_.address, addr) == 0) {
                    if (pending_connect_.secLevel < SecurityLevel::NONE) {
                        pending_connect_.secLevel = (SecurityLevel)((int)pending_connect_.secLevel + 1);
                        pending_connect_.retryCount++;
                        LOG(TAG, "Will retry #%d with security=%d",
                            pending_connect_.retryCount, (int)pending_connect_.secLevel);
                        // Schedule retry - will be picked up in run() loop
                    } else {
                        ERR(TAG, "All security levels failed for %s", addr);
                        sendEvent(BleEvent::CONNECT_FAILED, addr);
                        pending_connect_.active = false;
                        if (pair_state_ == PairState::CONNECTING) {
                            pair_state_ = PairState::IDLE;
                        }
                    }
                }
            }
            break;

        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            bdaToStr(evt.secReq.bda, addr);
            DBG(TAG, "PASSKEY_REQ from %s - sending 0", addr);
            esp_ble_passkey_reply(const_cast<uint8_t*>(evt.secReq.bda), true, 0);
            break;

        default:
            DBG(TAG, "GAP security event: %d", evt.event);
            break;
    }
}

void HidBridge::onConnParamsUpdate(const GapEventData& evt)
{
    auto& p = evt.connParams;

    // If interval is fast enough, we're happy — regardless of status
    if (p.conn_int <= BLE_CONN_PREFER_MAX) {
        if (p.status == ESP_BT_STATUS_SUCCESS) {
            LOG(TAG, "Conn params OK: interval=%d (%.1fms) latency=%d timeout=%d",
                p.conn_int, p.conn_int * 1.25f, p.latency, p.timeout);
        }
        conn_params_retry_ = {};
        return;
    }

    if (p.status == ESP_BT_STATUS_SUCCESS) {
        LOG(TAG, "Conn params: interval=%d (%.1fms) — slower than preferred %d",
            p.conn_int, p.conn_int * 1.25f, BLE_CONN_PREFER_MAX);
    }

    // Stepped negotiation: try progressively wider ranges
    // Step 0 → 1: prefer range, delay 2s
    // Step 1 → 2: wider fallback, delay 3s
    // Step 2 → 3: give up
    int nextStep = conn_params_retry_.step + 1;

    if (nextStep <= 2) {
        WARN(TAG, "Conn params: interval=%d (%.1fms), negotiation step %d",
            p.conn_int, p.conn_int * 1.25f, nextStep);
        memcpy(conn_params_retry_.bda, p.bda, sizeof(esp_bd_addr_t));
        int64_t delay = (nextStep == 1) ? 2000 : 3000;
        conn_params_retry_.retryAt = esp_timer_get_time() + (delay * US_PER_MS);
        conn_params_retry_.step = nextStep;
    } else {
        LOG(TAG, "Conn params: accepting interval=%d (%.1fms)",
            p.conn_int, p.conn_int * 1.25f);
        conn_params_retry_ = {};
    }
}

void HidBridge::handleGattcEvent(const GattcEventData& evt)
{
    // Handle registration event specially
    if (evt.event == ESP_GATTC_REG_EVT) {
        onGattcReg(evt);
        return;
    }

    // Route to appropriate connection by conn_id
    int slot = -1;
    uint16_t connId = 0;

    switch (evt.event) {
        case ESP_GATTC_OPEN_EVT:
            slot = findSlotByBda(evt.open.remoteBda);
            break;

        case ESP_GATTC_CLOSE_EVT:
            connId = evt.close.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            connId = evt.cfgMtu.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            connId = evt.searchRes.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            connId = evt.searchCmpl.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT:
            connId = evt.read.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            connId = evt.write.connId;
            slot = findSlotByConnId(connId);
            break;

        case ESP_GATTC_NOTIFY_EVT:
            connId = evt.notify.connId;
            slot = findSlotByConnId(connId);
            break;

        default:
            DBG(TAG, "GATTC event: %d", evt.event);
            return;
    }

    if (slot >= 0) {
        // Capture state before event may reset the connection
        bool isPendingSlot = pending_connect_.active &&
                             strcmp(conns_[slot].address(), pending_connect_.address) == 0;
        bool wasActive = conns_[slot].isActive();
        bool wasReady = conns_[slot].isReady();

        conns_[slot].handleGattcEvent(evt, event_queue_);

        // Save device when connection becomes READY
        if (!wasReady && conns_[slot].isReady()) {
            DBG(TAG, "Connection slot %d became READY", slot);
            onConnectionReady(slot);
        }

        // Clear pending_connect if this slot was reset (OPEN fail or CLOSE)
        // and no security retry is pending
        if (isPendingSlot && wasActive && !conns_[slot].isActive() &&
            pending_connect_.retryCount == 0) {
            LOG(TAG, "Pending connection cleared (slot reset)");
            pending_connect_.active = false;
        }
    }
}

void HidBridge::onGattcReg(const GattcEventData& evt)
{
    DBG(TAG, "GATTC_REG status=%d app_id=%d", evt.reg.status, evt.reg.appId);
    if (evt.reg.status == ESP_GATT_OK) {
        gattc_if_ = evt.gattcIf;
        LOG(TAG, "GATTC registered, if=%d", evt.gattcIf);
    } else {
        ERR(TAG, "GATTC register failed: %d", evt.reg.status);
    }
}

void HidBridge::processCommands()
{
    BleCmdMsg cmd;
    while (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
        LOG(TAG, "Command: %s", bleCmdName(cmd.cmd));

        switch (cmd.cmd) {
            case BleCmd::CONNECT:
                handleConnect(cmd.address, cmd.name);
                break;
            case BleCmd::DISCONNECT:
                handleDisconnect(cmd.address, cmd.deviceId);
                break;
            case BleCmd::SCAN:
                startScan();
                break;
            case BleCmd::SCAN_PAIR:
                pair_state_ = PairState::SCANNING;
                pair_start_us_ = esp_timer_get_time();
                startScan();
                break;

            case BleCmd::SET_REMAPS: {
                // Convert RemapCmdEntry to RemapEntry
                RemapEntry entries[BLE_CMD_MAX_REMAPS];
                for (int i = 0; i < cmd.remapCount && i < BLE_CMD_MAX_REMAPS; i++) {
                    entries[i].from = cmd.remapEntries[i].from;
                    entries[i].to = cmd.remapEntries[i].to;
                }

                if (cmd.remapCount > 0) {
                    remap_mgr_.setRemaps(cmd.address, entries, cmd.remapCount);
                } else {
                    remap_mgr_.clearRemaps(cmd.address);
                }

                // Re-assign table to active connection if any
                int slot = findSlotByAddr(cmd.address);
                if (slot >= 0 && conns_[slot].isReady()) {
                    conns_[slot].setRemapTable(remap_mgr_.getTable(cmd.address));
                }
                break;
            }
        }
    }
}

void HidBridge::handleConnect(const char* address, const char* name, uint8_t addrType)
{
    int slot = findFreeSlot();
    if (slot < 0) {
        ERR(TAG, "No free connection slots");
        sendEvent(BleEvent::CONNECT_FAILED, address);
        return;
    }

    esp_bd_addr_t bda;
    strToBda(address, bda);

    // Use provided addrType or lookup from found devices
    if (addrType == 0xFF) {
        addrType = getFoundDeviceAddrType(bda);
    }

    // Load security level from NVS or use default
    SecurityLevel secLevel = getDeviceSecurityLevel(address);
    applySecurityLevel(secLevel);

    // Save pending connection for security retry
    strncpy(pending_connect_.address, address, sizeof(pending_connect_.address) - 1);
    strncpy(pending_connect_.name, name, sizeof(pending_connect_.name) - 1);
    pending_connect_.addrType = addrType;
    pending_connect_.secLevel = secLevel;
    pending_connect_.retryCount = 0;
    pending_connect_.active = true;
    pending_connect_.startTime = esp_timer_get_time();

    LOG(TAG, "Connecting to %s [%s] (slot %d, addrType=%d, secLevel=%d)",
        name, address, slot, addrType, (int)secLevel);

    // Set preferred connection params BEFORE opening — controller picks from this range
    esp_ble_gap_set_prefer_conn_params(bda,
        BLE_CONN_MIN_INTERVAL, BLE_CONN_PREFER_MAX,
        BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);

    conns_[slot].startConnect(gattc_if_, bda, addrType, address, name, secLevel);
    esp_err_t err = esp_ble_gattc_open(gattc_if_, bda, (esp_ble_addr_type_t)addrType, true);
    if (err != ESP_OK) {
        ERR(TAG, "GATTC open failed: %s", esp_err_to_name(err));
        conns_[slot].reset();
        pending_connect_.active = false;
        if (pair_state_ != PairState::IDLE) {
            pair_state_ = PairState::IDLE;
        }
        sendEvent(BleEvent::CONNECT_FAILED, address);
    }
}

void HidBridge::handleDisconnect(const char* address, int8_t deviceId)
{
    // Disconnect by address if provided
    if (address[0] != '\0') {
        int slot = findSlotByAddr(address);
        if (slot >= 0 && conns_[slot].isActive()) {
            esp_ble_gattc_close(gattc_if_, conns_[slot].connId());
        }
        return;
    }

    // No address — disconnect all
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns_[i].isActive()) {
            esp_ble_gattc_close(gattc_if_, conns_[i].connId());
        }
    }
}

int HidBridge::connectionCount() const
{
    int count = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns_[i].isActive()) count++;
    }
    return count;
}

void HidBridge::onConnectionReady(int slot)
{
    DBG(TAG, "onConnectionReady: slot=%d", slot);
    if (slot < 0 || slot >= MAX_CONNECTIONS) return;
    if (!conns_[slot].isReady()) {
        DBG(TAG, "onConnectionReady: slot %d not ready", slot);
        return;
    }

    // Clear pending connect state
    if (pending_connect_.active &&
        strcmp(pending_connect_.address, conns_[slot].address()) == 0) {
        pending_connect_.active = false;
        if (pair_state_ == PairState::CONNECTING) {
            pair_state_ = PairState::IDLE;
            LOG(TAG, "Pair complete: %s", conns_[slot].name());
        }
    }

    // Assign remap table for this device
    const DeviceRemapTable* table = remap_mgr_.getTable(conns_[slot].address());
    conns_[slot].setRemapTable(table);
    if (table) {
        LOG(TAG, "Remap table assigned: %s (%d entries)", conns_[slot].address(), table->count);
    }

    LOG(TAG, "Connection ready: %s [%s]", conns_[slot].name(), conns_[slot].address());

    // Resume scanning for other bonded devices
    startScan();
}

// Security level management

void HidBridge::applySecurityLevel(SecurityLevel level)
{
    esp_ble_auth_req_t auth;
    esp_ble_io_cap_t io;

    switch (level) {
        case SecurityLevel::HIGH:
            auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
            io = ESP_IO_CAP_IO;  // DisplayYesNo for numeric comparison
            break;
        case SecurityLevel::MEDIUM:
            auth = ESP_LE_AUTH_REQ_SC_BOND;
            io = ESP_IO_CAP_NONE;  // Just Works
            break;
        case SecurityLevel::LOW:
            auth = ESP_LE_AUTH_BOND;
            io = ESP_IO_CAP_NONE;  // Legacy bonding
            break;
        case SecurityLevel::NONE:
        default:
            auth = ESP_LE_AUTH_NO_BOND;
            io = ESP_IO_CAP_NONE;  // No bonding at all
            break;
    }

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io));
    DBG(TAG, "Security level set to %d (auth=0x%x io=%d)", (int)level, auth, io);
}

SecurityLevel HidBridge::getDeviceSecurityLevel(const char* address)
{
    std::string json = storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(json.c_str());
    if (!saved) return SecurityLevel::MEDIUM;

    SecurityLevel result = SecurityLevel::MEDIUM;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, saved) {
        cJSON* addr = cJSON_GetObjectItem(item, "address");
        if (addr && cJSON_IsString(addr) && strcmp(addr->valuestring, address) == 0) {
            cJSON* sec = cJSON_GetObjectItem(item, "security");
            if (sec && cJSON_IsNumber(sec)) {
                result = (SecurityLevel)sec->valueint;
                DBG(TAG, "Loaded security=%d for %s", (int)result, address);
            }
            break;
        }
    }
    cJSON_Delete(saved);
    return result;
}

void HidBridge::saveDeviceSecurityLevel(const char* address, SecurityLevel level)
{
    std::string json = storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(json.c_str());
    if (!saved) saved = cJSON_CreateArray();

    cJSON* item = nullptr;
    bool found = false;
    cJSON_ArrayForEach(item, saved) {
        cJSON* addr = cJSON_GetObjectItem(item, "address");
        if (addr && cJSON_IsString(addr) && strcmp(addr->valuestring, address) == 0) {
            found = true;
            cJSON* sec = cJSON_GetObjectItem(item, "security");
            if (sec) {
                cJSON_SetIntValue(sec, (int)level);
            } else {
                cJSON_AddNumberToObject(item, "security", (int)level);
            }
            break;
        }
    }

    if (!found) {
        // Device not in saved_devs yet — add it
        cJSON* newDev = cJSON_CreateObject();
        cJSON_AddStringToObject(newDev, "address", address);
        cJSON_AddStringToObject(newDev, "name", "");
        cJSON_AddNumberToObject(newDev, "security", (int)level);
        cJSON_AddItemToArray(saved, newDev);
    }

    char* newJson = cJSON_PrintUnformatted(saved);
    if (newJson) {
        storage_->set("saved_devs", std::string(newJson));
        LOG(TAG, "Saved security=%d for %s", (int)level, address);
        cJSON_free(newJson);
    }
    cJSON_Delete(saved);
}

void HidBridge::retryConnectWithLowerSecurity()
{
    if (!pending_connect_.active) return;

    LOG(TAG, "Retrying connection to %s with security=%d",
        pending_connect_.address, (int)pending_connect_.secLevel);

    applySecurityLevel(pending_connect_.secLevel);

    esp_bd_addr_t bda;
    strToBda(pending_connect_.address, bda);

    int slot = findFreeSlot();
    if (slot < 0) {
        ERR(TAG, "No free slot for retry");
        pending_connect_.active = false;
        return;
    }

    esp_ble_gap_set_prefer_conn_params(bda,
        BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
        BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);

    conns_[slot].startConnect(gattc_if_, bda, pending_connect_.addrType,
                               pending_connect_.address, pending_connect_.name,
                               pending_connect_.secLevel);
    esp_err_t err = esp_ble_gattc_open(gattc_if_, bda, (esp_ble_addr_type_t)pending_connect_.addrType, true);
    if (err != ESP_OK) {
        ERR(TAG, "GATTC open retry failed: %s", esp_err_to_name(err));
        conns_[slot].reset();
        pending_connect_.active = false;
        if (pair_state_ != PairState::IDLE) {
            pair_state_ = PairState::IDLE;
        }
    }
}

// Static callbacks - NOTIFY processed directly, others enqueued

void HidBridge::gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    if (!instance_ || !instance_->internal_queue_) return;

    BleInternalEvent evt = makeGapEvent(event, param);
    xQueueSend(instance_->internal_queue_, &evt, 0);
}

void HidBridge::gattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattcIf,
                                esp_ble_gattc_cb_param_t* param)
{
    if (!instance_) return;

    // NOTIFY events - push to single-core ring buffer, wake run() on Core 1
    if (event == ESP_GATTC_NOTIFY_EVT && param->notify.value_len > 0) {
        NotifyEvent nevt;
        nevt.connId = param->notify.conn_id;
        nevt.handle = param->notify.handle;
        nevt.len = param->notify.value_len <= sizeof(nevt.data)
                   ? param->notify.value_len : sizeof(nevt.data);
        memcpy(nevt.data, param->notify.value, nevt.len);

        if (notifyPush(nevt)) {
            s_notify_enqueued++;
            // BT frame timing: measure inter-notify gap
            int64_t now = esp_timer_get_time();
            if (s_bt_last_notify_us != 0) {
                s_bt_frame_sum_us += (now - s_bt_last_notify_us);
                s_bt_frame_count++;
            }
            s_bt_last_notify_us = now;
            if (instance_->task_handle_) {
                xTaskNotifyGive(instance_->task_handle_);
            }
        } else {
            s_notify_dropped++;
        }
        return;
    }

    // Other events - enqueue for processing in run()
    if (!instance_->internal_queue_) return;
    BleInternalEvent evt = makeGattcEvent(event, gattcIf, param);
    BaseType_t ret = xQueueSend(instance_->internal_queue_, &evt, 0);
    if (ret != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, dropped GATTC event %d", event);
    }
}

// Helpers

void HidBridge::bdaToStr(const esp_bd_addr_t bda, char* str)
{
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void HidBridge::strToBda(const char* str, esp_bd_addr_t bda)
{
    unsigned int t[6];
    sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]);
    for (int i = 0; i < 6; i++) bda[i] = t[i];
}

int HidBridge::findFreeSlot()
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!conns_[i].isActive()) return i;
    }
    return -1;
}

int HidBridge::findSlotByAddr(const char* addr)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns_[i].isActive() && strcmp(conns_[i].address(), addr) == 0) return i;
    }
    return -1;
}

int HidBridge::findSlotByConnId(uint16_t connId)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns_[i].isActive() && conns_[i].connId() == connId) return i;
    }
    return -1;
}

int HidBridge::findSlotByBda(const esp_bd_addr_t bda)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (conns_[i].isActive() && memcmp(conns_[i].bda(), bda, 6) == 0) {
            return i;
        }
    }
    return -1;
}

bool HidBridge::isDeviceBonded(const esp_bd_addr_t bda, uint8_t* outAddrType)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;

    esp_ble_bond_dev_t* devList = (esp_ble_bond_dev_t*)malloc(count * sizeof(esp_ble_bond_dev_t));
    if (!devList) return false;

    bool found = false;
    if (esp_ble_get_bond_device_list(&count, devList) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            if (memcmp(devList[i].bd_addr, bda, 6) == 0) {
                found = true;
                if (outAddrType) {
                    *outAddrType = devList[i].bd_addr_type;
                }
                break;
            }
        }
    }
    free(devList);
    return found;
}

bool HidBridge::isDeviceSaved(const char* address)
{
    std::string json = storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(json.c_str());
    if (!saved) return false;

    bool found = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, saved) {
        cJSON* addr = cJSON_GetObjectItem(item, "address");
        if (addr && cJSON_IsString(addr) && strcmp(addr->valuestring, address) == 0) {
            found = true;
            break;
        }
    }
    cJSON_Delete(saved);
    return found;
}

void HidBridge::reconnectNext()
{
    if (pending_connect_.active) return;
    if (findFreeSlot() < 0) return;

    std::string json = storage_->get("saved_devs", "[]");
    cJSON* saved = cJSON_Parse(json.c_str());
    if (!saved) return;

    char addr[18] = {};
    char name[32] = {};
    uint8_t addrType = 0xFF;
    bool found = false;

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, saved) {
        cJSON* addrJ = cJSON_GetObjectItem(item, "address");
        if (!addrJ || !cJSON_IsString(addrJ)) continue;
        if (findSlotByAddr(addrJ->valuestring) >= 0) continue;

        // Copy data before freeing JSON
        strncpy(addr, addrJ->valuestring, sizeof(addr) - 1);
        cJSON* nameJ = cJSON_GetObjectItem(item, "name");
        if (nameJ && cJSON_IsString(nameJ)) {
            strncpy(name, nameJ->valuestring, sizeof(name) - 1);
        }

        // Try to get addr type from bond list (identity address)
        esp_bd_addr_t bda;
        strToBda(addr, bda);
        uint8_t bondedType;
        if (isDeviceBonded(bda, &bondedType)) {
            addrType = bondedType;
        }

        found = true;
        break;
    }
    cJSON_Delete(saved);

    if (found) {
        DBG(TAG, "Auto-reconnect: %s [%s] (type=%d)", name, addr, addrType);
        handleConnect(addr, name, addrType);
    }
}

void HidBridge::updateFoundDevice(const char* address, const char* name, uint8_t addrType, int8_t rssi)
{
    if (xSemaphoreTake(found_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;

    int64_t now = esp_timer_get_time();

    // Look for existing device
    for (size_t i = 0; i < found_devices_count_; i++) {
        if (strcmp(found_devices_[i].address, address) == 0) {
            found_devices_[i].rssi = rssi;
            found_devices_[i].lastSeen = now;
            xSemaphoreGive(found_mutex_);
            return;
        }
    }

    // Add new device if space available
    if (found_devices_count_ < BLE_MAX_FOUND_DEVICES) {
        FoundDevice& dev = found_devices_[found_devices_count_];
        strncpy(dev.address, address, sizeof(dev.address) - 1);
        strncpy(dev.name, name, sizeof(dev.name) - 1);
        dev.addrType = addrType;
        dev.rssi = rssi;
        dev.lastSeen = now;
        found_devices_count_++;
        DBG(TAG, "Added found device: %s [%s] rssi=%d", name, address, rssi);
    }

    xSemaphoreGive(found_mutex_);
}

void HidBridge::cleanupFoundDevices()
{
    if (xSemaphoreTake(found_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;

    int64_t now = esp_timer_get_time();
    int64_t timeout = BLE_SCAN_TTL_SEC * US_PER_SEC;

    // Remove old devices (compact array)
    size_t writeIdx = 0;
    for (size_t i = 0; i < found_devices_count_; i++) {
        if ((now - found_devices_[i].lastSeen) < timeout) {
            if (writeIdx != i) {
                found_devices_[writeIdx] = found_devices_[i];
            }
            writeIdx++;
        }
    }
    found_devices_count_ = writeIdx;

    xSemaphoreGive(found_mutex_);
}

int HidBridge::findFoundDevice(const esp_bd_addr_t bda)
{
    char addr[18];
    bdaToStr(bda, addr);

    for (size_t i = 0; i < found_devices_count_; i++) {
        if (strcmp(found_devices_[i].address, addr) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint8_t HidBridge::getFoundDeviceAddrType(const esp_bd_addr_t bda)
{
    int idx = findFoundDevice(bda);
    if (idx >= 0) {
        return found_devices_[idx].addrType;
    }
    return BLE_ADDR_TYPE_RANDOM;
}

size_t HidBridge::getFoundDevices(FoundDevice* out, size_t maxCount)
{
    if (xSemaphoreTake(found_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t count = (found_devices_count_ < maxCount) ? found_devices_count_ : maxCount;
    for (size_t i = 0; i < count; i++) {
        out[i] = found_devices_[i];
    }

    xSemaphoreGive(found_mutex_);
    return count;
}

HidStats HidBridge::getStats() const
{
    HidStats s = {};
    s.notifyIn = s_notify_enqueued;
    s.notifyDrop = s_notify_dropped;
    s.notifyOut = s_notify_processed;
    s.mouseSkip = bleConnectionMouseSkips();
    UsbHid::getStats(s.usbOk, s.usbBusy, s.usbNotReady, s.usbQueueFull);
    // Frame timing
    s.btFrameSumUs = s_bt_frame_sum_us;
    s.btFrameCount = s_bt_frame_count;
    UsbHid::getFrameStats(s.usbFrameSumUs, s.usbFrameCount,
                          s.usbFrameMinUs, s.usbFrameMaxUs);
    return s;
}

void HidBridge::sendEvent(BleEvent event, const char* address, const char* name, int8_t rssi)
{
    if (!event_queue_) return;

    BleEventMsg msg = {};
    msg.event = event;
    strncpy(msg.address, address, sizeof(msg.address) - 1);
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.rssi = rssi;

    xQueueSend(event_queue_, &msg, 0);
}