#pragma once

#include <cstdint>

/**
 * USB HID API
 *
 * Global API for sending HID reports via TinyUSB.
 * Send functions buffer reports in a ring buffer.
 * processOne() drains one report per call if 1ms has elapsed
 * since the last send (USB Full Speed frame pacing).
 *
 * Call init() once at startup before using other functions.
 */
namespace UsbHid {

/**
 * Initialize TinyUSB with custom HID descriptor.
 * Must be called once at startup before sending reports.
 */
void init();

/**
 * Check if USB HID is ready to send reports.
 * @return true if USB is attached and HID interface is ready
 */
bool isReady();

/**
 * Buffer mouse report for sending.
 * @return true if buffered (false = buffer full, report dropped)
 */
bool sendMouse(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t hwheel = 0);

/**
 * Buffer keyboard report for sending.
 * @return true if buffered (false = buffer full, report dropped)
 */
bool sendKeyboard(uint8_t modifier, const uint8_t* keys);

/**
 * Buffer consumer control report for sending.
 * @return true if buffered (false = buffer full, report dropped)
 */
bool sendConsumer(uint16_t usage);

/**
 * Send one buffered report if 1ms has elapsed since last send.
 * Call this from the main loop at ~1ms cadence.
 * @return true if a report was sent
 */
bool processOne();

/**
 * Get diagnostic counters.
 * ok = reports sent, busy = endpoint not ready (retried next tick),
 * notReady = USB not attached, queueFull = buffer full drops
 */
void getStats(uint32_t& ok, uint32_t& busy, uint32_t& notReady, uint32_t& queueFull);

/**
 * Get frame timing stats.
 * sumUs/count are cumulative (caller computes delta).
 * minUs/maxUs are reset-on-read via atomic exchange.
 */
void getFrameStats(int64_t& sumUs, uint32_t& count, uint32_t& minUs, uint32_t& maxUs);

}  // namespace UsbHid
