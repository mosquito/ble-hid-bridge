#pragma once

#include <stdint.h>
#include "config.h"
#include <stddef.h>

/**
 * HID Report Map Parser
 *
 * Parses HID Report Descriptor to extract report formats dynamically.
 * Works with any standard HID mouse/keyboard.
 */

namespace HidParser {

// Field types we care about
enum class FieldType : uint8_t {
    UNKNOWN = 0,
    BUTTONS,    // Mouse/keyboard buttons
    X,          // Mouse X movement
    Y,          // Mouse Y movement
    WHEEL,      // Vertical scroll
    HWHEEL,     // Horizontal scroll
    MODIFIER,   // Keyboard modifiers
    KEYCODE,    // Keyboard keycodes
    CONSUMER,   // Consumer control (media keys, volume)
};

// Single field in a report
struct Field {
    FieldType type;
    uint8_t bitOffset;   // Offset in bits from start of report
    uint8_t bitSize;     // Size in bits
    bool isSigned;       // true for relative values (X, Y, wheel)
};

// Parsed report format
struct ReportFormat {
    uint8_t reportId;
    uint8_t totalBits;       // Total size in bits
    uint8_t fieldCount;
    Field fields[HID_MAX_FIELDS_PER_REPORT];         // Max 8 fields per report

    bool isKeyboard() const;
    bool isMouse() const;
    bool isConsumer() const;
    uint8_t totalBytes() const { return (totalBits + 7) / 8; }
};

// Parser state - holds all parsed reports for a device
struct ParsedDevice {
    ReportFormat reports[HID_MAX_REPORTS_PER_DEVICE];
    uint8_t reportCount;

    const ReportFormat* findReport(uint8_t reportId) const;
    const ReportFormat* findMouseReport() const;
    const ReportFormat* findKeyboardReport() const;
    const ReportFormat* findConsumerReport() const;
};

/**
 * Parse HID Report Map descriptor
 * @param data Raw descriptor data
 * @param len Data length
 * @param out Output parsed device info
 * @return true if parsing succeeded
 */
bool parse(const uint8_t* data, size_t len, ParsedDevice* out);

/**
 * Decode a field value from report data
 * @param data Report data (without report ID)
 * @param field Field descriptor
 * @return Decoded value (sign-extended if signed)
 */
int32_t decodeField(const uint8_t* data, const Field& field);

} // namespace HidParser
