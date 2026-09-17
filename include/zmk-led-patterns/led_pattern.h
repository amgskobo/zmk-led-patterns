/*
 * Copyright (c) 2025-2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * The whole of what this module controls, as one struct.
 *
 * Two scalars and a switch form the persistent pattern state. Brightness is
 * kept here rather than delegated to ZMK's backlight subsystem, so that one
 * key press results in one split message: `&bl` has a global relay of its own,
 * and driving both would put two senders on the split link for a single
 * press.
 *
 * Handing the state over as one struct is what lets every entry point -- a
 * keymap binding, a relayed split command, a value edited in a Studio client
 * -- drive the same code with no path of its own.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dt-bindings/zmk/led_pattern.h>

/* Bounds every entry point clamps to, and the range the setting publishes. */
#define LED_PATTERN_SPEED_MIN 10
#define LED_PATTERN_SPEED_NOMINAL 100
#define LED_PATTERN_SPEED_MAX 400

/* How far &led_brightness's and &led_speed's down and up move, in points. */
#define LED_PATTERN_BRIGHTNESS_STEP 10
#define LED_PATTERN_SPEED_STEP 10

struct led_pattern_state {
    /* LED_PATTERN_STEADY .. LED_PATTERN_FADE_BLINK. */
    uint8_t pattern;
    /* Percent of the pattern's nominal rate; 100 is the rate it was drawn at. */
    uint16_t speed;
    /* Whether an unconnected central overrides the pattern with its blink. */
    bool advertising_indicator;
    /* Whether the LED goes dark once ZMK reports the keyboard idle. Clearing
     * it keeps the animation running, which is the one setting here that
     * costs real battery: it is what stops the core from being left alone. */
    bool idle_off;
};

/* Never fails; the controller has one static instance and no probe step. */
void led_pattern_get_state(struct led_pattern_state *out);

/*
 * Applies a whole state, clamping each field into range.
 *
 * Restarts the animation clock only when the pattern itself changes, so
 * dragging a speed control in a client does not make the LED stutter back to
 * the start of its cycle on every value that arrives.
 *
 * This is the one way in, for every caller: a keymap binding, a command
 * relayed from a split central, and a value read out of custom settings all
 * arrive here, so there is a single place that clamps and a single place that
 * decides whether the animation restarts.
 */
void led_pattern_set_state(const struct led_pattern_state *state);
uint8_t led_pattern_get_brightness(void);
void led_pattern_set_brightness(uint8_t brightness);

/* The three values the behaviors edit, one behavior each. */
enum led_pattern_field {
    LED_PATTERN_FIELD_PATTERN,
    LED_PATTERN_FIELD_BRIGHTNESS,
    LED_PATTERN_FIELD_SPEED,
};

/* Whether the LED can be driven at all. A behavior does nothing otherwise. */
bool led_pattern_ready(void);

/* What the LED is showing now for one field: the value a step starts from. */
int32_t led_pattern_field_value(enum led_pattern_field field);

/*
 * How every behavior asks for a value.
 *
 * With CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS the value becomes the setting of
 * the transport the keyboard is on now, and reaches the LED through that
 * setting's change event: the setting stays the one owner of the value, and a
 * client shows the change at once. Without it, the value is applied directly.
 */
void led_pattern_request(enum led_pattern_field field, int32_t value);

/*
 * The settings half of led_pattern_request(), defined only when that option is
 * on. Writes the live transport's setting in memory and schedules it to flash.
 * Returns 0 once the value is the live setting, or a negative error, in which
 * case the caller applies the value directly instead.
 */
int led_pattern_settings_store(enum led_pattern_field field, int32_t value);
