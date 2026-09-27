/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <zmk-led-patterns/battery_adc_offset.h>

/* ZMK's mv_to_pct_linear_interpolation (app/module/drivers/sensor/battery/
 * battery_common.c), float arithmetic included: the reference the integer
 * version has to match. */
static uint8_t zmk_reference(int16_t bat_mv, const int16_t *mv_thresholds,
                             size_t mv_thresholds_size) {
    if (bat_mv < mv_thresholds[0]) {
        return 0;
    }
    if (bat_mv >= mv_thresholds[mv_thresholds_size - 1]) {
        return 100;
    }
    for (size_t i = 1; i < mv_thresholds_size; i++) {
        if (bat_mv < mv_thresholds[i]) {
            int low = mv_thresholds[i - 1];
            int high = mv_thresholds[i];
            float deno = (float)(((bat_mv - low) + (int)(i - 1) * (high - low)) * 100);
            return (uint8_t)(deno / (float)((high - low) * (int)(mv_thresholds_size - 1)));
        }
    }
    return 100;
}

/* ZMK's lithium_ion_mv_to_pct, the mapping of upstream's divider and nrf-vddh. */
static uint8_t zmk_lithium_reference(int16_t bat_mv) {
    if (bat_mv >= 4200) {
        return 100;
    } else if (bat_mv <= 3450) {
        return 0;
    }

    return (uint8_t)(bat_mv * 2 / 15 - 459);
}

static void matches_reference(const int16_t *thresholds, size_t count) {
    for (int mv = 0; mv <= 5000; mv++) {
        assert(led_pattern_battery_mv_to_pct((uint16_t)mv, thresholds, count) ==
               zmk_reference((int16_t)mv, thresholds, count));
    }
}

int main(void) {
    assert(led_pattern_battery_offset_mv(0, 50) == 0);
    assert(led_pattern_battery_offset_mv(1, 50) == 1);
    assert(led_pattern_battery_offset_mv(50, 50) == 25);
    assert(led_pattern_battery_offset_mv(99, 50) == 50);
    assert(led_pattern_battery_offset_mv(100, 50) == 50);
    assert(led_pattern_battery_offset_mv(255, 50) == 50);
    assert(led_pattern_battery_offset_mv(100, 0) == 0);
    /* Clamped to 100%, not 99%: at 1000 mV the two differ by 10 mV. */
    assert(led_pattern_battery_offset_mv(100, 1000) == 1000);
    assert(led_pattern_battery_offset_mv(255, 1000) == 1000);

    assert(led_pattern_battery_corrected_mv(3, 975000, 0) == 3975);
    assert(led_pattern_battery_corrected_mv(3, 975000, 50) == 4025);
    assert(led_pattern_battery_corrected_mv(3, 975999, 0) == 3975);
    assert(led_pattern_battery_corrected_mv(0, 0, 0) == 0);
    assert(led_pattern_battery_corrected_mv(-1, 0, 50) == 0);
    assert(led_pattern_battery_corrected_mv(65, 535000, 0) == 65535);
    assert(led_pattern_battery_corrected_mv(65, 535000, 1) == 65535);
    assert(led_pattern_battery_corrected_mv(INT32_MAX, 999999, 1000) == 65535);

    /* The divider binding's default, which akkb46 and most boards use. */
    const int16_t lipo[] = {3450, 4200};
    assert(led_pattern_battery_mv_to_pct(3449, lipo, 2) == 0);
    assert(led_pattern_battery_mv_to_pct(3450, lipo, 2) == 0);
    assert(led_pattern_battery_mv_to_pct(3825, lipo, 2) == 50);
    assert(led_pattern_battery_mv_to_pct(4199, lipo, 2) == 99);
    assert(led_pattern_battery_mv_to_pct(4200, lipo, 2) == 100);
    assert(led_pattern_battery_mv_to_pct(UINT16_MAX, lipo, 2) == 100);
    matches_reference(lipo, 2);

    /* A curve with uneven steps, a single point and a repeated point. */
    const int16_t curve[] = {3300, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4200};
    assert(led_pattern_battery_mv_to_pct(3600, curve, 9) == 12);
    matches_reference(curve, 9);
    const int16_t single[] = {3700};
    assert(led_pattern_battery_mv_to_pct(3699, single, 1) == 0);
    assert(led_pattern_battery_mv_to_pct(3700, single, 1) == 100);
    matches_reference(single, 1);
    const int16_t repeated[] = {3400, 3800, 3800, 4200};
    assert(led_pattern_battery_mv_to_pct(3800, repeated, 4) == 66);
    matches_reference(repeated, 4);

    /* No thresholds: ZMK's fixed lithium-ion line. It differs from the
     * default thresholds' interpolation - 51% against 50% at 3825 mV. */
    assert(led_pattern_battery_mv_to_pct(3450, NULL, 0) == 0);
    assert(led_pattern_battery_mv_to_pct(3451, NULL, 0) == 1);
    assert(led_pattern_battery_mv_to_pct(3825, NULL, 0) == 51);
    assert(led_pattern_battery_mv_to_pct(4199, NULL, 0) == 100);
    assert(led_pattern_battery_mv_to_pct(4200, NULL, 0) == 100);
    assert(led_pattern_battery_mv_to_pct(UINT16_MAX, NULL, 0) == 100);
    for (int mv = 0; mv <= 5000; mv++) {
        assert(led_pattern_battery_mv_to_pct((uint16_t)mv, NULL, 0) ==
               zmk_lithium_reference((int16_t)mv));
    }

    puts("LED battery helpers: PASS");
    return 0;
}
