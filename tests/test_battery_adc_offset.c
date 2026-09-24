/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <zmk-led-patterns/battery_adc_offset.h>

int main(void) {
    assert(led_pattern_battery_offset_mv(0, 50) == 0);
    assert(led_pattern_battery_offset_mv(1, 50) == 1);
    assert(led_pattern_battery_offset_mv(50, 50) == 25);
    assert(led_pattern_battery_offset_mv(99, 50) == 50);
    assert(led_pattern_battery_offset_mv(100, 50) == 50);
    assert(led_pattern_battery_offset_mv(255, 50) == 50);
    assert(led_pattern_battery_offset_mv(100, 0) == 0);
    return 0;
}
