/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "usb_pending.h"

static void test_latest_value_and_independent_fields(void) {
    struct led_pattern_usb_pending pending = {0};
    int32_t value = -1;
    uint32_t version = 0;

    assert(!led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_PATTERN, &value, &version));
    for (int32_t i = 0; i < 1000; i++) {
        led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_BRIGHTNESS, i % 101);
    }
    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_PATTERN, 7);
    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_SPEED, 120);

    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_BRIGHTNESS, &value, &version));
    assert(value == 90 && version == 1000);
    assert(!led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_BRIGHTNESS, version, true));
    assert(!led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_BRIGHTNESS, &value, &version));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_PATTERN, &value, &version));
    assert(value == 7);
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &value, &version));
    assert(value == 120);
}

static void test_write_race_and_retry(void) {
    struct led_pattern_usb_pending pending = {0};
    int32_t value;
    uint32_t version;

    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_SPEED, 110);
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &value, &version));
    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_SPEED, 130);
    assert(led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, true));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &value, &version));
    assert(value == 130);
    assert(led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, false));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &value, &version));
    assert(!led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, true));
    assert(pending.mask == 0);
}

static void test_overlay_and_port_state(void) {
    struct led_pattern_usb_pending pending = {0};
    struct led_pattern_state state = {.pattern = 1, .speed = 100};
    uint8_t brightness = 20;

    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_PATTERN, 5);
    led_pattern_usb_pending_put(&pending, LED_PATTERN_FIELD_BRIGHTNESS, 80);
    led_pattern_usb_pending_overlay(&pending, &state, &brightness);
    assert(state.pattern == 5 && state.speed == 100 && brightness == 80);

    assert(!led_pattern_usb_port_closed(false, false, false, 0));
    assert(led_pattern_usb_port_closed(true, false, false, 0));
    assert(led_pattern_usb_port_closed(true, true, false, 0));
    assert(led_pattern_usb_port_closed(true, true, true, 0));
    assert(!led_pattern_usb_port_closed(true, true, true, 1));
}

int main(void) {
    test_latest_value_and_independent_fields();
    test_write_race_and_retry();
    test_overlay_and_port_state();
    puts("usb_pending: all checks passed");
    return 0;
}
