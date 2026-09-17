/*
 * Copyright (c) 2025-2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * The parameters of the three LED behaviors, one setting each.
 *
 *     &led_pattern     0 - 17          select this exact pattern
 *                      100, 101        previous / next pattern, wrapping around
 *     &led_brightness  0 - 100         set the brightness to this percentage
 *                      200, 201        ten down / up, between 0 and 100
 *     &led_speed       10 - 400        set the speed to this percentage
 *                      500, 501        ten down / up, between 10 and 400
 *                      502, 503, 504   the minimum, default and maximum speed
 *
 * A binding edits the setting its behavior is named after, for the transport
 * the keyboard is on now, so a key and a value edited in a client are the same
 * value. This module owns the LED, so brightness lives here rather than on
 * ZMK's `&bl`, which belongs to a backlight subsystem this module does not use.
 */

#pragma once

/* &led_pattern: select this exact pattern. */
#define LED_PATTERN_STEADY        0
#define LED_PATTERN_BREATHE       1
#define LED_PATTERN_HEARTBEAT     2
#define LED_PATTERN_BLINK         3
#define LED_PATTERN_FAST_BLINK    4
#define LED_PATTERN_TRIPLE_FLASH  5
#define LED_PATTERN_SOS           6
#define LED_PATTERN_CANDLE        7
#define LED_PATTERN_SAWTOOTH      8
#define LED_PATTERN_BEACON        9
#define LED_PATTERN_SLOW_BREATHE  10
#define LED_PATTERN_STROBE        11
#define LED_PATTERN_DOUBLE_BEACON 12
#define LED_PATTERN_LONG_FLASH    13
#define LED_PATTERN_SPARKLE       14
#define LED_PATTERN_COUNTDOWN     15
#define LED_PATTERN_RIPPLE        16
#define LED_PATTERN_FADE_BLINK    17

/* One past the last pattern. Add a pattern by raising this and adding its
 * curve; the cycling commands and the settings range both follow. */
#define LED_PATTERN_COUNT 18

/* &led_pattern: step to the previous or next pattern. Outside 0-17. */
#define LED_PATTERN_PREVIOUS 100
#define LED_PATTERN_NEXT     101

/* &led_brightness: ten points down or up. Outside 0-100, where every value
 * sets that percentage. Down reaches 0, the LED's low-power state: the PWM
 * output stops and nothing is redrawn until the brightness rises again. */
#define LED_BRIGHTNESS_DOWN 200
#define LED_BRIGHTNESS_UP   201

/* &led_speed: ten points down or up, and the three fixed speeds -- 10, 100 and
 * 400 percent of the rate each pattern was drawn at. Outside 10-400, where
 * every value sets that percentage. */
#define LED_SPEED_DOWN    500
#define LED_SPEED_UP      501
#define LED_SPEED_MIN     502
#define LED_SPEED_DEFAULT 503
#define LED_SPEED_MAX     504
