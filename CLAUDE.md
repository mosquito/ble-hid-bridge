# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# ESP32-S3 Project Guidelines

## Project Overview

ESP32-S3 firmware using **ESP-IDF** and **FreeRTOS**. This is a bare-metal embedded project with strict architectural constraints.

## Build & Flash

```bash
make              # Build (default: esp32s3_zero)
make upload       # Build and flash
make monitor      # Serial monitor
make clean        # Clean build
make menuconfig   # ESP-IDF configuration

# Or with pio directly:
pio run -e esp32s3_zero
pio run -e esp32s3_zero -t upload

```

Available environments: `esp32s3_zero`, `esp32s3_zero_dev`

---

## CRITICAL RULES

### 1. NO ARDUINO - ESP-IDF ONLY

**Arduino libraries are FORBIDDEN.** Use ESP-IDF APIs exclusively.

```cpp
// WRONG - Arduino
#include <Arduino.h>
pinMode(LED_PIN, OUTPUT);
digitalWrite(LED_PIN, HIGH);
delay(100);

// CORRECT - ESP-IDF
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << LED_PIN),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&io_conf);
gpio_set_level(LED_PIN, 1);
vTaskDelay(pdMS_TO_TICKS(100));
```

### 2. INTER-TASK COMMUNICATION VIA QUEUES ONLY

**Direct access to global variables between tasks is FORBIDDEN.** All inter-task communication MUST use FreeRTOS primitives.

```cpp
// WRONG - Shared global
static volatile bool g_button_pressed = false;  // Task A writes, Task B reads

// CORRECT - Queue-based communication
static QueueHandle_t button_queue;

// In init:
button_queue = xQueueCreate(10, sizeof(button_event_t));

// Task A (producer):
button_event_t event = { .pin = pin, .pressed = true };
xQueueSend(button_queue, &event, portMAX_DELAY);

// Task B (consumer):
button_event_t event;
if (xQueueReceive(button_queue, &event, portMAX_DELAY) == pdTRUE) {
    // handle event
}
```

**Acceptable FreeRTOS primitives:**
- `xQueueSend` / `xQueueReceive` - primary choice for data passing
- `xSemaphoreGive` / `xSemaphoreTake` - synchronization signals
- `xEventGroupSetBits` / `xEventGroupWaitBits` - multi-flag coordination
- `xTaskNotify` / `xTaskNotifyWait` - lightweight task-to-task signals

### 3. STATIC ALLOCATION PREFERRED

Avoid `malloc`/`new` at runtime. Use static buffers or FreeRTOS static allocation APIs.

```cpp
// WRONG - Dynamic allocation
QueueHandle_t queue = xQueueCreate(10, sizeof(event_t));

// CORRECT - Static allocation
static StaticQueue_t queue_buffer;
static uint8_t queue_storage[10 * sizeof(event_t)];
QueueHandle_t queue = xQueueCreateStatic(10, sizeof(event_t), queue_storage, &queue_buffer);
```

### 4. FILE SIZE LIMIT: 300 LINES

One file, one purpose, single responsibility. If a file exceeds 300 lines, split it.

### 5. CLASS-BASED TASKS REQUIRED

All tasks MUST inherit from `TaskBase` (in `src/task_base.h`) and be instantiated on the stack in `app_main()`.

```cpp
// WRONG - Function-based task
void my_task(void* p) { while(true) { ... } }

// CORRECT - Class inheriting TaskBase
class MyTask : public TaskBase {
public:
    MyTask(gpio_num_t pin) : pin_(pin) {}
    void init() override { /* setup hardware */ }
    void run() override { /* one iteration */ }
private:
    gpio_num_t pin_;
};
```

---

## Project Structure

```
include/               # Common shared headers (NOT task headers)
├── task_base.h        # Base class for all tasks
└── storage.h          # NVS key-value storage

src/
├── config.h           # ALL compile-time configuration
├── main.cpp           # Entry point, task creation only
├── storage.cpp        # Storage implementation
└── tasks/             # Task headers stay here with their .cpp
    ├── taskname.cpp
    ├── taskname.h
    └── complex_task/
        ├── complex_task.h
        ├── complex_task.cpp
        └── helper.cpp
```

**Header placement rules:**
- `include/` - Common utilities shared across multiple tasks (task_base.h, storage.h)
- `src/tasks/` - Task-specific headers stay with their implementation

### Task File Template

**Header (`src/tasks/blink_task.h`):**
```cpp
#pragma once

#include "task_base.h"
#include "driver/gpio.h"

class BlinkTask : public TaskBase {
public:
    BlinkTask(gpio_num_t pin);
    void init() override;
    void run() override;

private:
    gpio_num_t pin_;
    bool state_ = false;
};
```

**Implementation (`src/tasks/blink_task.cpp`):**
```cpp
#include "blink_task.h"
#include "config.h"
#include "esp_log.h"

static const char* TAG = "blink";

BlinkTask::BlinkTask(gpio_num_t pin) : pin_(pin) {}

void BlinkTask::init()
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Initialized on GPIO%d", pin_);
}

void BlinkTask::run()
{
    state_ = !state_;
    gpio_set_level(pin_, state_);
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

---

## Configuration

**ALL configurable values go in `src/config.h`**. Hardware pins are passed via build flags in `platformio.ini`.

### Available Constants

```cpp
// Core affinity (see Architecture section for full mapping)
RTOS_CORE_0         // Networking + UI (WiFi, WebServer, LED, Button)
RTOS_CORE_1         // Realtime HID pipeline (Bluedroid, HidBridge, TinyUSB)
RTOS_CORE_ANY       // No affinity

// Task priorities (0-24, higher = more urgent)
RTOS_PRIORITY_LOW       // 2  - Background tasks
RTOS_PRIORITY_MEDIUM    // 5  - Normal tasks
RTOS_PRIORITY_HIGH      // 10 - Time-sensitive tasks
RTOS_PRIORITY_REALTIME  // 20 - Critical timing

// Defaults
DEFAULT_TASK_STACK_SIZE // 2048 bytes
DEFAULT_TASK_PRIORITY   // RTOS_PRIORITY_MEDIUM
```

### Adding New Configuration

```cpp
// In config.h - with build flag override support
#ifndef MY_NEW_SETTING
    #define MY_NEW_SETTING default_value
#endif

// Required settings (no default, must be in platformio.ini)
#ifndef REQUIRED_PIN
    #error "REQUIRED_PIN not defined - add to platformio.ini build_flags"
#endif
```

---

## Common Patterns

### GPIO Configuration

```cpp
#include "driver/gpio.h"

static void init_gpio_output(gpio_num_t pin)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}
```

### Logging

```cpp
#include "esp_log.h"

static const char* TAG = "module_name";

ESP_LOGE(TAG, "Error: %s", error_msg);    // Red - errors
ESP_LOGW(TAG, "Warning: %d", value);      // Yellow - warnings
ESP_LOGI(TAG, "Info message");            // Green - info
ESP_LOGD(TAG, "Debug: ptr=%p", ptr);      // (hidden by default)
```

### Task Creation in main.cpp

```cpp
#include "task_base.h"
#include "tasks/blink_task.h"

extern "C" void app_main()
{
    ESP_LOGI(TAG, "ESP32-S3 started");

    // Instantiate on stack (valid forever - app_main never returns)
    BlinkTask blink(USER_LED_PIN);

    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,     // Base class static wrapper
        "blink",
        DEFAULT_TASK_STACK_SIZE,
        &blink,                  // Pass instance pointer
        RTOS_PRIORITY_LOW,
        nullptr,
        RTOS_CORE_1
    );

    // Block forever (tasks run independently)
    while (true) { vTaskDelay(portMAX_DELAY); }
}
```

---

## ESP-IDF Quick Reference

| Arduino | ESP-IDF Equivalent |
|---------|-------------------|
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| `millis()` | `esp_timer_get_time() / 1000` |
| `micros()` | `esp_timer_get_time()` |
| `digitalWrite(pin, val)` | `gpio_set_level(pin, val)` |
| `digitalRead(pin)` | `gpio_get_level(pin)` |
| `Serial.print()` | `ESP_LOGI(TAG, "...")` |

---

## Web UI

Built from `web/` directory into a single gzipped HTML embedded in firmware.

- `web/build.py` parses SFC-like `.html` components using regex: `<template>(.*?)</template>` (non-greedy)
- **NEVER use `<template>` tags inside component templates** — the regex will match the first `</template>` and break extraction
- Use `v-if`/`v-else` directly on `<div>`/`<span>` elements instead
- Components: `web/components/*.html`, app logic: `web/app.js`
- Build runs automatically via Makefile (`make` triggers `web/build.py` → `include/index_html.h`)

---

## Architecture

### Core Affinity

```
Core 0 (networking/UI):         Core 1 (realtime HID pipeline):
  WiFi AP                         Bluedroid (BTC callbacks)
  WebServer (httpd)               HidBridge task
  LED task                        TinyUSB task
  Button task
  Button handler
```

**Core 1 is a dedicated realtime core** — nothing else runs there. BTC callbacks and HidBridge::run() are both on Core 1, so the NOTIFY ring buffer needs no mutex.

### HID Data Flow (hot path)

```
BLE Mouse/Keyboard
    ↓ NOTIFY (BTC callback, Core 1)
    ↓ single-core ring buffer + xTaskNotifyGive
HidBridge::run() (Core 1)
    ↓ processNotify → BleConnection (parse HID report map)
    ↓ UsbHid::sendMouse/sendKeyboard → ring buffer
    ↓ UsbHid::processOne() (1ms USB Full Speed frame pacing)
USB Host PC
```

### Cross-core Communication

Web Server (Core 0) ↔ HidBridge (Core 1) via FreeRTOS queues only:
- `cmd_queue_` — commands from Web/Button to HidBridge (BleCmdMsg)
- `event_queue_` — BLE events from HidBridge to WebServer (BleEventMsg)
- `getStats()` — reads atomic counters (safe cross-core, no queue needed)

### Button Actions

- **Double-click**: Toggle WiFi AP + WebServer on/off
- **Triple-click**: Headless BLE pairing (scan + auto-connect first HID device)
- **Hold 5s**: Factory reset warning (LED blinks red)
- **Hold 25s**: Factory reset (NVS erase + reboot)

---

## Checklist Before Committing

- [ ] No Arduino includes (`#include <Arduino.h>`)
- [ ] No global variable access between tasks
- [ ] All inter-task communication uses queues/semaphores
- [ ] New config values added to `src/config.h`
- [ ] Files under 300 lines
- [ ] Task files follow naming convention
- [ ] Static allocation used where possible
- [ ] Task classes inherit from `TaskBase`
- [ ] Tasks implement `init()` and `run()` methods
- [ ] Task instances created on stack in `app_main()`
