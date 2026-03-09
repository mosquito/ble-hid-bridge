#include "led.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char* TAG = "led";

static constexpr int64_t TRANSITION_US = (int64_t)LED_TRANSITION_MS * 1000;

static const char* patternName(LedPattern p)
{
    switch (p) {
        case LedPattern::OFF:   return "OFF";
        case LedPattern::SOLID: return "SOLID";
        case LedPattern::BLINK: return "BLINK";
        case LedPattern::FADE:  return "FADE";
        default: return "?";
    }
}

// Smooth fade-in / fade-out curve (gamma-corrected triangle)
static uint8_t breathCurve(uint8_t phase)
{
    uint8_t linear;
    if (phase < 128) {
        linear = phase * 2;
    } else {
        linear = (255 - phase) * 2;
    }
    return (uint8_t)((uint16_t)linear * linear / 255);
}

static uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t)
{
    return a + (int16_t)(b - a) * t / 255;
}

Led::Led(gpio_num_t pin)
    : pin_(pin)
{
}

void Led::init()
{
    queue_ = xQueueCreateStatic(
        QUEUE_SIZE,
        sizeof(LedCmd),
        queue_storage_,
        &queue_buffer_
    );

    initHardware();
    setPixel(0, 0, 0);

    LOG(TAG, "Task started on core %d, GPIO%d (WS2812)",
             xPortGetCoreID(), pin_);
}

void Led::initHardware()
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = pin_,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags = { .with_dma = false }
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
    setPixel(0, 0, 0);
}

void Led::setPixel(uint8_t r, uint8_t g, uint8_t b)
{
    disp_r_ = r;
    disp_g_ = g;
    disp_b_ = b;
    led_strip_set_pixel(strip_, 0, r, g, b);
    led_strip_refresh(strip_);
}

void Led::applyCmd(const LedCmd& cmd)
{
    if (cmd == target_) {
        return;
    }

    DBG(TAG, "%s(%d,%d,%d) -> %s(%d,%d,%d)",
        patternName(target_.pattern), target_.r, target_.g, target_.b,
        patternName(cmd.pattern), cmd.r, cmd.g, cmd.b);

    // Start transition from current displayed color
    from_r_ = disp_r_;
    from_g_ = disp_g_;
    from_b_ = disp_b_;

    target_ = cmd;
    transition_start_us_ = esp_timer_get_time();
    transitioning_ = true;

    // Reset pattern state for after transition
    blink_state_ = false;
    breath_phase_ = 0;
}

void Led::set(const LedCmd& cmd)
{
    if (queue_ == nullptr) {
        return;
    }
    xQueueSend(queue_, &cmd, 0);
}

void Led::set(LedPattern pattern, uint8_t r, uint8_t g, uint8_t b)
{
    set({ pattern, r, g, b });
}

void Led::set(LedPattern pattern)
{
    set({ pattern, 0, 0, 0 });
}

void Led::processCommands()
{
    LedCmd cmd;
    while (xQueueReceive(queue_, &cmd, 0) == pdTRUE) {
        applyCmd(cmd);
    }
}

void Led::updateTransition()
{
    int64_t elapsed_us = esp_timer_get_time() - transition_start_us_;

    if (elapsed_us >= TRANSITION_US) {
        // Transition complete — enter target pattern
        transitioning_ = false;
        last_update_us_ = esp_timer_get_time();

        switch (target_.pattern) {
            case LedPattern::OFF:
                setPixel(0, 0, 0);
                break;
            case LedPattern::SOLID:
                setPixel(target_.r, target_.g, target_.b);
                break;
            case LedPattern::BLINK:
                setPixel(target_.r, target_.g, target_.b);
                blink_state_ = true;
                break;
            case LedPattern::FADE:
                setPixel(target_.r, target_.g, target_.b);
                breath_phase_ = 64; // Start at ~peak so transition feels seamless
                break;
        }
        return;
    }

    // Interpolate displayed color toward target
    uint8_t t = (uint8_t)(elapsed_us * 255 / TRANSITION_US);

    uint8_t to_r = (target_.pattern == LedPattern::OFF) ? 0 : target_.r;
    uint8_t to_g = (target_.pattern == LedPattern::OFF) ? 0 : target_.g;
    uint8_t to_b = (target_.pattern == LedPattern::OFF) ? 0 : target_.b;

    setPixel(lerp8(from_r_, to_r, t),
             lerp8(from_g_, to_g, t),
             lerp8(from_b_, to_b, t));
}

void Led::updatePattern()
{
    if (target_.pattern == LedPattern::OFF ||
        target_.pattern == LedPattern::SOLID) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_ms = (now_us - last_update_us_) / 1000;

    if (target_.pattern == LedPattern::FADE) {
        uint16_t step_ms = LED_FADE_CYCLE_MS / 256;
        if (step_ms < 1) step_ms = 1;

        if (elapsed_ms >= step_ms) {
            last_update_us_ = now_us;
            breath_phase_++;
            uint8_t scale = breathCurve(breath_phase_);
            setPixel((target_.r * scale) / 255,
                     (target_.g * scale) / 255,
                     (target_.b * scale) / 255);
        }
        return;
    }

    // BLINK
    if (elapsed_ms >= LED_BLINK_INTERVAL_MS) {
        last_update_us_ = now_us;
        blink_state_ = !blink_state_;
        if (blink_state_) {
            setPixel(target_.r, target_.g, target_.b);
        } else {
            setPixel(0, 0, 0);
        }
    }
}

void Led::run()
{
    processCommands();
    if (transitioning_) {
        updateTransition();
    } else {
        updatePattern();
    }
    vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
}
