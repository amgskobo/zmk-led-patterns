/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <zmk/event_manager.h>

/* The applied values, not the behavior command or a split transport event. */
struct zmk_led_pattern_state_changed {
    uint8_t pattern;
    uint8_t brightness;
    uint16_t speed;
};

ZMK_EVENT_DECLARE(zmk_led_pattern_state_changed);
