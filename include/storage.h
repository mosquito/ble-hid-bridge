#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string>
#include <stdint.h>

/**
 * Thread-safe NVS Key-Value Storage
 *
 * All operations protected by mutex for multi-task access.
 * Get methods return default value if key doesn't exist.
 */
class Storage {
public:
    explicit Storage(const char* ns = "storage");
    ~Storage();

    // Initialize NVS and open namespace
    bool init();

    // Clear all keys in this namespace
    void prune();

    // String operations
    void set(const char* key, const std::string& value);
    void set(const char* key, const char* value);  // Prevent implicit char*->bool conversion
    std::string get(const char* key, const std::string& default_value = "");

    // Integer operations
    void set(const char* key, int32_t value);
    int32_t getInt(const char* key, int32_t default_value = 0);

    // Unsigned integer operations
    void set(const char* key, uint32_t value);
    uint32_t getUInt(const char* key, uint32_t default_value = 0);

    // Float operations
    void set(const char* key, float value);
    float getFloat(const char* key, float default_value = 0.0f);

    // Bool operations
    void set(const char* key, bool value);
    bool getBool(const char* key, bool default_value = false);

    // Raw blob operations
    void setBlob(const char* key, const void* data, size_t len);
    size_t getBlob(const char* key, void* data, size_t max_len);

    // Check if key exists
    bool exists(const char* key);

    // Remove single key
    void remove(const char* key);

private:
    char namespace_[16];
    nvs_handle_t handle_ = 0;
    bool initialized_ = false;

    // Binary semaphore for multi-core synchronization
    StaticSemaphore_t mutex_buffer_;
    SemaphoreHandle_t mutex_ = nullptr;

    void lock();
    void unlock();
};
