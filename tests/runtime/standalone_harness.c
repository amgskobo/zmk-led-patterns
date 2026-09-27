/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern.c built for a keyboard with no split, with BLE and
 * custom settings.
 */
#define CONFIG_ZMK_BLE 1
#define CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS 1
#include "led_stubs.h"

/* DRIVER_FUNCTIONS */

static void activity(enum zmk_activity_state state) {
    const struct zmk_activity_state_changed event = {{EVENT_ACTIVITY}, state};
    assert(led_pattern_activity_listener(&event.header) == ZMK_EV_EVENT_BUBBLE);
}

static void set_state(uint8_t pattern, uint16_t speed, bool indicator, bool off_when_idle) {
    const struct led_pattern_state state = {pattern, speed, indicator, off_when_idle};
    led_pattern_set_state(&state);
}

static int press(uint32_t param1) {
    struct zmk_behavior_binding binding = {.param1 = param1};
    return on_pattern_pressed(&binding, (struct zmk_behavior_binding_event){0});
}

/* The ramps, where the level moves between scheduled samples. */
static bool on_ramp(uint8_t pattern, int64_t t) {
    return pattern == LED_PATTERN_BREATHE || pattern == LED_PATTERN_SAWTOOTH ||
           (pattern == LED_PATTERN_SLOW_BREATHE && t % 16000 < 6000) ||
           (pattern == LED_PATTERN_FADE_BLINK && t % 1200 < 300);
}

static void test_curves(void) {
    static const uint32_t periods[LED_PATTERN_COUNT] = {
        0, 2000, 1200, 1000, 300, 1500, 2800, 1080, 1800,
        2200, 16000, 200, 1600, 1200, 1050, 2500, 1320, 1200,
    };

    for (uint8_t pattern = 0; pattern < LED_PATTERN_COUNT; pattern++) {
        const uint32_t period = periods[pattern];
        for (int64_t t = 0; t < (period ? 2 * (int64_t)period : 100); t++) {
            const struct pattern_sample sample = pattern_sample_at(pattern, t);
            assert(sample.brightness <= 100);
            if (period == 0) {
                assert(sample.brightness == 100 && sample.hold_ms == 0);
                continue;
            }
            /* Every animated sample says when it next changes, and repeats. */
            assert(sample.hold_ms >= 1 && sample.hold_ms <= period);
            assert(pattern_sample_at(pattern, t + period).brightness == sample.brightness);
            if (on_ramp(pattern, t)) {
                assert(sample.hold_ms <= RAMP_STEP_MS);
            } else {
                /* A step holds its level for the whole of what it promised. */
                assert(pattern_sample_at(pattern, t + sample.hold_ms - 1).brightness ==
                       sample.brightness);
            }
        }
    }

    /* Every animated shape changes within its period; a curve read against
     * the wrong phase would sit at one level. */
    for (uint8_t pattern = 1; pattern < LED_PATTERN_COUNT; pattern++) {
        const uint8_t first = pattern_sample_at(pattern, 0).brightness;
        bool moves = false;
        for (int64_t t = 1; t < periods[pattern] && !moves; t++) {
            moves = pattern_sample_at(pattern, periods[pattern] * 3 + t).brightness != first;
        }
        assert(moves);
    }
    /* Mid-ramp levels, by the formulas. */
    assert(pattern_sample_at(LED_PATTERN_SAWTOOTH, 900).brightness == 52);
    assert(pattern_sample_at(LED_PATTERN_SLOW_BREATHE, 1500).brightness == 50);
    assert(pattern_sample_at(LED_PATTERN_SLOW_BREATHE, 4000).brightness == 67);
    assert(pattern_sample_at(LED_PATTERN_FADE_BLINK, 75).brightness == 70);
    assert(pattern_sample_at(LED_PATTERN_FADE_BLINK, 225).brightness == 23);
    /* A ramp step is cut short by the end of its segment. */
    assert(pattern_sample_at(LED_PATTERN_FADE_BLINK, 145).hold_ms == 5);

    /* A few fixed points of the shapes themselves. */
    assert(pattern_sample_at(LED_PATTERN_BREATHE, 0).brightness == 10);
    assert(pattern_sample_at(LED_PATTERN_BREATHE, 1000).brightness == 100);
    struct pattern_sample dark = pattern_sample_at(LED_PATTERN_SLOW_BREATHE, 7000);
    assert(dark.brightness == 0 && dark.hold_ms == 9000);
    struct pattern_sample on = pattern_sample_at(LED_PATTERN_BLINK, 0);
    assert(on.brightness == 100 && on.hold_ms == 500);
    /* An unknown id draws Steady. */
    struct pattern_sample unknown = pattern_sample_at(LED_PATTERN_COUNT, 5);
    assert(unknown.brightness == 100 && unknown.hold_ms == 0);
}

static void test_clock(void) {
    speed_percent = LED_PATTERN_SPEED_MAX;
    assert(pattern_clock(100) == 400);
    assert(hold_to_wall_ms(1) == 1); /* 0.25 ms rounds to the 1 ms floor */
    assert(hold_to_wall_ms(400) == 100);
    speed_percent = LED_PATTERN_SPEED_MIN;
    assert(hold_to_wall_ms(UINT32_MAX) == UINT32_MAX); /* never wraps */
    speed_percent = 0; /* never set, but never a division by zero either */
    assert(hold_to_wall_ms(10) == 100);
    speed_percent = LED_PATTERN_SPEED_NOMINAL;

    /* A start stamp in the future reads as zero elapsed. */
    now_ms = 1000;
    assert(elapsed_since(2000) == 0 && elapsed_since(400) == 600);
}

static void test_before_ready(void) {
    /* Nothing is drawn, scheduled or announced before init. */
    led_pattern_set_brightness(90);
    assert(!pattern_work.scheduled && state_events == 0 && brightness_percent == 90);
    assert(ble_profile_changed_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    assert(led_pattern_endpoint_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    assert(!pattern_work.scheduled);
    led_pattern_set_brightness(100);
}

static void test_init_and_indicator(void) {
    /* Init takes the activity state as it finds it. */
    idle_off = false;
    reported_activity = ZMK_ACTIVITY_IDLE;
    assert(behavior_led_pattern_init(NULL) == 0);
    assert(!animation_suspended);
    idle_off = true;
    assert(behavior_led_pattern_init(NULL) == 0);
    assert(animation_suspended);
    reported_activity = ZMK_ACTIVITY_ACTIVE;
    assert(behavior_led_pattern_init(NULL) == 0);
    assert(controller_ready && !animation_suspended && pattern_work.scheduled);

    /* No host: the advertising blink stands in front of the pattern, at its
     * own fixed rate. */
    assert(advertising_blink && strstr(masking_indicator(), "advertising") != NULL);
    assert(run_pattern() && led_level == ADVERTISING_BLINK_BRIGHTNESS);
    assert(pattern_work.scheduled && pattern_work.delay_ms == ADVERTISING_BLINK_ON_MS);

    /* Each condition that turns it off. */
    profile_connected = true;
    assert(!should_show_advertising_blink());
    profile_connected = false;
    selected_transport = ZMK_TRANSPORT_BLE;
    assert(!should_show_advertising_blink());
    selected_transport = ZMK_TRANSPORT_NONE;
    advertising_indicator = false;
    assert(!should_show_advertising_blink());
    advertising_indicator = true;

    /* A host arrives on USB: the endpoint change drops the blink. */
    selected_transport = ZMK_TRANSPORT_USB;
    const zmk_event_t endpoint = {EVENT_ENDPOINT};
    assert(led_pattern_endpoint_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    assert(advertising_blink);
    assert(led_pattern_endpoint_listener(&endpoint) == ZMK_EV_EVENT_BUBBLE);
    assert(!advertising_blink && strcmp(masking_indicator(), "") == 0);
    selected_transport = ZMK_TRANSPORT_NONE;
    assert(ble_profile_changed_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    assert(advertising_blink);
    selected_transport = ZMK_TRANSPORT_USB;
    assert(ble_profile_changed_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    assert(!advertising_blink);
}

static void test_drawing(void) {
    /* Breathe, scaled by the brightness. */
    now_ms = pattern_started_at = 10000;
    led_pattern_set_brightness(50);
    assert(run_pattern() && led_level == 5 && pattern_work.delay_ms == RAMP_STEP_MS);
    assert(led_pattern_applied_brightness() == 5);

    /* A write the LED refuses is not recorded as applied. */
    led_result = -EIO;
    now_ms += 500;
    run_pattern();
    assert(led_pattern_applied_brightness() == 5);
    led_result = 0;

    /* An LED device not ready yet is left alone. */
    backlight_device.ready = false;
    const int writes = led_writes;
    run_pattern();
    assert(led_writes == writes && !led_pattern_ready());
    backlight_device.ready = true;

    /* Steady draws once and schedules nothing. */
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_NOMINAL, true, true);
    assert(run_pattern() && led_level == 50 && !pattern_work.scheduled);

    /* Brightness zero is one dark write and no schedule, whatever the shape. */
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_NOMINAL, true, true);
    led_pattern_set_brightness(0);
    assert(run_pattern() && led_level == 0 && !pattern_work.scheduled);
    led_pattern_set_brightness(100);
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 500);

    /* A hold of one millisecond is still a hold. */
    set_state(LED_PATTERN_SAWTOOTH, LED_PATTERN_SPEED_NOMINAL, true, true);
    pattern_started_at = now_ms - 1799;
    assert(run_pattern() && pattern_work.scheduled && pattern_work.delay_ms == 1);

    /* Speed stretches the hold on the wall clock. */
    set_state(LED_PATTERN_BLINK, 200, true, true);
    assert(run_pattern() && pattern_work.delay_ms == 250);
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_NOMINAL, true, true);
}

static void test_activity(void) {
    /* Idle darkens the LED; activity restarts the curve from its beginning. */
    activity(ZMK_ACTIVITY_IDLE);
    assert(animation_suspended && run_pattern() && led_level == 0 && !pattern_work.scheduled);
    activity(ZMK_ACTIVITY_SLEEP); /* already dark: nothing to change */
    assert(!pattern_work.scheduled);
    assert(led_pattern_activity_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    now_ms += 777;
    activity(ZMK_ACTIVITY_ACTIVE);
    assert(!animation_suspended && pattern_started_at == now_ms && pattern_work.scheduled);

    /* Clearing idle-off while idle lights it at once. */
    activity(ZMK_ACTIVITY_IDLE);
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_NOMINAL, true, false);
    assert(!animation_suspended);
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_NOMINAL, true, true);
    assert(animation_suspended);
    activity(ZMK_ACTIVITY_ACTIVE);
}

static void test_state_api(void) {
    struct led_pattern_state state;

    /* Out-of-range values are brought into range. */
    state_events = 0;
    set_state(99, 5, true, true);
    led_pattern_get_state(&state);
    assert(state.pattern == LED_PATTERN_STEADY && state.speed == LED_PATTERN_SPEED_MIN);
    set_state(LED_PATTERN_STEADY, 1000, true, true);
    led_pattern_get_state(&state);
    assert(state.speed == LED_PATTERN_SPEED_MAX && state_events == 2);
    /* Nothing shown changed: no event. */
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_MAX, true, true);
    assert(state_events == 2);
    set_state(LED_PATTERN_COUNT, LED_PATTERN_SPEED_MAX, true, true);
    led_pattern_get_state(&state);
    assert(state.pattern == LED_PATTERN_STEADY && state_events == 2);

    set_state(LED_PATTERN_TRIPLE_FLASH, 150, false, false);
    led_pattern_get_state(&state);
    assert(state.pattern == LED_PATTERN_TRIPLE_FLASH && state.speed == 150);
    assert(!state.advertising_indicator && !state.idle_off);
    pattern_work.scheduled = false;
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_MAX, true, true);
    led_pattern_get_state(&state);
    assert(state.pattern == LED_PATTERN_BLINK && state.advertising_indicator && state.idle_off);
    assert(pattern_work.scheduled && pattern_work.delay_ms == 0);
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_MAX, true, true);

    /* Asking for a pattern clears the advertising blink; so does switching
     * the indicator off. */
    advertising_blink = true;
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_MAX, true, true);
    assert(advertising_blink);
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_MAX, true, true);
    assert(!advertising_blink);
    advertising_blink = true;
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_MAX, false, true);
    assert(!advertising_blink);

    /* Brightness: clamped, and an unchanged value is a no-op. */
    led_pattern_set_brightness(150);
    assert(led_pattern_get_brightness() == 100);
    const int events = state_events;
    led_pattern_set_brightness(100);
    assert(state_events == events);
    led_pattern_set_brightness(40);
    assert(state_events == events + 1 && last_state_event.brightness == 40);

    assert(led_pattern_field_value(LED_PATTERN_FIELD_PATTERN) == LED_PATTERN_BLINK);
    assert(led_pattern_field_value(LED_PATTERN_FIELD_BRIGHTNESS) == 40);
    assert(led_pattern_field_value(LED_PATTERN_FIELD_SPEED) == LED_PATTERN_SPEED_MAX);
    assert(led_pattern_field_value((enum led_pattern_field)99) == 0);
}

static void test_requests(void) {
    /* With settings, a stored request reaches the LED through the settings. */
    settings_store_result = 0;
    led_pattern_request(LED_PATTERN_FIELD_PATTERN, 4);
    assert(settings_stores == 1 && active_pattern == LED_PATTERN_BLINK);

    /* A refused store falls back to the LED directly, clamped. */
    settings_store_result = -EINVAL;
    led_pattern_request(LED_PATTERN_FIELD_PATTERN, 99);
    assert(active_pattern == LED_PATTERN_FADE_BLINK);
    led_pattern_request(LED_PATTERN_FIELD_PATTERN, -1);
    assert(active_pattern == LED_PATTERN_STEADY);
    led_pattern_request(LED_PATTERN_FIELD_BRIGHTNESS, 150);
    assert(brightness_percent == 100);
    led_pattern_request(LED_PATTERN_FIELD_BRIGHTNESS, -5);
    assert(brightness_percent == 0);
    led_pattern_request(LED_PATTERN_FIELD_SPEED, 1);
    assert(speed_percent == LED_PATTERN_SPEED_MIN);
    led_pattern_request(LED_PATTERN_FIELD_SPEED, 5000);
    assert(speed_percent == LED_PATTERN_SPEED_MAX);
    led_pattern_request(LED_PATTERN_FIELD_PATTERN, LED_PATTERN_TRIPLE_FLASH);
    led_pattern_request(LED_PATTERN_FIELD_SPEED, 150);
    assert(speed_percent == 150 && active_pattern == LED_PATTERN_TRIPLE_FLASH);
    led_pattern_request(LED_PATTERN_FIELD_BRIGHTNESS, 55);
    assert(brightness_percent == 55);

    /* &led_pattern: a number, a step either way (wrapping), or a warning. */
    assert(press(3) == ZMK_BEHAVIOR_OPAQUE && active_pattern == 3);
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_NOMINAL, true, true);
    assert(press(LED_PATTERN_PREVIOUS) == ZMK_BEHAVIOR_OPAQUE);
    assert(active_pattern == LED_PATTERN_FADE_BLINK);
    assert(press(LED_PATTERN_NEXT) == ZMK_BEHAVIOR_OPAQUE);
    assert(active_pattern == LED_PATTERN_STEADY);
    set_state(LED_PATTERN_TRIPLE_FLASH, LED_PATTERN_SPEED_NOMINAL, true, true);
    assert(press(LED_PATTERN_PREVIOUS) == ZMK_BEHAVIOR_OPAQUE && active_pattern == 4);
    assert(press(LED_PATTERN_NEXT) == ZMK_BEHAVIOR_OPAQUE && active_pattern == 5);
    assert(press(LED_PATTERN_NEXT) == ZMK_BEHAVIOR_OPAQUE && active_pattern == 6);
    set_state(LED_PATTERN_STEADY, LED_PATTERN_SPEED_NOMINAL, true, true);
    const int before = warnings;
    assert(press(999) == ZMK_BEHAVIOR_OPAQUE && warnings == before + 1);
    assert(press(LED_PATTERN_COUNT) == ZMK_BEHAVIOR_OPAQUE && warnings == before + 2);
    assert(active_pattern == LED_PATTERN_STEADY);
    backlight_device.ready = false;
    assert(press(3) == -ENODEV);
    backlight_device.ready = true;
    struct zmk_behavior_binding binding = {0};
    assert(on_pattern_released(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    led_pattern_set_brightness(100);
}

static void test_power_off(void) {
    set_state(LED_PATTERN_BLINK, LED_PATTERN_SPEED_NOMINAL, true, true);
    assert(pattern_work.scheduled);

    /* Suspend writes the LED dark, stops the work and holds everything. */
    led_level = 77;
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(led_level == 0 && !pattern_work.scheduled && powering_off);
    led_pattern_set_brightness(60);
    assert(!pattern_work.scheduled);
    pattern_work.work.handler(&pattern_work.work);
    assert(led_level == 0);
    /* The second power-off device finds it already done. */
    const int writes = led_writes;
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(led_writes == writes);

    /* An abandoned power-off redraws; a stray resume does nothing. */
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0);
    assert(!powering_off && pattern_work.scheduled);
    pattern_work.scheduled = false;
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0);
    assert(!pattern_work.scheduled);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_TURN_OFF) == -ENOTSUP);
    assert(led_pattern_pm_init(NULL) == 0);
}

int main(void) {
    test_curves();
    test_clock();
    test_before_ready();
    test_init_and_indicator();
    test_drawing();
    test_activity();
    test_state_api();
    test_requests();
    test_power_off();
    puts("led pattern (standalone): PASS");
    return 0;
}
