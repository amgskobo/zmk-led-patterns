/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <zmk/event_manager.h>

/* Accepted desired values. A split's visible reservation may still be pending.
 * led_pattern_applied_brightness() separately reports the last local PWM level.
 * Rendering a reservation never publishes an older desired-state event. */
struct zmk_led_pattern_state_changed {
    uint8_t pattern;
    uint8_t brightness;
    uint16_t speed;
};

ZMK_EVENT_DECLARE(zmk_led_pattern_state_changed);
