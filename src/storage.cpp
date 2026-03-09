#include "storage.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "storage";

Storage::Storage(const char* ns)
{
    mutex_ = xSemaphoreCreateBinaryStatic(&mutex_buffer_);
    xSemaphoreGive(mutex_);  // Binary semaphore starts empty

    strncpy(namespace_, ns, sizeof(namespace_) - 1);
    namespace_[sizeof(namespace_) - 1] = '\0';
}

Storage::~Storage()
{
    if (initialized_ && handle_ != 0) {
        nvs_close(handle_);
    }
}

void Storage::lock()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
}

void Storage::unlock()
{
    xSemaphoreGive(mutex_);
}

bool Storage::init()
{
    lock();

    if (initialized_) {
        unlock();
        return true;
    }

    // Initialize NVS flash (only once across all instances)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase required: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        unlock();
        return false;
    }

    // Open namespace
    ret = nvs_open(namespace_, NVS_READWRITE, &handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        unlock();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized namespace '%s'", namespace_);

    unlock();
    return true;
}

void Storage::prune()
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_erase_all(handle_);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Pruned namespace '%s'", namespace_);
        } else {
            ESP_LOGE(TAG, "Prune commit failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Prune erase failed: %s", esp_err_to_name(ret));
    }

    unlock();
}

void Storage::set(const char* key, const std::string& value)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_set_str(handle_, key, value.c_str());
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

void Storage::set(const char* key, const char* value)
{
    set(key, std::string(value ? value : ""));
}

std::string Storage::get(const char* key, const std::string& default_value)
{
    lock();

    if (!initialized_) {
        unlock();
        return default_value;
    }

    size_t required_size = 0;
    esp_err_t ret = nvs_get_str(handle_, key, nullptr, &required_size);
    if (ret != ESP_OK || required_size == 0) {
        unlock();
        return default_value;
    }

    char* buf = new char[required_size];
    ret = nvs_get_str(handle_, key, buf, &required_size);
    if (ret != ESP_OK) {
        delete[] buf;
        unlock();
        return default_value;
    }

    std::string result(buf);
    delete[] buf;

    unlock();
    return result;
}

void Storage::set(const char* key, int32_t value)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_set_i32(handle_, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set int32 '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

int32_t Storage::getInt(const char* key, int32_t default_value)
{
    lock();

    if (!initialized_) {
        unlock();
        return default_value;
    }

    int32_t value = default_value;
    nvs_get_i32(handle_, key, &value);

    unlock();
    return value;
}

void Storage::set(const char* key, uint32_t value)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_set_u32(handle_, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set uint32 '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

uint32_t Storage::getUInt(const char* key, uint32_t default_value)
{
    lock();

    if (!initialized_) {
        unlock();
        return default_value;
    }

    uint32_t value = default_value;
    nvs_get_u32(handle_, key, &value);

    unlock();
    return value;
}

void Storage::set(const char* key, float value)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    // Store float as blob
    esp_err_t ret = nvs_set_blob(handle_, key, &value, sizeof(float));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set float '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

float Storage::getFloat(const char* key, float default_value)
{
    lock();

    if (!initialized_) {
        unlock();
        return default_value;
    }

    float value = default_value;
    size_t len = sizeof(float);
    nvs_get_blob(handle_, key, &value, &len);

    unlock();
    return value;
}

void Storage::set(const char* key, bool value)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_set_u8(handle_, key, value ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set bool '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

bool Storage::getBool(const char* key, bool default_value)
{
    lock();

    if (!initialized_) {
        unlock();
        return default_value;
    }

    uint8_t value = default_value ? 1 : 0;
    nvs_get_u8(handle_, key, &value);

    unlock();
    return value != 0;
}

void Storage::setBlob(const char* key, const void* data, size_t len)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_set_blob(handle_, key, data, len);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Set blob '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}

size_t Storage::getBlob(const char* key, void* data, size_t max_len)
{
    lock();

    if (!initialized_) {
        unlock();
        return 0;
    }

    size_t len = max_len;
    esp_err_t ret = nvs_get_blob(handle_, key, data, &len);
    if (ret != ESP_OK) {
        unlock();
        return 0;
    }

    unlock();
    return len;
}

bool Storage::exists(const char* key)
{
    lock();

    if (!initialized_) {
        unlock();
        return false;
    }

    // Try to get string size - works for any type as existence check
    size_t required_size = 0;
    esp_err_t ret = nvs_get_str(handle_, key, nullptr, &required_size);
    if (ret == ESP_OK) {
        unlock();
        return true;
    }

    // Try blob
    ret = nvs_get_blob(handle_, key, nullptr, &required_size);
    if (ret == ESP_OK) {
        unlock();
        return true;
    }

    // Try i32
    int32_t dummy;
    ret = nvs_get_i32(handle_, key, &dummy);

    unlock();
    return ret == ESP_OK;
}

void Storage::remove(const char* key)
{
    lock();

    if (!initialized_) {
        unlock();
        return;
    }

    esp_err_t ret = nvs_erase_key(handle_, key);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Commit remove '%s' failed: %s", key, esp_err_to_name(ret));
        }
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Remove '%s' failed: %s", key, esp_err_to_name(ret));
    }

    unlock();
}
