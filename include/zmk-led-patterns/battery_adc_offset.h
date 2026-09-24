/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* The measured sag at full LED duty is the upper bound. Round to the nearest
 * millivolt; an unlit LED needs no correction. */
static inline uint16_t led_pattern_battery_offset_mv(uint8_t applied_percent,
                                                      uint16_t full_duty_offset_mv) {
    const uint32_t duty = applied_percent > 100U ? 100U : applied_percent;
    return (uint16_t)((duty * full_duty_offset_mv + 50U) / 100U);
}
