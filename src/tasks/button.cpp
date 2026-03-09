#include "button.h"
#include "config.h"

#include "esp_timer.h"

static const char* TAG = "button";

// Internal state machine
enum class State {
    IDLE,
    PRESSED,
    RELEASED_WAIT,
    PRESSED_SECOND,
    RELEASED_SECOND,
    PRESSED_THIRD
};

static State s_state = State::IDLE;

static const char* stateName(State s)
{
    switch (s) {
        case State::IDLE: return "IDLE";
        case State::PRESSED: return "PRESSED";
        case State::RELEASED_WAIT: return "RELEASED_WAIT";
        case State::PRESSED_SECOND: return "PRESSED_SECOND";
        case State::RELEASED_SECOND: return "RELEASED_SECOND";
        case State::PRESSED_THIRD: return "PRESSED_THIRD";
        default: return "?";
    }
}

static const char* eventName(ButtonEvent e)
{
    switch (e) {
        case ButtonEvent::NONE: return "NONE";
        case ButtonEvent::SHORT_PRESS: return "SHORT_PRESS";
        case ButtonEvent::LONG_PRESS: return "LONG_PRESS";
        case ButtonEvent::EXTRA_LONG_PRESS: return "EXTRA_LONG_PRESS";
        case ButtonEvent::DOUBLE_CLICK: return "DOUBLE_CLICK";
        case ButtonEvent::TRIPLE_CLICK: return "TRIPLE_CLICK";
        case ButtonEvent::DOUBLE_LONG_CLICK: return "DOUBLE_LONG_CLICK";
        case ButtonEvent::FACTORY_RESET: return "FACTORY_RESET";
        default: return "?";
    }
}

static int64_t now_ms()
{
    return esp_timer_get_time() / US_PER_MS;
}

Button::Button(gpio_num_t pin, const ButtonConfig& config)
    : pin_(pin)
    , config_(config)
{
}

void Button::init()
{
    sem_ = xSemaphoreCreateBinaryStatic(&sem_buffer_);
    xSemaphoreGive(sem_);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    last_reading_ = gpio_get_level(pin_) != 0;
    stable_state_ = last_reading_;

    LOG(TAG, "Task started on core %d, GPIO%d", xPortGetCoreID(), pin_);
}

void Button::run()
{
    bool reading = gpio_get_level(pin_) != 0;
    int64_t now = now_ms();

    // Debounce
    if (reading != last_reading_) {
        debounce_time_ = now;
    }
    last_reading_ = reading;

    if ((now - debounce_time_) > config_.debounce_ms) {
        if (reading != stable_state_) {
            stable_state_ = reading;

            if (stable_state_ == false) {
                handlePress();
            } else {
                handleRelease();
            }
        }
    }

    checkLongPress();

    // Timeout for multi-click detection
    if (stable_state_ == true) {
        int64_t since_release = now - release_time_;

        if (s_state == State::RELEASED_WAIT && since_release > config_.click_window_ms) {
            setEvent(ButtonEvent::SHORT_PRESS);
            s_state = State::IDLE;
        }

        if (s_state == State::RELEASED_SECOND && since_release > config_.click_window_ms) {
            setEvent(ButtonEvent::DOUBLE_CLICK);
            s_state = State::IDLE;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_TICK_MS));
}

void Button::handlePress()
{
    press_time_ = now_ms();
    long_press_triggered_ = false;
    extra_long_press_triggered_ = false;
    factory_reset_triggered_ = false;

    State prev = s_state;
    switch (s_state) {
        case State::IDLE:
            s_state = State::PRESSED;
            break;
        case State::RELEASED_WAIT:
            s_state = State::PRESSED_SECOND;
            break;
        case State::RELEASED_SECOND:
            s_state = State::PRESSED_THIRD;
            break;
        default:
            break;
    }
    DBG(TAG, "Press: %s -> %s", stateName(prev), stateName(s_state));
}

void Button::handleRelease()
{
    release_time_ = now_ms();

    State prev = s_state;
    switch (s_state) {
        case State::PRESSED:
            if (long_press_triggered_ || extra_long_press_triggered_) {
                s_state = State::IDLE;
            } else {
                s_state = State::RELEASED_WAIT;
            }
            break;
        case State::PRESSED_SECOND:
            if (long_press_triggered_) {
                s_state = State::IDLE;
            } else {
                s_state = State::RELEASED_SECOND;
            }
            break;
        case State::PRESSED_THIRD:
            setEvent(ButtonEvent::TRIPLE_CLICK);
            s_state = State::IDLE;
            break;
        default:
            s_state = State::IDLE;
            break;
    }
    DBG(TAG, "Release: %s -> %s", stateName(prev), stateName(s_state));
}

void Button::checkLongPress()
{
    if (stable_state_ != false) return;

    int64_t hold_time = now_ms() - press_time_;

    if (!factory_reset_triggered_ && hold_time >= config_.factory_reset_ms) {
        factory_reset_triggered_ = true;
        setEvent(ButtonEvent::FACTORY_RESET);
        return;
    }

    if (!extra_long_press_triggered_ && hold_time >= config_.extra_long_press_ms) {
        extra_long_press_triggered_ = true;
        setEvent(ButtonEvent::EXTRA_LONG_PRESS);
        return;
    }

    if (!long_press_triggered_ && hold_time >= config_.long_press_ms) {
        long_press_triggered_ = true;

        if (s_state == State::PRESSED_SECOND) {
            setEvent(ButtonEvent::DOUBLE_LONG_CLICK);
        } else {
            setEvent(ButtonEvent::LONG_PRESS);
        }
    }
}

void Button::setEvent(ButtonEvent event)
{
    xSemaphoreTake(sem_, portMAX_DELAY);
    last_event_ = event;
    xSemaphoreGive(sem_);

    LOG(TAG, "Event: %s", eventName(event));
}

bool Button::handleState(ButtonEvent expected)
{
    xSemaphoreTake(sem_, portMAX_DELAY);
    bool match = (last_event_ == expected);
    if (match) {
        last_event_ = ButtonEvent::NONE;
    }
    xSemaphoreGive(sem_);
    return match;
}
