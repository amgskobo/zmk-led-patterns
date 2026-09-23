/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Small, Zephyr-free state machine for USB key-repeat coalescing. Callers
 * protect an instance with their own lock; no RPC or flash operation belongs
 * inside that lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk-led-patterns/led_pattern.h>

struct led_pattern_usb_pending {
    int32_t value[LED_PATTERN_FIELD_SPEED + 1];
    uint32_t version[LED_PATTERN_FIELD_SPEED + 1];
    uint8_t mask;
};

static inline uint8_t led_pattern_usb_field_bit(enum led_pattern_field field) {
    return (uint8_t)(1U << field);
}

static inline void led_pattern_usb_pending_put(struct led_pattern_usb_pending *pending,
                                               enum led_pattern_field field, int32_t value) {
    pending->value[field] = value;
    pending->version[field]++;
    pending->mask |= led_pattern_usb_field_bit(field);
}

static inline bool led_pattern_usb_pending_peek(const struct led_pattern_usb_pending *pending,
                                                enum led_pattern_field field, int32_t *value,
                                                uint32_t *version) {
    if ((pending->mask & led_pattern_usb_field_bit(field)) == 0U) {
        return false;
    }
    *value = pending->value[field];
    *version = pending->version[field];
    return true;
}

/* True means a newer value (or a failed write) still needs a retry. */
static inline bool led_pattern_usb_pending_finish(struct led_pattern_usb_pending *pending,
                                                  enum led_pattern_field field, uint32_t version,
                                                  bool write_succeeded) {
    if (write_succeeded && pending->version[field] == version) {
        pending->mask &= (uint8_t)~led_pattern_usb_field_bit(field);
        return false;
    }
    return true;
}

static inline void led_pattern_usb_pending_overlay(const struct led_pattern_usb_pending *pending,
                                                   struct led_pattern_state *state,
                                                   uint8_t *brightness) {
    if ((pending->mask & led_pattern_usb_field_bit(LED_PATTERN_FIELD_PATTERN)) != 0U) {
        state->pattern = (uint8_t)pending->value[LED_PATTERN_FIELD_PATTERN];
    }
    if ((pending->mask & led_pattern_usb_field_bit(LED_PATTERN_FIELD_BRIGHTNESS)) != 0U) {
        *brightness = (uint8_t)pending->value[LED_PATTERN_FIELD_BRIGHTNESS];
    }
    if ((pending->mask & led_pattern_usb_field_bit(LED_PATTERN_FIELD_SPEED)) != 0U) {
        state->speed = (uint16_t)pending->value[LED_PATTERN_FIELD_SPEED];
    }
}

static inline bool led_pattern_usb_port_closed(bool selected_usb, bool uart_ready,
                                               bool dtr_read_succeeded, uint32_t dtr) {
    return selected_usb && (!uart_ready || !dtr_read_succeeded || dtr == 0U);
}
