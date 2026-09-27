/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * &led_brightness: a percentage, or a step up or down to the next ten.
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
static int32_t current = 50;
static int requests;
static int32_t requested = -1;

bool led_pattern_ready(void) { return ready; }
int32_t led_pattern_field_value(enum led_pattern_field field) {
    assert(field == LED_PATTERN_FIELD_BRIGHTNESS);
    return current;
}
void led_pattern_request(enum led_pattern_field field, int32_t value) {
    assert(field == LED_PATTERN_FIELD_BRIGHTNESS);
    requests++;
    requested = value;
}

/* DRIVER_FUNCTIONS */

static int32_t press(uint32_t param1) {
    struct zmk_behavior_binding binding = {.param1 = param1};
    requested = -1;
    assert(on_brightness_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    return requested;
}

int main(void) {
    /* A percentage is taken as it is, up to 100. */
    assert(press(0) == 0 && press(73) == 73 && press(100) == 100);

    /* Steps land on the tens, and stop at either end. */
    static const struct { int32_t from; int32_t up; int32_t down; } steps[] = {
        {0, 10, 0}, {5, 10, 0}, {10, 20, 0}, {95, 100, 90}, {100, 100, 90},
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        current = steps[i].from;
        assert(press(LED_BRIGHTNESS_UP) == steps[i].up);
        assert(press(LED_BRIGHTNESS_DOWN) == steps[i].down);
    }

    /* Anything else is refused with a warning and requests nothing. */
    const int before = requests;
    assert(press(101) == -1 && press(LED_SPEED_UP) == -1);
    assert(requests == before && warnings == 2);

    /* No LED yet: the press is not handled. */
    ready = false;
    struct zmk_behavior_binding binding = {.param1 = 50};
    assert(on_brightness_pressed(&binding, (struct zmk_behavior_binding_event){0}) == -ENODEV);
    assert(on_brightness_released(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    puts("&led_brightness: PASS");
    return 0;
}
