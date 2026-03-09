#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "task_base.h"

/**
 * WS2812 RGB LED Controller
 *
 * Drives a single NeoPixel via RMT.
 * Pattern + color sent via queue - safe for inter-task use.
 * Smooth crossfade (LED_TRANSITION_MS) between any state changes.
 */

enum class LedPattern : uint8_t {
    OFF,
    SOLID,
    BLINK,
    FADE
};

struct LedCmd {
    LedPattern pattern;
    uint8_t r, g, b;

    bool operator==(const LedCmd& o) const {
        return pattern == o.pattern && r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const LedCmd& o) const { return !(*this == o); }
};

class Led : public TaskBase {
public:
    explicit Led(gpio_num_t pin);

    void init() override;
    void run() override;

    // Queue-based commands (thread-safe)
    void set(const LedCmd& cmd);
    void set(LedPattern pattern, uint8_t r, uint8_t g, uint8_t b);
    void set(LedPattern pattern);

private:
    static constexpr size_t QUEUE_SIZE = 8;

    gpio_num_t pin_;
    led_strip_handle_t strip_ = nullptr;

    // Target state (what we're transitioning towards)
    LedCmd target_ = {};

    // Transition
    uint8_t from_r_ = 0, from_g_ = 0, from_b_ = 0;
    int64_t transition_start_us_ = 0;
    bool transitioning_ = false;

    // Pattern animation state
    bool blink_state_ = false;
    uint8_t breath_phase_ = 0;
    int64_t last_update_us_ = 0;

    // Last displayed color (used as transition start point)
    uint8_t disp_r_ = 0, disp_g_ = 0, disp_b_ = 0;

    // Static queue allocation
    StaticQueue_t queue_buffer_;
    uint8_t queue_storage_[QUEUE_SIZE * sizeof(LedCmd)];
    QueueHandle_t queue_ = nullptr;

    void initHardware();
    void setPixel(uint8_t r, uint8_t g, uint8_t b);
    void applyCmd(const LedCmd& cmd);
    void processCommands();
    void updateTransition();
    void updatePattern();
};
