#include "usb_hid_api.h"
#include "config.h"

#include <string.h>
#include <tinyusb.h>
#include <class/hid/hid_device.h>
#include <esp_timer.h>

static const char* TAG = "usb_hid";

// =============================================================================
// USB Descriptors
// =============================================================================

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

// HID Report Descriptor - custom with 16-bit X/Y for high-DPI mice
static const uint8_t HID_REPORT_DESC[] = {
    // Mouse (Report ID 1)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)

    // Buttons (5 bits + 3 padding)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x01,        //     Input (Constant) - padding

    // X, Y (8-bit each)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data, Variable, Relative)

    // Wheel (8-bit vertical scroll)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)

    // Horizontal scroll (AC Pan)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)

    0xC0,              //   End Collection
    0xC0,              // End Collection

    // Keyboard (Report ID 2)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)

    // Modifiers (8 bits)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // Reserved byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant)

    // Keycodes (6 bytes)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xFF,        //   Usage Maximum (255)
    0x81, 0x00,        //   Input (Data, Array)

    0xC0,              // End Collection

    // Consumer Control (Report ID 3)
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x10,        //   Report Size (16)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0               // End Collection
};

// USB Configuration Descriptor
static const uint8_t USB_CONFIG_DESC[] = {
    // Configuration descriptor
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0, USB_MAX_POWER_MA),
    // HID descriptor (interface 0, no string, boot protocol, report desc len, EP IN 0x81, poll interval 1ms, bufsize 16)
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(HID_REPORT_DESC), 0x81, USB_HID_EP_BUFSIZE, 1),
};

// =============================================================================
// TinyUSB Callbacks
// =============================================================================

static volatile bool s_hid_ready = false;

extern "C" {

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return HID_REPORT_DESC;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t* buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

}  // extern "C"

static void tinyusb_event_handler(tinyusb_event_t* event, void* arg) {
    (void)arg;
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            s_hid_ready = true;
            break;
        case TINYUSB_EVENT_DETACHED:
            s_hid_ready = false;
            break;
    }
}

// =============================================================================
// Ring Buffer (plain static array, single-producer/single-consumer on Core 1)
// =============================================================================

enum class ReportType : uint8_t { MOUSE, KEYBOARD, CONSUMER };

struct BufferedReport {
    ReportType type;
    union {
        struct { uint8_t buttons; int16_t x, y; int8_t wheel; int8_t hwheel; } mouse;
        struct { uint8_t modifier; uint8_t keys[6]; } keyboard;
        struct { uint16_t usage; } consumer;
    };
};

static constexpr size_t BUF_SIZE = USB_SEND_QUEUE_SIZE;
static BufferedReport s_buf[BUF_SIZE];
static size_t s_head = 0;  // next write position
static size_t s_tail = 0;  // next read position
static size_t s_count = 0; // items in buffer

static bool bufPush(const BufferedReport& r)
{
    if (s_count >= BUF_SIZE) return false;
    s_buf[s_head] = r;
    s_head = (s_head + 1) % BUF_SIZE;
    s_count++;
    return true;
}

static bool bufPeek(BufferedReport& r)
{
    if (s_count == 0) return false;
    r = s_buf[s_tail];
    return true;
}

static void bufPop()
{
    s_tail = (s_tail + 1) % BUF_SIZE;
    s_count--;
}

// =============================================================================
// Diagnostic Counters & Timing
// =============================================================================

static uint32_t s_send_ok = 0;
static uint32_t s_send_busy = 0;
static uint32_t s_not_ready = 0;
static uint32_t s_queue_full = 0;

static constexpr int64_t FRAME_INTERVAL_US = 1000;  // 1ms USB Full Speed frame
static int64_t s_last_send_us = 0;

// Frame timing (inter-send gap tracking)
static int64_t  s_usb_prev_ok_us = 0;
static int64_t  s_usb_frame_sum_us = 0;
static uint32_t s_usb_frame_count = 0;
static uint32_t s_usb_frame_min_us = UINT32_MAX;
static uint32_t s_usb_frame_max_us = 0;

// =============================================================================
// UsbHid API Implementation
// =============================================================================

namespace UsbHid {

void init()
{
    tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = USB_TUSB_STACK_SIZE,
            .priority = RTOS_PRIORITY_MEDIUM,
            .xCoreID = RTOS_CORE_0,
        },
        .descriptor = {
            .device = nullptr,  // Use default device descriptor
            .qualifier = nullptr,
            .string = nullptr,
            .string_count = 0,
            .full_speed_config = USB_CONFIG_DESC,
            .high_speed_config = nullptr,
        },
        .event_cb = tinyusb_event_handler,
        .event_arg = nullptr,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    LOG(TAG, "USB HID initialized");
}

bool isReady()
{
    return s_hid_ready;
}

bool sendMouse(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t hwheel)
{
    // Determine step count:
    // 1) Interpolation smoothing for pure movement (no wheel)
    // 2) Overflow protection: enough steps so each sub-frame fits ±127
    int maxD = (x > 0 ? x : -x);
    int absY = (y > 0 ? y : -y);
    if (absY > maxD) maxD = absY;

    int steps = 1;
#if HID_MOUSE_INTERP
    if (maxD >= HID_MOUSE_INTERP_MIN_DELTA && wheel == 0 && hwheel == 0) {
        steps = HID_MOUSE_INTERP_STEPS;
    }
#endif
    // Always split deltas > ±127 to avoid clamping loss
    int minSteps = (maxD + 126) / 127;
    if (minSteps > steps) steps = minSteps;

    for (int i = 0; i < steps; i++) {
        BufferedReport r;
        r.type = ReportType::MOUSE;
        r.mouse.buttons = buttons;
        // Bresenham-like even distribution (exact total, no drift)
        r.mouse.x = (int16_t)(x * (i + 1) / steps - x * i / steps);
        r.mouse.y = (int16_t)(y * (i + 1) / steps - y * i / steps);
        // Wheel/hwheel only on first sub-frame
        r.mouse.wheel  = (i == 0) ? wheel  : 0;
        r.mouse.hwheel = (i == 0) ? hwheel : 0;

        if (!bufPush(r)) {
            s_queue_full++;
            return false;
        }
    }
    return true;
}

bool sendKeyboard(uint8_t modifier, const uint8_t* keys)
{
    BufferedReport r;
    r.type = ReportType::KEYBOARD;
    r.keyboard.modifier = modifier;
    memcpy(r.keyboard.keys, keys, HID_KEYBOARD_KEY_COUNT);

    if (!bufPush(r)) {
        s_queue_full++;
        return false;
    }
    return true;
}

bool sendConsumer(uint16_t usage)
{
    BufferedReport r;
    r.type = ReportType::CONSUMER;
    r.consumer.usage = usage;

    if (!bufPush(r)) {
        s_queue_full++;
        return false;
    }
    return true;
}

bool processOne()
{
    BufferedReport r;
    if (!bufPeek(r)) return false;

    // Wait for 1ms frame interval
    int64_t now = esp_timer_get_time();
    if (now - s_last_send_us < FRAME_INTERVAL_US) return false;

    if (!isReady()) {
        s_not_ready++;
        bufPop();  // Drop — nowhere to send
        return false;
    }

    bool ok = false;
    switch (r.type) {
        case ReportType::MOUSE: {
            uint8_t report[HID_MOUSE_REPORT_SIZE];
            report[0] = r.mouse.buttons;
            int16_t cx = (r.mouse.x > 127) ? 127 : (r.mouse.x < -127) ? -127 : r.mouse.x;
            int16_t cy = (r.mouse.y > 127) ? 127 : (r.mouse.y < -127) ? -127 : r.mouse.y;
            report[1] = (uint8_t)(int8_t)cx;
            report[2] = (uint8_t)(int8_t)cy;
            report[3] = (uint8_t)r.mouse.wheel;
            report[4] = (uint8_t)r.mouse.hwheel;
            ok = tud_hid_report(USB_REPORT_ID_MOUSE, report, sizeof(report));
            break;
        }
        case ReportType::KEYBOARD: {
            uint8_t report[HID_KEYBOARD_REPORT_SIZE];
            report[0] = r.keyboard.modifier;
            report[1] = 0;  // reserved
            memcpy(&report[2], r.keyboard.keys, HID_KEYBOARD_KEY_COUNT);
            ok = tud_hid_report(USB_REPORT_ID_KEYBOARD, report, sizeof(report));
            break;
        }
        case ReportType::CONSUMER: {
            uint8_t report[HID_CONSUMER_REPORT_SIZE];
            report[0] = r.consumer.usage & 0xFF;
            report[1] = (r.consumer.usage >> 8) & 0xFF;
            ok = tud_hid_report(USB_REPORT_ID_CONSUMER, report, sizeof(report));
            break;
        }
    }

    if (ok) {
        bufPop();
        s_send_ok++;
        int64_t sendTime = esp_timer_get_time();
        s_last_send_us = sendTime;
        // Frame timing: measure inter-send gap
        if (s_usb_prev_ok_us != 0) {
            uint32_t gap = (uint32_t)(sendTime - s_usb_prev_ok_us);
            s_usb_frame_sum_us += gap;
            s_usb_frame_count++;
            if (gap < s_usb_frame_min_us) s_usb_frame_min_us = gap;
            if (gap > s_usb_frame_max_us) s_usb_frame_max_us = gap;
        }
        s_usb_prev_ok_us = sendTime;
    } else {
        s_send_busy++;
        // Re-sync: count 1ms from this failed attempt, not from last success
        s_last_send_us = esp_timer_get_time();
    }
    return ok;
}

void getStats(uint32_t& ok, uint32_t& busy, uint32_t& notReady, uint32_t& queueFull)
{
    ok = s_send_ok;
    busy = s_send_busy;
    notReady = s_not_ready;
    queueFull = s_queue_full;
}

void getFrameStats(int64_t& sumUs, uint32_t& count, uint32_t& minUs, uint32_t& maxUs)
{
    sumUs = s_usb_frame_sum_us;
    count = s_usb_frame_count;
    minUs = __atomic_exchange_n(&s_usb_frame_min_us, UINT32_MAX, __ATOMIC_RELAXED);
    maxUs = __atomic_exchange_n(&s_usb_frame_max_us, 0, __ATOMIC_RELAXED);
}

}  // namespace UsbHid
