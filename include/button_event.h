#pragma once

#include <stdint.h>

enum class ButtonEvent : uint8_t {
    NONE,
    SHORT_PRESS,
    LONG_PRESS,
    EXTRA_LONG_PRESS,
    DOUBLE_CLICK,
    DOUBLE_LONG_CLICK,
    TRIPLE_CLICK,
    FACTORY_RESET
};
