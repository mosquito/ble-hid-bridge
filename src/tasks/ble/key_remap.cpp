#include "key_remap.h"
#include "storage.h"
#include "config.h"

#include <cstring>
#include <cstdio>

static const char* TAG = "remap";

// Storage blob format per device:
//   [17 bytes address (null-terminated)] [1 byte count] [count * 3 bytes entries]
// Entry: [1 byte from] [2 bytes to (little-endian)]
static constexpr size_t BLOB_ADDR_SIZE = 18;
static constexpr size_t BLOB_ENTRY_SIZE = 3;
static constexpr size_t BLOB_MAX_SIZE = BLOB_ADDR_SIZE + 1 + MAX_KEY_REMAPS * BLOB_ENTRY_SIZE + 1;

uint16_t applyRemap(const DeviceRemapTable* table,
                    uint8_t& modifier, uint8_t* keys)
{
    if (!table || table->count == 0) return 0;

    uint8_t newModifier = 0;
    uint8_t newKeys[6] = {};
    int keyIdx = 0;
    uint16_t consumerUsage = 0;

    // Phase 1: process 8 modifier bits (keycodes 0xE0-0xE7)
    for (int bit = 0; bit < 8; bit++) {
        if (!(modifier & (1 << bit))) continue;
        uint8_t srcKeycode = HID_MOD_KEY_START + bit;
        uint16_t target = table->remapFull(srcKeycode);

        if (target >= REMAP_CONSUMER_BASE) {
            consumerUsage = target - REMAP_CONSUMER_BASE;
        } else if (target >= HID_MOD_KEY_START && target <= HID_MOD_KEY_END) {
            newModifier |= (1 << (target - HID_MOD_KEY_START));
        } else if (target != 0 && keyIdx < HID_KEYBOARD_KEY_COUNT) {
            newKeys[keyIdx++] = (uint8_t)target;
        } else if (target == srcKeycode) {
            // No remap — preserve original modifier bit
            newModifier |= (1 << bit);
        }
    }

    // Phase 2: process 6 keycodes
    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) continue;
        uint16_t target = table->remapFull(keys[i]);

        if (target >= REMAP_CONSUMER_BASE) {
            consumerUsage = target - REMAP_CONSUMER_BASE;
        } else if (target >= HID_MOD_KEY_START && target <= HID_MOD_KEY_END) {
            newModifier |= (1 << (target - HID_MOD_KEY_START));
        } else if (target != 0 && keyIdx < HID_KEYBOARD_KEY_COUNT) {
            newKeys[keyIdx++] = (uint8_t)target;
        }
    }

    // Write back
    modifier = newModifier;
    memcpy(keys, newKeys, HID_KEYBOARD_KEY_COUNT);
    return consumerUsage;
}

// --- KeyRemapManager ---

void KeyRemapManager::loadAll()
{
    uint8_t count = (uint8_t)storage_->getUInt("rmap_cnt", 0);

    tableCount_ = 0;
    uint8_t buf[BLOB_MAX_SIZE];

    for (int i = 0; i < count && i < MAX_REMAP_DEVICES; i++) {
        char key[12];
        snprintf(key, sizeof(key), "rmap_%d", i);

        size_t blobLen = storage_->getBlob(key, buf, sizeof(buf));
        if (blobLen < BLOB_ADDR_SIZE + 1) continue;

        DeviceRemapTable& t = tables_[tableCount_];
        memcpy(t.address, buf, BLOB_ADDR_SIZE);
        t.address[BLE_ADDR_STR_LEN - 1] = '\0';
        t.count = buf[BLOB_ADDR_SIZE];
        if (t.count > MAX_KEY_REMAPS) t.count = MAX_KEY_REMAPS;

        size_t expectedLen = BLOB_ADDR_SIZE + 1 + t.count * BLOB_ENTRY_SIZE;
        if (blobLen < expectedLen) {
            t.count = 0;
            continue;
        }

        const uint8_t* p = buf + BLOB_ADDR_SIZE + 1;
        for (int j = 0; j < t.count; j++) {
            t.entries[j].from = p[0];
            t.entries[j].to = p[1] | (p[2] << 8);
            p += BLOB_ENTRY_SIZE;
        }

        t.scrollScale = (blobLen > expectedLen) ? (int8_t)buf[expectedLen] : 0;

        LOG(TAG, "Loaded %d remaps for %s (scrollScale=%d)", t.count, t.address, t.scrollScale);
        tableCount_++;
    }
}

const DeviceRemapTable* KeyRemapManager::getTable(const char* address) const
{
    int idx = findTable(address);
    return (idx >= 0) ? &tables_[idx] : nullptr;
}

void KeyRemapManager::setRemaps(const char* address, const RemapEntry* entries, uint8_t count,
                                int8_t scrollScale)
{
    if (count > MAX_KEY_REMAPS) count = MAX_KEY_REMAPS;

    int idx = findTable(address);
    if (idx < 0) {
        if (tableCount_ >= MAX_REMAP_DEVICES) {
            ERR(TAG, "No room for remap table");
            return;
        }
        idx = tableCount_++;
        strncpy(tables_[idx].address, address, sizeof(tables_[idx].address) - 1);
        tables_[idx].address[BLE_ADDR_STR_LEN - 1] = '\0';
    }

    tables_[idx].count = count;
    memcpy(tables_[idx].entries, entries, count * sizeof(RemapEntry));
    tables_[idx].scrollScale = scrollScale;

    saveTable(idx);
    saveCount();
    LOG(TAG, "Set %d remaps for %s (scrollScale=%d)", count, address, scrollScale);
}

void KeyRemapManager::clearRemaps(const char* address)
{
    int idx = findTable(address);
    if (idx < 0) return;

    // Compact array
    for (int i = idx; i < tableCount_ - 1; i++) {
        tables_[i] = tables_[i + 1];
    }
    tableCount_--;

    // Re-save all tables (indices shifted)
    for (int i = idx; i < tableCount_; i++) {
        saveTable(i);
    }

    // Remove the now-unused last slot
    char key[12];
    snprintf(key, sizeof(key), "rmap_%d", tableCount_);
    storage_->remove(key);

    saveCount();
    LOG(TAG, "Cleared remaps for %s", address);
}

void KeyRemapManager::saveTable(int idx)
{
    if (idx < 0 || idx >= tableCount_) return;

    uint8_t buf[BLOB_MAX_SIZE];
    const DeviceRemapTable& t = tables_[idx];

    memcpy(buf, t.address, BLOB_ADDR_SIZE);
    buf[BLOB_ADDR_SIZE] = t.count;

    uint8_t* p = buf + BLOB_ADDR_SIZE + 1;
    for (int j = 0; j < t.count; j++) {
        p[0] = t.entries[j].from;
        p[1] = t.entries[j].to & 0xFF;
        p[2] = (t.entries[j].to >> 8) & 0xFF;
        p += BLOB_ENTRY_SIZE;
    }

    size_t blobLen = BLOB_ADDR_SIZE + 1 + t.count * BLOB_ENTRY_SIZE;
    buf[blobLen] = (uint8_t)t.scrollScale;
    blobLen++;

    char key[12];
    snprintf(key, sizeof(key), "rmap_%d", idx);
    storage_->setBlob(key, buf, blobLen);
}

void KeyRemapManager::saveCount()
{
    storage_->set("rmap_cnt", (uint32_t)tableCount_);
}

int KeyRemapManager::findTable(const char* address) const
{
    for (int i = 0; i < tableCount_; i++) {
        if (strcmp(tables_[i].address, address) == 0) return i;
    }
    return -1;
}
