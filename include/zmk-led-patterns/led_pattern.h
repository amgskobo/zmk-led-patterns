/*
 * Copyright (c) 2025-2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * The whole of what this module controls, as one struct.
 *
 * Pattern, speed, advertising indication and idle policy form the persistent
 * pattern state. Brightness is
 * kept here rather than delegated to ZMK's backlight subsystem, so every
 * accepted key intent has one LED state owner. `&bl` has a global relay of its
 * own; driving both would duplicate state senders. The LED owner coalesces
 * intents into bounded clock/reservation/acknowledgement transactions.
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
     * this keeps normal animation active. Continuous animation at nonzero
     * brightness can increase power use. Actual current depends on hardware
     * and settings and is not measured here. */
    bool idle_off;
};

/* Latest accepted desired state. On a split it is observable immediately;
 * visible presentation changes at a validated future reservation. */
void led_pattern_get_state(struct led_pattern_state *out);

/*
 * Accepts a whole desired state, clamping each field into range.
 *
 * With a reusable fresh clock, speed/brightness edits preserve animation phase.
 * A pattern change or stale-clock/refit/recovery can deliberately restart phase
 * at the validated common future edge. No-op setters do not request sync.
 *
 * Standalone output follows immediately. A split uses the central's logical
 * clock and future reservations on both halves; delivery loss can leave a
 * running reservation unconfirmed. Idle presentations keep free-running after
 * clock quality expires, explicitly COASTING with no periodic LED radio work.
 * A later effective visual change requests bounded fresh synchronization as
 * needed. Registry values and getters remain immediate. No global kernel
 * uptime is adjusted.
 *
 * This is the whole-state way in: a keymap binding, a command
 * relayed from a split central, and a value read out of custom settings all
 * arrive here, so there is a single place that clamps and a single place that
 * decides whether the animation restarts.
 */
void led_pattern_set_state(const struct led_pattern_state *state);
/* Accept one complete desired snapshot, including brightness. A split creates
 * one reservation from the complete snapshot; independent setters above/below
 * continue to preserve fields outside their own argument. */
void led_pattern_set_presentation(const struct led_pattern_state *state, uint8_t brightness);
uint8_t led_pattern_get_brightness(void);
/* Last brightness successfully written to the local LED, including pattern
 * phase and temporary status indicators. Safe to read from a sensor driver. */
uint8_t led_pattern_applied_brightness(void);
void led_pattern_set_brightness(uint8_t brightness);

/* The three values the behaviors edit, one behavior each. */
enum led_pattern_field {
    LED_PATTERN_FIELD_PATTERN,
    LED_PATTERN_FIELD_BRIGHTNESS,
    LED_PATTERN_FIELD_SPEED,
};

/* Whether the LED can be driven at all. A behavior does nothing otherwise. */
bool led_pattern_ready(void);

/* Latest desired field value, which a repeated step starts from. This is
 * distinct from the last successful local PWM level above. */
int32_t led_pattern_field_value(enum led_pattern_field field);

/*
 * How every behavior asks for a value.
 *
 * With CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS the value becomes the setting
 * of the current transport immediately on both BLE and USB. USB coalesces the
 * Studio notification dispatch to limit bursts; this does not acknowledge
 * transport delivery. Without settings or on a store error, the value is
 * applied directly.
 */
void led_pattern_request(enum led_pattern_field field, int32_t value);

/*
 * The settings half of led_pattern_request(), defined only when that option is
 * on. Writes the live transport's setting in memory and schedules it to flash.
 * Returns 0 once the value is the live setting, or a negative error, in which
 * case the caller applies the value directly. Only USB notifications and flash
 * persistence are deferred; no old pending value is replayed into the registry.
 */
int led_pattern_settings_store(enum led_pattern_field field, int32_t value);
