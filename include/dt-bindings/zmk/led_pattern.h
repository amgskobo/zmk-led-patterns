/*
 * SPDX-License-Identifier: MIT
 *
 * The one parameter of &led_pattern, and everything it can say.
 *
 * Only what ZMK has no equivalent of. Brightness and on/off are not here:
 * those are `&bl` (BL_INC, BL_DEC, BL_TOG, BL_SET), ZMK's own backlight
 * behavior, which already persists its value, relays it to both split halves
 * and draws itself properly in a Studio keymap editor. This module scales its
 * patterns by whatever brightness that subsystem is holding.
 *
 * What is left is the shape of the animation and how fast it runs, told apart
 * by which band the number falls in:
 *
 *     0 - 17      select this exact pattern
 *     100 - 103   previous / next pattern, brightness down / up
 *     410 - 800   set the speed to an exact percentage
 *
 * The absolute speed band exists because a split central has to be able to
 * tell a peripheral what the state now *is*, not how it changed: "next
 * pattern" only keeps two halves together for as long as both started from the
 * same place. It is an ordinary keymap parameter too.
 */

#pragma once

/* Argument for &led_pattern: select this exact pattern. */
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

/* Named actions are outside the pattern-number range (0-17). */
#define LED_PATTERN_PREVIOUS 100
#define LED_PATTERN_NEXT     101
#define LED_PATTERN_BRIGHTNESS_UP 102
#define LED_PATTERN_BRIGHTNESS_DOWN 103

/* Absolute speed. pct is 10-400: a tenth of the drawn rate through four times
 * it. The argument is a percentage in the same units the setting uses, so a
 * relayed command and an edited value are the same number. */
#define LED_PATTERN_SPEED_BASE 400
#define LED_PATTERN_SPEED(pct) (LED_PATTERN_SPEED_BASE + (pct))
