/*
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

struct led_pattern_state {
    /* LED_PATTERN_STEADY .. LED_PATTERN_FADE_BLINK. */
    uint8_t pattern;
    /* Percent of the pattern's nominal rate; 100 is the rate it was drawn at. */
    uint16_t speed;
    /* Whether an unconnected central overrides the pattern with its blink. */
    bool advertising_indicator;
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
