/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * &led_speed: a percentage within the published range, a step, or a preset.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk-led-patterns/led_pattern.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARG_UNUSED(x) ((void)(x))
#define LOG_WRN(fmt, ...) ((void)warnings++, (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define ZMK_BEHAVIOR_OPAQUE 0

struct zmk_behavior_binding { uint32_t param1; };
struct zmk_behavior_binding_event { int unused; };

static int warnings;
static bool ready = true;
static int32_t current = LED_PATTERN_SPEED_NOMINAL;
static int requests;
static int32_t requested = -1;

bool led_pattern_ready(void) { return ready; }
int32_t led_pattern_field_value(enum led_pattern_field field) {
    assert(field == LED_PATTERN_FIELD_SPEED);
    return current;
}
void led_pattern_request(enum led_pattern_field field, int32_t value) {
    assert(field == LED_PATTERN_FIELD_SPEED);
    requests++;
    requested = value;
}

/* DRIVER_FUNCTIONS */

static int32_t press(uint32_t param1) {
    struct zmk_behavior_binding binding = {.param1 = param1};
    requested = -1;
    assert(on_speed_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    return requested;
}

int main(void) {
    /* A percentage inside the published range is taken as it is. */
    assert(press(LED_PATTERN_SPEED_MIN) == LED_PATTERN_SPEED_MIN);
    assert(press(155) == 155 && press(LED_PATTERN_SPEED_MAX) == LED_PATTERN_SPEED_MAX);

    /* Presets. */
    assert(press(LED_SPEED_MIN) == LED_PATTERN_SPEED_MIN);
    assert(press(LED_SPEED_DEFAULT) == LED_PATTERN_SPEED_NOMINAL);
    assert(press(LED_SPEED_MAX) == LED_PATTERN_SPEED_MAX);

    /* Steps land on the tens and stop at the published bounds. */
    static const struct { int32_t from; int32_t up; int32_t down; } steps[] = {
        {10, 20, 10}, {15, 20, 10}, {100, 110, 90}, {395, 400, 390}, {400, 400, 390},
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        current = steps[i].from;
        assert(press(LED_SPEED_UP) == steps[i].up);
        assert(press(LED_SPEED_DOWN) == steps[i].down);
    }

    /* Below or above the range, or an unknown command: refused. */
    const int before = requests;
    assert(press(LED_PATTERN_SPEED_MIN - 1) == -1 && press(LED_PATTERN_SPEED_MAX + 1) == -1);
    assert(press(1000) == -1);
    assert(requests == before && warnings == 3);

    ready = false;
    struct zmk_behavior_binding binding = {.param1 = LED_SPEED_UP};
    assert(on_speed_pressed(&binding, (struct zmk_behavior_binding_event){0}) == -ENODEV);
    assert(on_speed_released(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    puts("&led_speed: PASS");
    return 0;
}
