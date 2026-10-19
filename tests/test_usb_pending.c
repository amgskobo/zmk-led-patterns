/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "usb_pending.h"

static void test_latest_notification_and_independent_fields(void) {
    struct led_pattern_usb_pending pending = {0};
    uint32_t version = 0;

    assert(!led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_PATTERN, &version));
    for (int32_t i = 0; i < 1000; i++) {
        led_pattern_usb_pending_mark(&pending, LED_PATTERN_FIELD_BRIGHTNESS);
    }
    led_pattern_usb_pending_mark(&pending, LED_PATTERN_FIELD_PATTERN);
    led_pattern_usb_pending_mark(&pending, LED_PATTERN_FIELD_SPEED);

    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_BRIGHTNESS, &version));
    assert(version == 1000);
    assert(!led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_BRIGHTNESS, version, true));
    assert(!led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_BRIGHTNESS, &version));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_PATTERN, &version));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &version));
}

static void test_write_race_and_retry(void) {
    struct led_pattern_usb_pending pending = {0};
    uint32_t version;

    led_pattern_usb_pending_mark(&pending, LED_PATTERN_FIELD_SPEED);
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &version));
    led_pattern_usb_pending_mark(&pending, LED_PATTERN_FIELD_SPEED);
    assert(led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, true));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &version));
    assert(led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, false));
    assert(led_pattern_usb_pending_peek(&pending, LED_PATTERN_FIELD_SPEED, &version));
    assert(!led_pattern_usb_pending_finish(&pending, LED_PATTERN_FIELD_SPEED, version, true));
    assert(pending.mask == 0);
}

static void test_port_state(void) {
    assert(!led_pattern_usb_port_closed(false, false, false, 0));
    assert(led_pattern_usb_port_closed(true, false, false, 0));
    assert(led_pattern_usb_port_closed(true, true, false, 0));
    assert(led_pattern_usb_port_closed(true, true, true, 0));
    assert(!led_pattern_usb_port_closed(true, true, true, 1));
    assert(led_pattern_usb_port_closed(true, false, true, 1));
    assert(led_pattern_usb_port_closed(true, true, false, 1));
}

static uint32_t next_random(uint32_t *seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

static void test_deterministic_interleavings(void) {
    struct led_pattern_usb_pending pending = {0};
    uint32_t expected_version[3] = {0};
    bool expected_dirty[3] = {false};
    uint32_t seed = 0x4217211EU;

    for (int i = 0; i < 100000; i++) {
        enum led_pattern_field field = (enum led_pattern_field)(next_random(&seed) % 3U);
        uint32_t action = next_random(&seed) % 3U;
        if (action == 0U) {
            /* Retain the archived input schedule's value draw. The registry
             * now owns values; this helper owns only dispatch generations. */
            (void)next_random(&seed);
            led_pattern_usb_pending_mark(&pending, field);
            expected_version[field]++;
            expected_dirty[field] = true;
        } else {
            uint32_t version = 0;
            bool found = led_pattern_usb_pending_peek(&pending, field, &version);
            assert(found == expected_dirty[field]);
            if (!found) {
                continue;
            }
            assert(version == expected_version[field]);
            if (action == 2U) {
                /* A new key press lands while this older notification is being emitted. */
                (void)next_random(&seed);
                led_pattern_usb_pending_mark(&pending, field);
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
            uint32_t version = 0;
            assert(led_pattern_usb_pending_peek(&pending, (enum led_pattern_field)f,
                                                &version) == expected_dirty[f]);
            if (expected_dirty[f]) {
                assert(version == expected_version[f]);
            }
        }
    }
}

int main(void) {
    test_latest_notification_and_independent_fields();
    test_write_race_and_retry();
    test_port_state();
    test_deterministic_interleavings();
    puts("usb_pending: all checks passed");
    return 0;
}
