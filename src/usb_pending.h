/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Small, Zephyr-free state machine for USB notification coalescing. Callers
 * protect an instance with their own lock; no RPC or flash operation belongs
 * inside that lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk-led-patterns/led_pattern.h>

struct led_pattern_usb_pending {
    uint32_t version[LED_PATTERN_FIELD_SPEED + 1];
    uint8_t mask;
};

static inline uint8_t led_pattern_usb_field_bit(enum led_pattern_field field) {
    return (uint8_t)(1U << field);
}

static inline void led_pattern_usb_pending_mark(struct led_pattern_usb_pending *pending,
                                                enum led_pattern_field field) {
    pending->version[field]++;
    pending->mask |= led_pattern_usb_field_bit(field);
}

static inline bool led_pattern_usb_pending_peek(const struct led_pattern_usb_pending *pending,
                                                enum led_pattern_field field, uint32_t *version) {
    if ((pending->mask & led_pattern_usb_field_bit(field)) == 0U) {
        return false;
    }
    *version = pending->version[field];
    return true;
}

/* True means a newer change (or a failed event dispatch) still needs a retry.
 * Dispatch success does not acknowledge the Studio queue or transport. */
static inline bool led_pattern_usb_pending_finish(struct led_pattern_usb_pending *pending,
                                                  enum led_pattern_field field, uint32_t version,
                                                  bool dispatch_succeeded) {
    if (dispatch_succeeded && pending->version[field] == version) {
        pending->mask &= (uint8_t)~led_pattern_usb_field_bit(field);
        return false;
    }
    return true;
}

static inline bool led_pattern_usb_port_closed(bool selected_usb, bool uart_ready,
                                               bool dtr_read_succeeded, uint32_t dtr) {
    return selected_usb && (!uart_ready || !dtr_read_succeeded || dtr == 0U);
}
