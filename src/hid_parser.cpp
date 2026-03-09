#include "hid_parser.h"
#include <string.h>

namespace HidParser {

// HID item types
static const uint8_t TYPE_MAIN = 0;
static const uint8_t TYPE_GLOBAL = 1;
static const uint8_t TYPE_LOCAL = 2;

// Main item tags
static const uint8_t TAG_INPUT = 0x8;
static const uint8_t TAG_OUTPUT = 0x9;
static const uint8_t TAG_FEATURE = 0xB;
static const uint8_t TAG_COLLECTION = 0xA;
static const uint8_t TAG_END_COLLECTION = 0xC;

// Global item tags
static const uint8_t TAG_USAGE_PAGE = 0x0;
static const uint8_t TAG_LOGICAL_MIN = 0x1;
static const uint8_t TAG_LOGICAL_MAX = 0x2;
static const uint8_t TAG_REPORT_SIZE = 0x7;
static const uint8_t TAG_REPORT_ID = 0x8;
static const uint8_t TAG_REPORT_COUNT = 0x9;

// Local item tags
static const uint8_t TAG_USAGE = 0x0;
static const uint8_t TAG_USAGE_MIN = 0x1;
static const uint8_t TAG_USAGE_MAX = 0x2;

// Usage pages
static const uint16_t PAGE_GENERIC_DESKTOP = 0x01;
static const uint16_t PAGE_KEYBOARD = 0x07;
static const uint16_t PAGE_BUTTON = 0x09;
static const uint16_t PAGE_CONSUMER = 0x0C;

// Generic Desktop usages
static const uint16_t USAGE_X = 0x30;
static const uint16_t USAGE_Y = 0x31;
static const uint16_t USAGE_WHEEL = 0x38;
static const uint16_t USAGE_AC_PAN = 0x238;  // Horizontal scroll

// Parser state
struct ParserState {
    uint16_t usagePage = 0;
    int32_t logicalMin = 0;
    int32_t logicalMax = 0;
    uint8_t reportSize = 0;
    uint8_t reportCount = 0;
    uint8_t reportId = 0;

    uint16_t usages[HID_MAX_USAGES_PER_ITEM];
    uint8_t usageCount = 0;
    uint16_t usageMin = 0;
    uint16_t usageMax = 0;

    uint16_t bitOffset = 0;

    void clearLocal() {
        usageCount = 0;
        usageMin = 0;
        usageMax = 0;
    }
};

static int32_t getItemData(const uint8_t* data, uint8_t size, bool isSigned) {
    if (size == 0) return 0;
    if (size == 1) return isSigned ? (int8_t)data[0] : data[0];
    if (size == 2) {
        uint16_t val = data[0] | (data[1] << 8);
        return isSigned ? (int16_t)val : val;
    }
    if (size == 4) {
        return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    }
    return 0;
}

static FieldType getFieldType(uint16_t usagePage, uint16_t usage) {
    if (usagePage == PAGE_BUTTON) {
        return FieldType::BUTTONS;
    }
    if (usagePage == PAGE_GENERIC_DESKTOP) {
        switch (usage) {
            case USAGE_X: return FieldType::X;
            case USAGE_Y: return FieldType::Y;
            case USAGE_WHEEL: return FieldType::WHEEL;
        }
    }
    if (usagePage == PAGE_CONSUMER) {
        if (usage == USAGE_AC_PAN) {
            return FieldType::HWHEEL;
        }
        // Consumer Control usage (media keys, volume, etc.)
        return FieldType::CONSUMER;
    }
    if (usagePage == PAGE_KEYBOARD) {
        if (usage >= HID_MOD_KEY_START && usage <= HID_MOD_KEY_END) {
            return FieldType::MODIFIER;
        }
        return FieldType::KEYCODE;
    }
    return FieldType::UNKNOWN;
}

static void addField(ReportFormat* fmt, FieldType type, uint8_t bitOffset,
                     uint8_t bitSize, bool isSigned) {
    if (fmt->fieldCount >= HID_MAX_FIELDS_PER_REPORT) return;
    if (type == FieldType::UNKNOWN) return;

    // Check for duplicate type - update size if larger
    for (int i = 0; i < fmt->fieldCount; i++) {
        if (fmt->fields[i].type == type) {
            if (bitSize > fmt->fields[i].bitSize) {
                fmt->fields[i].bitSize = bitSize;
            }
            return;
        }
    }

    Field& f = fmt->fields[fmt->fieldCount++];
    f.type = type;
    f.bitOffset = bitOffset;
    f.bitSize = bitSize;
    f.isSigned = isSigned;
}

static ReportFormat* findOrCreateReport(ParsedDevice* dev, uint8_t reportId) {
    for (int i = 0; i < dev->reportCount; i++) {
        if (dev->reports[i].reportId == reportId) {
            return &dev->reports[i];
        }
    }
    if (dev->reportCount >= HID_MAX_REPORTS_PER_DEVICE) return nullptr;
    ReportFormat* fmt = &dev->reports[dev->reportCount++];
    memset(fmt, 0, sizeof(ReportFormat));
    fmt->reportId = reportId;
    return fmt;
}

bool parse(const uint8_t* data, size_t len, ParsedDevice* out) {
    memset(out, 0, sizeof(ParsedDevice));

    ParserState state;
    size_t pos = 0;

    while (pos < len) {
        uint8_t prefix = data[pos++];

        // Long item
        if (prefix == 0xFE) {
            if (pos + 2 > len) break;
            uint8_t dataSize = data[pos++];
            pos++;
            pos += dataSize;
            continue;
        }

        uint8_t size = prefix & 0x03;
        if (size == 3) size = 4;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;

        if (pos + size > len) break;

        const uint8_t* itemData = &data[pos];
        pos += size;

        if (type == TYPE_GLOBAL) {
            switch (tag) {
                case TAG_USAGE_PAGE:
                    state.usagePage = getItemData(itemData, size, false);
                    break;
                case TAG_LOGICAL_MIN:
                    state.logicalMin = getItemData(itemData, size, true);
                    break;
                case TAG_LOGICAL_MAX:
                    state.logicalMax = getItemData(itemData, size, true);
                    break;
                case TAG_REPORT_SIZE:
                    state.reportSize = getItemData(itemData, size, false);
                    break;
                case TAG_REPORT_COUNT:
                    state.reportCount = getItemData(itemData, size, false);
                    break;
                case TAG_REPORT_ID:
                    state.reportId = getItemData(itemData, size, false);
                    state.bitOffset = 0;
                    break;
            }
        } else if (type == TYPE_LOCAL) {
            switch (tag) {
                case TAG_USAGE:
                    if (state.usageCount < HID_MAX_USAGES_PER_ITEM) {
                        state.usages[state.usageCount++] = getItemData(itemData, size, false);
                    }
                    break;
                case TAG_USAGE_MIN:
                    state.usageMin = getItemData(itemData, size, false);
                    break;
                case TAG_USAGE_MAX:
                    state.usageMax = getItemData(itemData, size, false);
                    break;
            }
        } else if (type == TYPE_MAIN) {
            switch (tag) {
                case TAG_COLLECTION:
                    state.clearLocal();
                    break;

                case TAG_END_COLLECTION:
                    break;

                case TAG_INPUT: {
                    ReportFormat* fmt = findOrCreateReport(out, state.reportId);
                    if (!fmt) break;

                    bool isConstant = (size > 0 && (itemData[0] & 0x01));
                    bool isRelative = (size > 0 && (itemData[0] & 0x04));

                    if (!isConstant) {
                        bool isSigned = isRelative || (state.logicalMin < 0);

                        if (state.usageMax > 0 && state.usageMax >= state.usageMin) {
                            // Usage range (e.g. keyboard keycodes 0-255)
                            FieldType ftype = getFieldType(state.usagePage, state.usageMin);
                            uint8_t totalBits = state.reportSize * state.reportCount;
                            addField(fmt, ftype, state.bitOffset, totalBits, isSigned);
                        } else if (state.usageCount > 0) {
                            for (int i = 0; i < state.reportCount && i < state.usageCount; i++) {
                                FieldType ftype = getFieldType(state.usagePage, state.usages[i]);
                                addField(fmt, ftype, state.bitOffset + i * state.reportSize,
                                        state.reportSize, isSigned);
                            }
                            if (state.usageCount > 0 && state.usageCount < state.reportCount) {
                                FieldType ftype = getFieldType(state.usagePage,
                                    state.usages[state.usageCount - 1]);
                                for (int i = state.usageCount; i < state.reportCount; i++) {
                                    addField(fmt, ftype, state.bitOffset + i * state.reportSize,
                                            state.reportSize, isSigned);
                                }
                            }
                        }
                    }

                    state.bitOffset += state.reportSize * state.reportCount;
                    fmt->totalBits = state.bitOffset;
                    state.clearLocal();
                    break;
                }

                case TAG_OUTPUT:
                case TAG_FEATURE:
                    state.bitOffset += state.reportSize * state.reportCount;
                    state.clearLocal();
                    break;
            }
        }
    }

    return out->reportCount > 0;
}

int32_t decodeField(const uint8_t* data, const Field& field) {
    uint8_t byteOffset = field.bitOffset / 8;
    uint8_t bitInByte = field.bitOffset % 8;

    uint32_t raw = 0;
    int bytesNeeded = (bitInByte + field.bitSize + 7) / 8;

    for (int i = 0; i < bytesNeeded && i < 4; i++) {
        raw |= (uint32_t)data[byteOffset + i] << (i * 8);
    }

    raw >>= bitInByte;
    uint32_t mask = (1UL << field.bitSize) - 1;
    raw &= mask;

    if (field.isSigned && (raw & (1UL << (field.bitSize - 1)))) {
        raw |= ~mask;
    }

    return (int32_t)raw;
}

bool ReportFormat::isKeyboard() const {
    for (int i = 0; i < fieldCount; i++) {
        if (fields[i].type == FieldType::MODIFIER || fields[i].type == FieldType::KEYCODE) {
            return true;
        }
    }
    return false;
}

bool ReportFormat::isMouse() const {
    for (int i = 0; i < fieldCount; i++) {
        if (fields[i].type == FieldType::X || fields[i].type == FieldType::Y ||
            fields[i].type == FieldType::BUTTONS ||
            fields[i].type == FieldType::WHEEL || fields[i].type == FieldType::HWHEEL) {
            return true;
        }
    }
    return false;
}

bool ReportFormat::isConsumer() const {
    for (int i = 0; i < fieldCount; i++) {
        if (fields[i].type == FieldType::CONSUMER) {
            return true;
        }
    }
    return false;
}

const ReportFormat* ParsedDevice::findReport(uint8_t reportId) const {
    for (int i = 0; i < reportCount; i++) {
        if (reports[i].reportId == reportId) {
            return &reports[i];
        }
    }
    return nullptr;
}

const ReportFormat* ParsedDevice::findMouseReport() const {
    for (int i = 0; i < reportCount; i++) {
        for (int j = 0; j < reports[i].fieldCount; j++) {
            if (reports[i].fields[j].type == FieldType::X) {
                return &reports[i];
            }
        }
    }
    return nullptr;
}

const ReportFormat* ParsedDevice::findKeyboardReport() const {
    for (int i = 0; i < reportCount; i++) {
        if (reports[i].isKeyboard()) {
            return &reports[i];
        }
    }
    return nullptr;
}

const ReportFormat* ParsedDevice::findConsumerReport() const {
    for (int i = 0; i < reportCount; i++) {
        if (reports[i].isConsumer()) {
            return &reports[i];
        }
    }
    return nullptr;
}

} // namespace HidParser
