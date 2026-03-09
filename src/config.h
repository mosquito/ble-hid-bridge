/**
 * @file config.h
 * @brief Central configuration file for the project
 *
 * All configurable defines must be defined here. Values can be overridden
 * via build flags in platformio.ini (-DUSER_LED_PIN=GPIO_NUM_21).
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

//------------------------------------------------------------------------------
// Debug Logging (enables DBG() macro output)
//------------------------------------------------------------------------------

#ifndef DEV_LOGS
    #define DEV_LOGS 0
#endif

// LOG/WARN/ERR - always enabled, output to ESP-IDF console
#define LOG(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define WARN(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define ERR(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)

// DBG - only enabled with DEV_LOGS (uses LOGI to bypass default log level filter)
#if DEV_LOGS
    #define DBG(tag, fmt, ...) ESP_LOGI(tag, "[DBG] " fmt, ##__VA_ARGS__)
#else
    #define DBG(tag, fmt, ...) do {} while(0)
#endif

//------------------------------------------------------------------------------
// Unit Conversion Helpers
//------------------------------------------------------------------------------

#define US_PER_MS       1000
#define US_PER_SEC      1000000LL

//------------------------------------------------------------------------------
// Hardware
//------------------------------------------------------------------------------

#ifndef USER_LED_PIN
    #error "USER_LED_PIN not defined - add to platformio.ini build_flags"
#endif

#ifndef BUTTON_PIN
    #define BUTTON_PIN      GPIO_NUM_0  // BOOT button
#endif

//------------------------------------------------------------------------------
// Button Timings
//------------------------------------------------------------------------------

#define BUTTON_DEBOUNCE_MS          50      // Debounce filter
#define BUTTON_CLICK_WINDOW_MS      300     // Multi-click detection window
#define BUTTON_LONG_PRESS_MS        3000    // Long press threshold
#define BUTTON_EXTRA_LONG_PRESS_MS  5000    // Extra-long press (reset warning)
#define BUTTON_FACTORY_RESET_MS     25000   // Factory reset hold threshold

//------------------------------------------------------------------------------
// LED Colors (R, G, B)
//------------------------------------------------------------------------------

#define LED_TRANSITION_MS       500     // Crossfade duration between modes
#define LED_BLINK_INTERVAL_MS   100     // Blink on/off toggle period
#define LED_FADE_CYCLE_MS       2000    // Full breath cycle duration
#define LED_TICK_MS             10      // LED task loop interval

#define LED_COLOR_IDLE       10,  0, 20   // Dim purple - idle heartbeat
#define LED_COLOR_SCANNING   60, 20,  0   // Orange - BLE scanning
#define LED_COLOR_CONNECTING  0, 30, 60   // Cyan - BLE connecting
#define LED_COLOR_WIFI        0,  0, 60   // Blue - WiFi AP active
#define LED_COLOR_CONNECTED   0, 30,  0   // Green - BLE connected
#define LED_COLOR_DANGER    255,  0,  0   // Red - factory reset warning

//------------------------------------------------------------------------------
// WiFi Access Point
//------------------------------------------------------------------------------

#ifndef WIFI_AP_SSID
    #define WIFI_AP_SSID        "ESP32-S3-AP"
#endif

#ifndef WIFI_AP_PASSWORD
    #define WIFI_AP_PASSWORD    "12345678"
#endif

#define WIFI_AP_CHANNEL             1       // WiFi AP channel
#define WIFI_AP_MAX_CONNECTIONS     4       // Max simultaneous WiFi clients

//------------------------------------------------------------------------------
// BLE Configuration
//------------------------------------------------------------------------------

// Scan type: BLE_SCAN_TYPE_PASSIVE or BLE_SCAN_TYPE_ACTIVE
#define BLE_SCAN_TYPE           BLE_SCAN_TYPE_ACTIVE

// Filter to show only HID devices (0 = show all named devices, 1 = HID only)
#define BLE_FILTER_HID_DEVICES  1

// Scan schedule: scan for DURATION seconds, then pause for PAUSE seconds
#define BLE_SCAN_DURATION_SEC   15      // Scan duration in seconds
#define BLE_SCAN_PAUSE_SEC      5       // Pause between scans in seconds

// Scan timing (~20% duty cycle for power efficiency)
// Duty = Window / Interval = 0x30 / 0xF0 = 48 / 240 = 20%
#ifndef BLE_SCAN_INTERVAL
    #define BLE_SCAN_INTERVAL   0xF0    // 240 × 0.625ms = 150ms
#endif

#ifndef BLE_SCAN_WINDOW
    #define BLE_SCAN_WINDOW     0x30    // 48 × 0.625ms = 30ms
#endif

// Found device TTL (seconds) - devices not seen are removed from list
#ifndef BLE_SCAN_TTL_SEC
    #define BLE_SCAN_TTL_SEC  15
#endif

// Connection timeout (seconds) - abandon if no GATTC OPEN/AUTH in this time
#ifndef BLE_CONNECT_TIMEOUT_SEC
    #define BLE_CONNECT_TIMEOUT_SEC 15
#endif

// Auto-reconnect interval (seconds) - periodically scan to reconnect saved devices
#ifndef BLE_RECONNECT_SEC
    #define BLE_RECONNECT_SEC       5
#endif

// SCAN_PAIR total timeout (scan + connect)
#define BLE_PAIR_TIMEOUT_SEC    60

#define BLE_KEY_SIZE                16      // Encryption key size (bytes)
#define BLE_ADDR_STR_LEN            18      // "XX:XX:XX:XX:XX:XX\0"
#define BLE_MAX_DEVICE_NAME_LEN     32      // Device name buffer size
#define BLE_MAX_NOTIFY_DATA_SIZE    64      // Max notify event payload
#define BLE_MAX_REPORT_MAP_SIZE     512     // Max HID Report Map size
#define BLE_MAX_GATTC_CHARS         16      // Max characteristics per service discovery

// Connection parameters for HID devices (units of 1.25ms)
// PREFER range: used in set_prefer_conn_params BEFORE connecting — controller picks from this
// FALLBACK range: used in post-connection update retries if peripheral negotiates slower
#define BLE_CONN_MIN_INTERVAL   6       // 6 × 1.25ms = 7.5ms
#define BLE_CONN_PREFER_MAX     6       // 6 × 1.25ms = 7.5ms (fastest BLE allows)
#define BLE_CONN_MAX_INTERVAL   24      // 24 × 1.25ms = 30ms (wide fallback)
#define BLE_CONN_LATENCY        0       // No slave latency (always respond)
#define BLE_CONN_TIMEOUT        400     // 400 × 10ms = 4 sec supervision timeout

// Maximum number of found devices to track
#ifndef BLE_MAX_FOUND_DEVICES
    #define BLE_MAX_FOUND_DEVICES  16
#endif

//------------------------------------------------------------------------------
// Task Tick Rates
//------------------------------------------------------------------------------

#define BUTTON_TICK_MS              10      // Button debounce poll interval
#define BUTTON_HANDLER_POLL_MS      100     // Button handler state check interval
#define WIFI_POLL_MS                100     // WiFi task loop interval
#define WEB_SERVER_POLL_MS          50      // Web server event loop interval
#define FACTORY_RESET_DELAY_MS      3000    // Delay before reboot on factory reset
#define WIFI_TOGGLE_DELAY_MS        500     // Delay after WiFi AP start before web server

//------------------------------------------------------------------------------
// Task Stack Sizes
//------------------------------------------------------------------------------

#define LED_TASK_STACK_SIZE     4096
#define WIFI_TASK_STACK_SIZE    4096
#define WEB_TASK_STACK_SIZE     4096
#define BUTTON_TASK_STACK_SIZE  4096
#define BLE_TASK_STACK_SIZE     8192

#define USB_HID_TASK_STACK_SIZE 4096
#define USB_HID_QUEUE_SIZE  128
#define USB_KBD_QUEUE_SIZE  256
#define USB_KBD_FORWARD_TIMEOUT pdMS_TO_TICKS(500)
#define USB_MOUSE_QUEUE_SIZE 16
#define USB_MOUSE_MAX_BATCH  8
#define USB_SEND_QUEUE_SIZE  64

//------------------------------------------------------------------------------
// USB HID
//------------------------------------------------------------------------------

#define USB_REPORT_ID_MOUSE     1
#define USB_REPORT_ID_KEYBOARD  2
#define USB_REPORT_ID_CONSUMER  3

#define USB_MAX_POWER_MA            100     // USB max power draw
#define USB_HID_EP_BUFSIZE          16      // HID endpoint buffer size
#define USB_TUSB_STACK_SIZE         4096    // TinyUSB task stack

#define HID_KEYBOARD_KEY_COUNT      6       // Max simultaneous keycodes
#define HID_KEYBOARD_REPORT_SIZE    8       // modifier + reserved + 6 keys
#define HID_MOUSE_REPORT_SIZE       5       // buttons + x8 + y8 + wheel + hwheel

//------------------------------------------------------------------------------
// Mouse Interpolation
// Splits BLE mouse deltas into multiple USB sub-frames for smoother movement.
// Overflow protection (splitting deltas > ±127) is always active regardless.
//------------------------------------------------------------------------------

#ifndef HID_MOUSE_INTERP
    #define HID_MOUSE_INTERP            0       // 0 = off, 1 = on
#endif

#ifndef HID_MOUSE_INTERP_STEPS
    #define HID_MOUSE_INTERP_STEPS      3       // USB sub-frames per BLE mouse report
#endif

#ifndef HID_MOUSE_INTERP_MIN_DELTA
    #define HID_MOUSE_INTERP_MIN_DELTA  8       // Min max(|x|,|y|) to trigger interpolation
#endif
#define HID_CONSUMER_REPORT_SIZE    2       // 16-bit consumer code
#define HID_MOD_KEY_START           0xE0    // First modifier keycode
#define HID_MOD_KEY_END             0xE7    // Last modifier keycode

#define HID_MAX_FIELDS_PER_REPORT   8       // Max fields in parsed report
#define HID_MAX_REPORTS_PER_DEVICE  8       // Max reports per parsed device
#define HID_MAX_USAGES_PER_ITEM     16      // Max usages per HID parser item

//------------------------------------------------------------------------------
// Key Remapping
//------------------------------------------------------------------------------

#ifndef MAX_KEY_REMAPS
    #define MAX_KEY_REMAPS          32  // Max remap entries per device
#endif

#ifndef MAX_REMAP_DEVICES
    #define MAX_REMAP_DEVICES       4   // Max devices with remaps
#endif

//------------------------------------------------------------------------------
// Web Server
//------------------------------------------------------------------------------

#define WEB_MAX_URI_HANDLERS        16      // Max registered HTTP handlers
#define WEB_MAX_REQUEST_BODY        1024    // Max HTTP request body size

//------------------------------------------------------------------------------
// BLE Queue Timeouts
//------------------------------------------------------------------------------

#ifndef BLE_KBD_QUEUE_TIMEOUT
    #define BLE_KBD_QUEUE_TIMEOUT   pdMS_TO_TICKS(10)
#endif

//------------------------------------------------------------------------------
// HID Usage Pages and Codes (prefixed to avoid TinyUSB conflicts)
//------------------------------------------------------------------------------

#define BLE_HID_PAGE_GENERIC_DESKTOP  0x01
#define BLE_HID_PAGE_KEYBOARD         0x07
#define BLE_HID_PAGE_BUTTON           0x09
#define BLE_HID_PAGE_CONSUMER         0x0C

#define BLE_HID_CONSUMER_CONTROL      0x01
#define BLE_HID_CONSUMER_PLAY_PAUSE   0xCD
#define BLE_HID_CONSUMER_NEXT_TRACK   0xB5
#define BLE_HID_CONSUMER_PREV_TRACK   0xB6
#define BLE_HID_CONSUMER_VOLUME_UP    0xE9
#define BLE_HID_CONSUMER_VOLUME_DOWN  0xEA
#define BLE_HID_CONSUMER_MUTE         0xE2

//------------------------------------------------------------------------------
// FreeRTOS Core Affinity
//------------------------------------------------------------------------------

#define RTOS_CORE_0     0           // Networking + UI (WiFi, WebServer, LED, Button)
#define RTOS_CORE_1     1           // Realtime HID pipeline (Bluedroid, HidBridge, TinyUSB)
#define RTOS_CORE_ANY   tskNO_AFFINITY

//------------------------------------------------------------------------------
// FreeRTOS Task Priorities (0-24, higher = more priority)
//------------------------------------------------------------------------------

#define RTOS_PRIORITY_LOW       2
#define RTOS_PRIORITY_MEDIUM    5
#define RTOS_PRIORITY_HIGH      10
#define RTOS_PRIORITY_REALTIME  20

//------------------------------------------------------------------------------
// FreeRTOS Defaults
//------------------------------------------------------------------------------

#ifndef DEFAULT_TASK_STACK_SIZE
    #define DEFAULT_TASK_STACK_SIZE 4096
#endif

#ifndef DEFAULT_TASK_PRIORITY
    #define DEFAULT_TASK_PRIORITY RTOS_PRIORITY_MEDIUM
#endif
