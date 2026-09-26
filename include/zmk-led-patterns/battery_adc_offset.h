/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* The measured sag at full LED duty is the upper bound. Round to the nearest
 * millivolt; an unlit LED needs no correction. */
static inline uint16_t led_pattern_battery_offset_mv(uint8_t applied_percent,
                                                      uint16_t full_duty_offset_mv) {
    const uint32_t duty = applied_percent > 100U ? 100U : applied_percent;
    return (uint16_t)((duty * full_duty_offset_mv + 50U) / 100U);
}

/* The source's voltage in millivolts plus the offset, the way the source
 * reports it: val1 whole volts, val2 microvolts. Saturates rather than wraps. */
static inline uint16_t led_pattern_battery_corrected_mv(int32_t val1, int32_t val2,
                                                        uint16_t offset_mv) {
    const int64_t mv = (int64_t)val1 * 1000 + val2 / 1000 + offset_mv;

    if (mv < 0) {
        return 0;
    }
    return mv > UINT16_MAX ? UINT16_MAX : (uint16_t)mv;
}

/*
 * A corrected voltage has to land on the percentage the source sensor itself
 * would report for it, so this maps millivolts the way ZMK's battery drivers
 * do.
 *
 * With thresholds - the DYA fork's zmk,battery-voltage-divider and its
 * mv-to-pct-thresholds - it interpolates linearly over them: the first entry
 * is 0%, the last 100%, and entry i of n is i / (n - 1). ZMK computes that
 * step in float and truncates; this does it in integers and truncates.
 *
 * With none (count 0) - upstream ZMK's divider, and nrf-vddh on either - it
 * is ZMK's lithium_ion_mv_to_pct: 0% at 3450 mV, 100% at 4200 mV, and the
 * integer line between.
 */
static inline uint8_t led_pattern_battery_mv_to_pct(uint16_t mv, const int16_t *thresholds,
                                                     size_t count) {
    if (count == 0) {
        if (mv >= 4200) {
            return 100;
        }
        if (mv <= 3450) {
            return 0;
        }
        return (uint8_t)(mv * 2 / 15 - 459);
    }

    if (mv < thresholds[0]) {
        return 0;
    }

    size_t i = 1;

    while (i < count && mv >= thresholds[i]) {
        i++;
    }
    if (i == count) {
        return 100;
    }

    const int64_t low = thresholds[i - 1];
    const int64_t span = thresholds[i] - low;

    return (uint8_t)(((int64_t)mv - low + (int64_t)(i - 1) * span) * 100 /
                     (span * (int64_t)(count - 1)));
}
