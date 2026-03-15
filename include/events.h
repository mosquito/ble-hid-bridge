#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

/**
 * Queue-Based Event System for BLE-USB Bridge
 *
 * Architecture:
 *                     g_bleCmdQueue            g_hidReportQueue
 *   Web Task ────────────────────────> BLE Task ─────────────────> USB HID Task
 *      ^                                   │
 *      │        g_bleEventQueue            │
 *      └───────────────────────────────────┘
 */

// =============================================================================
// BLE Commands (Web/Button -> BLE Task)
// =============================================================================

enum class BleCmd : uint8_t {
    CONNECT,
    DISCONNECT,
    SCAN,
    SCAN_PAIR,
    SET_REMAPS
};

static constexpr int BLE_CMD_MAX_REMAPS = 32;

struct RemapCmdEntry {
    uint8_t from;
    uint16_t to;
};

struct BleCmdMsg {
    BleCmd cmd;
    char address[18];  // For CONNECT/DISCONNECT/SET_REMAPS (MAC address)
    union {
        struct {
            char name[32];     // Device name (for CONNECT)
            int8_t deviceId;   // For targeted DISCONNECT (-1 = all)
        };
        struct {
            RemapCmdEntry remapEntries[BLE_CMD_MAX_REMAPS];
            uint8_t remapCount;
            int8_t scrollScale;  // 0=no remap, nonzero=wheel multiplier (neg=invert)
        };
    };
};

// =============================================================================
// BLE Events (BLE Task -> Web Task)
// =============================================================================

enum class BleEvent : uint8_t {
    DEVICE_FOUND,
    CONNECTED,
    DISCONNECTED,
    CONNECT_FAILED
};

struct BleEventMsg {
    BleEvent event;
    char address[18];
    char name[32];
    int8_t rssi;
};

// =============================================================================
// HID Reports (BLE -> USB HID)
// =============================================================================

struct MouseReport {
    int16_t x;
    int16_t y;
    int8_t wheel;
    int8_t hwheel;
    uint8_t buttons;  // bit0=left, bit1=right, bit2=middle
};

struct KeyboardReport {
    uint8_t modifier;
    uint8_t keys[6];
};

struct ConsumerReport {
    uint16_t usage;  // Consumer control usage code (e.g. Play/Pause, Volume)
};

struct HidReportMsg {
    enum Type : uint8_t { MOUSE, KEYBOARD, CONSUMER } type;
    uint8_t deviceId;
    union {
        MouseReport mouse;
        KeyboardReport keyboard;
        ConsumerReport consumer;
    };
};

// =============================================================================
// Event name helpers (for debug logging)
// =============================================================================

inline const char* bleEventName(BleEvent e) {
    switch (e) {
        case BleEvent::DEVICE_FOUND:   return "DEVICE_FOUND";
        case BleEvent::CONNECTED:      return "CONNECTED";
        case BleEvent::DISCONNECTED:   return "DISCONNECTED";
        case BleEvent::CONNECT_FAILED: return "CONNECT_FAILED";
        default:                       return "UNKNOWN";
    }
}

inline const char* bleCmdName(BleCmd c) {
    switch (c) {
        case BleCmd::CONNECT:    return "CONNECT";
        case BleCmd::DISCONNECT: return "DISCONNECT";
        case BleCmd::SCAN:       return "SCAN";
        case BleCmd::SCAN_PAIR:  return "SCAN_PAIR";
        case BleCmd::SET_REMAPS: return "SET_REMAPS";
        default:                 return "UNKNOWN";
    }
}
