#pragma once

#include "config.h"

#include "task_base.h"
#include "button_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

/**
 * Button Task with debouncing and gesture detection
 *
 * Thread-safe polling via handleState().
 */

struct ButtonConfig {
    uint16_t debounce_ms = BUTTON_DEBOUNCE_MS;
    uint16_t click_window_ms = BUTTON_CLICK_WINDOW_MS;
    uint16_t long_press_ms = BUTTON_LONG_PRESS_MS;
    uint16_t extra_long_press_ms = BUTTON_EXTRA_LONG_PRESS_MS;
    uint16_t factory_reset_ms = BUTTON_FACTORY_RESET_MS;
};

class Button : public TaskBase {
public:
    explicit Button(gpio_num_t pin, const ButtonConfig& config = ButtonConfig{});

    void init() override;
    void run() override;

    /**
     * Check if expected event occurred. Thread-safe.
     * If match: clears event and returns true.
     * If no match: returns false (event preserved).
     */
    bool handleState(ButtonEvent expected);

    // Raw GPIO read - safe to call from any task
    bool isPressed() const { return gpio_get_level(pin_) == 0; }

private:
    gpio_num_t pin_;
    ButtonConfig config_;

    // Semaphore for thread-safe event access
    StaticSemaphore_t sem_buffer_;
    SemaphoreHandle_t sem_ = nullptr;

    // Current pending event
    ButtonEvent last_event_ = ButtonEvent::NONE;

    bool last_reading_ = true;
    bool stable_state_ = true;
    int64_t debounce_time_ = 0;
    int64_t press_time_ = 0;
    int64_t release_time_ = 0;

    bool long_press_triggered_ = false;
    bool extra_long_press_triggered_ = false;
    bool factory_reset_triggered_ = false;

    void handlePress();
    void handleRelease();
    void checkLongPress();
    void setEvent(ButtonEvent event);
};
