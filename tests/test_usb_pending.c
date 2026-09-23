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

    struct led_pattern_usb_pending empty = {0};
    led_pattern_usb_pending_overlay(&empty, &state, &brightness);
    assert(state.pattern == 5 && state.speed == 100 && brightness == 80);
    led_pattern_usb_pending_put(&empty, LED_PATTERN_FIELD_SPEED, 175);
    led_pattern_usb_pending_overlay(&empty, &state, &brightness);
    assert(state.pattern == 5 && state.speed == 175 && brightness == 80);

    assert(!led_pattern_usb_port_closed(false, false, false, 0));
    assert(led_pattern_usb_port_closed(true, false, false, 0));
    assert(led_pattern_usb_port_closed(true, true, false, 0));
    assert(led_pattern_usb_port_closed(true, true, true, 0));
    assert(!led_pattern_usb_port_closed(true, true, true, 1));
}

static uint32_t next_random(uint32_t *seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

static void test_deterministic_interleavings(void) {
    struct led_pattern_usb_pending pending = {0};
    int32_t expected_value[3] = {0};
    uint32_t expected_version[3] = {0};
    bool expected_dirty[3] = {false};
    uint32_t seed = 0x4217211EU;

    for (int i = 0; i < 100000; i++) {
        enum led_pattern_field field = (enum led_pattern_field)(next_random(&seed) % 3U);
        uint32_t action = next_random(&seed) % 3U;
        if (action == 0U) {
            int32_t value = (int32_t)(next_random(&seed) % 201U);
            led_pattern_usb_pending_put(&pending, field, value);
            expected_value[field] = value;
            expected_version[field]++;
            expected_dirty[field] = true;
        } else {
            int32_t value = -1;
            uint32_t version = 0;
            bool found = led_pattern_usb_pending_peek(&pending, field, &value, &version);
            assert(found == expected_dirty[field]);
            if (!found) {
                continue;
            }
            assert(value == expected_value[field] && version == expected_version[field]);
            if (action == 2U) {
                /* A new key press lands while this older value is being written. */
                int32_t newer = (int32_t)(next_random(&seed) % 201U);
                led_pattern_usb_pending_put(&pending, field, newer);
                expected_value[field] = newer;
                expected_version[field]++;
                expected_dirty[field] = true;
            }
            bool success = (next_random(&seed) & 1U) != 0U;
            bool retry = led_pattern_usb_pending_finish(&pending, field, version, success);
            bool should_retry = !success || expected_version[field] != version;
            assert(retry == should_retry);
            if (!should_retry) {
                expected_dirty[field] = false;
            }
        }
        for (int f = 0; f < 3; f++) {
            int32_t value = -1;
            uint32_t version = 0;
            assert(led_pattern_usb_pending_peek(&pending, (enum led_pattern_field)f,
                                                &value, &version) == expected_dirty[f]);
            if (expected_dirty[f]) {
                assert(value == expected_value[f] && version == expected_version[f]);
            }
        }
    }
}

int main(void) {
    test_latest_value_and_independent_fields();
    test_write_race_and_retry();
    test_overlay_and_port_state();
    test_deterministic_interleavings();
    puts("usb_pending: all checks passed");
    return 0;
}
