#pragma once

#include "config.h"
#include <cstdint>
#include <cstring>

class Storage;

// Unified keycode encoding:
//   0x0000-0x00FF  = keyboard keycode (HID Usage Page 0x07)
//   0x1000-0x13FF  = consumer control (HID Usage Page 0x0C), value = code - REMAP_CONSUMER_BASE
static constexpr uint16_t REMAP_CONSUMER_BASE = 0x1000;

struct RemapEntry {
    uint8_t from;    // source keyboard keycode (0x00-0xFF)
    uint16_t to;     // target: keyboard (0x0000-0x00FF) or consumer (0x1000+)
};

struct DeviceRemapTable {
    char address[18];
    RemapEntry entries[MAX_KEY_REMAPS];
    uint8_t count;

    /**
     * Look up a keycode. Returns the full unified target code.
     * If no remap exists, returns the original keycode unchanged.
     */
    uint16_t remapFull(uint8_t keycode) const
    {
        for (int i = 0; i < count; i++) {
            if (entries[i].from == keycode) {
                return entries[i].to;
            }
        }
        return keycode;  // no remap
    }
};

/**
 * Apply remap table to a keyboard report in-place.
 *
 * Processes modifier bits (0xE0-0xE7) and keycodes array.
 * Remapped keys can target modifiers, regular keys, or consumer codes.
 *
 * @param table       Remap table for this device
 * @param modifier    [in/out] Modifier byte
 * @param keys        [in/out] 6-key array
 * @return Consumer usage code if any key was remapped to consumer, else 0
 */
uint16_t applyRemap(const DeviceRemapTable* table,
                    uint8_t& modifier, uint8_t* keys);

/**
 * Manages per-device remap tables with Storage persistence.
 *
 * Owned by HidBridge (Core 1). All access is single-threaded.
 * Web server reads via Storage (mutex-protected) for GET;
 * writes go through cmd queue.
 */
class KeyRemapManager {
public:
    explicit KeyRemapManager(Storage* storage) : storage_(storage) {}

    void loadAll();

    const DeviceRemapTable* getTable(const char* address) const;

    void setRemaps(const char* address, const RemapEntry* entries, uint8_t count);

    void clearRemaps(const char* address);

private:
    Storage* storage_;
    DeviceRemapTable tables_[MAX_REMAP_DEVICES];
    uint8_t tableCount_ = 0;

    void saveTable(int idx);
    void saveCount();
    int findTable(const char* address) const;
};
