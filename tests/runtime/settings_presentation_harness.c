/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 * Actual bridge and adapter definitions, with controlled registry/kernel calls.
 * Controller forwarding is a boundary stand-in; its actual wrappers are tested
 * by the controller role fixtures. No radio, kernel scheduling or PWM claim.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zmk-led-patterns/led_pattern.h>
#include "led_sync_protocol.h"
#define IS_ENABLED(x) (x)
#define CONFIG_ZMK_USB 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CLAMP(v,lo,hi) MIN(MAX(v,lo),hi)
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define K_FOREVER 0
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LED_PATTERNS_SUBSYSTEM "amgskobo__led"
struct k_mutex { unsigned depth; };
static struct k_mutex sync_mutex, settings_apply_mutex;
static int k_mutex_lock(struct k_mutex *m, int timeout) {
    (void)timeout; assert(m->depth == 0); m->depth++; return 0;
}
static int k_mutex_unlock(struct k_mutex *m) {
    assert(m->depth == 1); m->depth--; return 0;
}
static uint64_t now_ms = 10000;
static uint64_t k_uptime_get(void) { return now_ms; }
static struct led_sync_engine engine;
static struct led_sync_values desired;
static bool started = true;
static void schedule_locked(uint64_t now) {
    assert(sync_mutex.depth == 1); assert(now == now_ms);
}
/* DRIVER_ADAPTER_FUNCTIONS */

/* Only forwarding: pending/clock/visible state is the production engine above. */
void led_pattern_get_state(struct led_pattern_state *out) {
    struct led_sync_values values; led_scheduled_get_desired(&values);
    *out = (struct led_pattern_state){values.pattern, values.speed,
        (values.flags & LED_SYNC_INDICATOR) != 0, (values.flags & LED_SYNC_IDLE_OFF) != 0};
}
uint8_t led_pattern_get_brightness(void) {
    struct led_sync_values values; led_scheduled_get_desired(&values); return values.brightness;
}
void led_pattern_set_state(const struct led_pattern_state *state) { (void)led_scheduled_set_state(state); }
void led_pattern_set_brightness(uint8_t value) { (void)led_scheduled_set_brightness(value); }
void led_pattern_set_presentation(const struct led_pattern_state *state, uint8_t value) {
    (void)led_scheduled_set_presentation(state, value);
}

enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance { enum zmk_transport transport; };
static enum zmk_transport selected;
static struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){selected};
}
enum zmk_custom_setting_value_type { ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL };
struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    union { int32_t int32_value; bool bool_value; };
};
struct zmk_custom_setting { const char *key; const char *custom_subsystem_id; struct zmk_custom_setting_value value; };
struct zmk_custom_setting_changed { const struct zmk_custom_setting *setting; };
typedef struct { const struct zmk_custom_setting_changed *changed; } zmk_event_t;
static const struct zmk_custom_setting_changed *as_zmk_custom_setting_changed(const zmk_event_t *event) { return event->changed; }
static int zmk_custom_setting_read(const struct zmk_custom_setting *setting, struct zmk_custom_setting_value *value) {
    *value = setting->value; return 0;
}
struct transport_settings {
    const char *name;
    const struct zmk_custom_setting *pattern, *speed, *brightness, *idle_off;
};
#define INT_SETTING(name) static struct zmk_custom_setting name = {#name, ZMK_LED_PATTERNS_SUBSYSTEM, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32}}
#define BOOL_SETTING(name) static struct zmk_custom_setting name = {#name, ZMK_LED_PATTERNS_SUBSYSTEM, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL}}
INT_SETTING(usb_pattern); INT_SETTING(usb_speed); INT_SETTING(usb_brightness); BOOL_SETTING(usb_idle_off);
INT_SETTING(ble_pattern); INT_SETTING(ble_speed); INT_SETTING(ble_brightness); BOOL_SETTING(ble_idle_off);
BOOL_SETTING(led_pattern_cs_adv_blink);
static const struct transport_settings usb_transport = {"usb", &usb_pattern, &usb_speed, &usb_brightness, &usb_idle_off};
static const struct transport_settings ble_transport = {"ble", &ble_pattern, &ble_speed, &ble_brightness, &ble_idle_off};
/* DRIVER_FUNCTIONS */

static void seed(struct led_sync_values old) {
    now_ms = 10000; desired = old;
    led_sync_init(&engine, true, 123, 20, old);
    led_sync_link(&engine, true, now_ms, 7);
    assert(led_sync_clock_sample(&engine.fit, 9960, 9970, 9980, 9990, 20));
    engine.clock_due = engine.clock_timeout = LED_SYNC_NEVER;
    engine.clock_ready = true; engine.clock_sequence = engine.sequence = 4;
    engine.have_active = engine.active_rendered = true;
    engine.active = (struct led_sync_plan){.valid = true, .confirmed = true, .sequence = 3, .clock = engine.fit};
    engine.active_clock = engine.fit; engine.visible = old;
    engine.phase_epoch = 9000; engine.dirty = engine.reset_phase = false;
    engine.transaction_active = false; engine.stage = LED_SYNC_SYNCED;
}
static void configure(struct led_sync_values next) {
    usb_pattern.value.int32_value = next.pattern; usb_speed.value.int32_value = next.speed;
    usb_brightness.value.int32_value = next.brightness;
    usb_idle_off.value.bool_value = (next.flags & LED_SYNC_IDLE_OFF) != 0;
    led_pattern_cs_adv_blink.value.bool_value = (next.flags & LED_SYNC_INDICATOR) != 0;
    selected = ZMK_TRANSPORT_USB;
}
static void apply(void) {
    zmk_event_t endpoint_event = {0};
    assert(led_pattern_settings_event_cb(&endpoint_event) == ZMK_EV_EVENT_BUBBLE);
    assert(sync_mutex.depth == 0 && settings_apply_mutex.depth == 0);
}
static void expect_pending(struct led_sync_values expected) {
    assert(engine.pending.valid);
    struct led_sync_values reserved = led_sync_wire_values(&engine.pending.wire);
    printf("reservation now=%llu seq=%u pattern=%u brightness=%u speed=%u flags=%u\n",
        (unsigned long long)now_ms, engine.pending.sequence, reserved.pattern,
        reserved.brightness, reserved.speed, reserved.flags); fflush(stdout);
    assert(led_sync_values_equal(&reserved, &expected) && "first reservation must contain the complete selected transport snapshot");
    assert(led_sync_values_equal(&desired, &expected));
    assert(engine.plan_attempts == 1 && engine.pending.sequence == 5);
}
int main(void) {
    const struct led_sync_values old = {1, 80, 100, LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR};
    const struct led_sync_values next = {7, 20, 130, LED_SYNC_ACTIVE};
    seed(old); configure(next); apply(); expect_pending(next);
    assert(led_sync_values_equal(&engine.visible, &old));
    now_ms = engine.pending.local_deadline - 1;
    assert(!led_sync_advance(&engine, now_ms));
    assert(led_sync_values_equal(&engine.visible, &old));
    now_ms++;
    assert(led_sync_advance(&engine, now_ms));
    assert(led_sync_values_equal(&engine.visible, &next));
    led_sync_rendered(&engine, true, now_ms);
    assert(!engine.pending.valid && engine.plan_attempts == 1);

    seed(old); configure(old); apply();
    assert(!engine.pending.valid && !engine.transaction_active && engine.sequence == 4);
    seed(old); configure(next);
    usb_pattern.value.int32_value = 99; usb_speed.value.int32_value = 999;
    usb_brightness.value.int32_value = -55; apply();
    struct led_sync_values clamped = {old.pattern, 0, 400, LED_SYNC_ACTIVE};
    expect_pending(clamped);

    seed(old);
    assert(led_scheduled_request(LED_PATTERN_FIELD_BRIGHTNESS, 35));
    struct led_pattern_state state = {7, 130, false, false};
    assert(led_scheduled_set_state(&state));
    assert(desired.brightness == 35 && desired.pattern == 7 && desired.speed == 130);
    assert(!led_scheduled_set_state(&state));
    assert(desired.brightness == 35);
    seed(old);
    struct led_pattern_state unchanged = {old.pattern, old.speed, true, true};
    assert(!led_scheduled_set_presentation(&unchanged, old.brightness));
    assert(!engine.pending.valid && !engine.transaction_active);
    struct led_pattern_state invalid = {UINT8_MAX, UINT16_MAX, false, false};
    assert(led_scheduled_set_presentation(&invalid, UINT8_MAX));
    struct led_sync_values bounded = {LED_PATTERN_STEADY, 100, 400, LED_SYNC_ACTIVE};
    expect_pending(bounded);
    puts("settings complete presentation admission: PASS");
    return 0;
}
