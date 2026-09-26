/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Stand-ins for Zephyr and ZMK shared by the behavior_led_pattern.c harnesses,
 * plus the file-scope state that the lifted functions read. A harness sets
 * the CONFIG_ options it builds as, then includes this before its
 * DRIVER_FUNCTIONS marker.
 */

#pragma once

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zmk-led-patterns/led_pattern.h>

#define IS_ENABLED(option) option
/* Not every role uses every stand-in. */
#define STUB __attribute__((unused))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ARG_UNUSED(x) ((void)(x))
/* Unevaluated printf() keeps the format strings checked and the arguments used. */
#define LOG_DBG(fmt, ...) ((void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define LOG_INF(fmt, ...) LOG_DBG(fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) ((void)warnings++, LOG_DBG(fmt, ##__VA_ARGS__))
#define LOG_ERR(fmt, ...) ((void)errors++, LOG_DBG(fmt, ##__VA_ARGS__))
#define K_MSEC(ms) ((k_timeout_t){(ms)})
#define K_NO_WAIT ((k_timeout_t){0})
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_RELAY_EVENT_SOURCE_SELF 0xFF
#define LED_INDEX 0
#define RAMP_STEP_MS 40
#define ADVERTISING_BLINK_PERIOD_MS 1500
#define ADVERTISING_BLINK_ON_MS 300
#define ADVERTISING_BLINK_BRIGHTNESS 70
#define ADVERTISING_BLINK_OFF_BRIGHTNESS 0
#define MIRROR_DEBOUNCE_MS 20
#define SPLIT_SYNC_START_DELAY_MS 5000
#define SPLIT_SYNC_ACK_TIMEOUT_MS 500
#define SPLIT_SYNC_MAX_ATTEMPTS 5

typedef long atomic_t;
typedef long atomic_val_t;
typedef struct { int64_t ms; } k_timeout_t;
struct k_work { void (*handler)(struct k_work *work); bool pending; };
struct k_work_delayable { struct k_work work; bool scheduled; int64_t delay_ms; };
struct k_work_q { int unused; };
struct k_work_sync { int unused; };
struct device { const char *name; bool ready; };
struct zmk_behavior_binding { uint32_t param1; };
struct zmk_behavior_binding_event { int unused; };
enum pm_device_action { PM_DEVICE_ACTION_SUSPEND, PM_DEVICE_ACTION_RESUME,
                        PM_DEVICE_ACTION_TURN_OFF };
enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
enum event_kind { EVENT_ACTIVITY = 1, EVENT_ENDPOINT, EVENT_MIRROR, EVENT_ACK,
                  EVENT_PERIPHERAL_STATUS, EVENT_OTHER };
typedef struct { enum event_kind kind; } zmk_event_t;
struct zmk_activity_state_changed { zmk_event_t header; enum zmk_activity_state state; };
struct zmk_led_pattern_state_changed { uint8_t pattern; uint8_t brightness; uint16_t speed; };

static STUB int warnings;
static STUB int errors;
static STUB int64_t now_ms;
static STUB struct k_work_q lowprio_queue;
static STUB struct device backlight_device = {.name = "backlight", .ready = true};
static STUB const struct device *const backlight = &backlight_device;
static STUB int led_result;
static STUB int led_writes;
static STUB uint8_t led_level = 255;
static STUB enum zmk_activity_state reported_activity = ZMK_ACTIVITY_ACTIVE;
static STUB int state_events;
static STUB struct zmk_led_pattern_state_changed last_state_event;

static STUB int64_t k_uptime_get(void) { return now_ms; }
static STUB atomic_val_t atomic_get(const atomic_t *value) { return *value; }
static STUB atomic_val_t atomic_set(atomic_t *value, atomic_val_t next) {
    const atomic_val_t old = *value;
    *value = next;
    return old;
}
static STUB atomic_val_t atomic_clear(atomic_t *value) { return atomic_set(value, 0); }
static STUB struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &lowprio_queue; }
static STUB int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                       k_timeout_t delay) {
    assert(queue == &lowprio_queue);
    work->scheduled = true;
    work->delay_ms = delay.ms;
    return 1;
}
static STUB int k_work_cancel_delayable(struct k_work_delayable *work) {
    work->scheduled = false;
    return 0;
}
static STUB bool k_work_cancel_delayable_sync(struct k_work_delayable *work,
                                         struct k_work_sync *sync) {
    ARG_UNUSED(sync);
    work->scheduled = false;
    return false;
}
static STUB int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    assert(queue == &lowprio_queue);
    work->pending = true;
    return 1;
}
static STUB bool device_is_ready(const struct device *dev) { return dev->ready; }
static STUB int led_set_brightness(const struct device *dev, uint32_t led, uint8_t value) {
    assert(dev == backlight && led == LED_INDEX && value <= 100);
    led_writes++;
    if (led_result == 0) {
        led_level = value;
    }
    return led_result;
}
static STUB enum zmk_activity_state zmk_activity_get_state(void) { return reported_activity; }
static STUB const struct zmk_activity_state_changed *
as_zmk_activity_state_changed(const zmk_event_t *eh) {
    return eh->kind == EVENT_ACTIVITY ? (const void *)eh : NULL;
}
static STUB int raise_zmk_led_pattern_state_changed(struct zmk_led_pattern_state_changed event) {
    last_state_event = event;
    state_events++;
    return 0;
}

/* The file-scope state of behavior_led_pattern.c, with its initial values. */
static STUB void pattern_work_handler(struct k_work *work);
static STUB struct k_work_delayable pattern_work = {.work = {.handler = pattern_work_handler}};
static STUB uint8_t active_pattern = LED_PATTERN_BREATHE;
static STUB uint16_t speed_percent = LED_PATTERN_SPEED_NOMINAL;
static STUB uint8_t brightness_percent = 100;
static STUB bool advertising_indicator = true;
static STUB bool idle_off = true;
static STUB int64_t pattern_started_at;
static STUB bool controller_ready;
static STUB bool advertising_blink;
static STUB bool animation_suspended;
static STUB enum zmk_activity_state activity_state = ZMK_ACTIVITY_ACTIVE;
static STUB atomic_t powering_off;
static STUB atomic_t applied_brightness;

static STUB void refresh_pattern_output(void);
static STUB void schedule_mirror(k_timeout_t delay);
static STUB void apply_activity(void);
/* Gated in some roles and so lifted after the functions that call them. */
static STUB const char *masking_indicator(void);

struct pattern_sample { uint8_t brightness; uint32_t hold_ms; };
struct pattern_segment { uint32_t until_ms; uint8_t brightness; };
#define SEGMENT_SAMPLE(segments, elapsed) segment_sample(segments, ARRAY_SIZE(segments), elapsed)
#define TABLE_SAMPLE(levels, step_ms, elapsed)                                                     \
    table_sample(levels, ARRAY_SIZE(levels), step_ms, elapsed)

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
enum split_stage { SPLIT_STAGE_WAITING = 0, SPLIT_STAGE_LINKED, SPLIT_STAGE_SYNCED };
static STUB enum split_stage split_stage = SPLIT_STAGE_WAITING;
static STUB int64_t split_stage_started_at;
static STUB bool split_status_showing(void);
static STUB void set_split_stage(enum split_stage stage);
struct zmk_led_pattern_mirror {
    uint8_t source;
    uint8_t pattern;
    uint8_t brightness;
    uint16_t speed;
    uint32_t elapsed_ms;
    bool advertising_blink;
    bool idle_off;
    bool central_active;
};
struct zmk_led_pattern_ack { uint8_t source; uint8_t pattern; uint32_t elapsed_ms; };
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static STUB bool should_show_advertising_blink(void);
static STUB void refresh_connection_state(void);
enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance { enum zmk_transport transport; };
static STUB enum zmk_transport selected_transport = ZMK_TRANSPORT_NONE;
static STUB bool profile_connected;
static STUB struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){.transport = selected_transport};
}
static STUB bool zmk_ble_active_profile_is_connected(void) { return profile_connected; }
static STUB const void *as_zmk_endpoint_changed(const zmk_event_t *eh) {
    return eh->kind == EVENT_ENDPOINT ? eh : NULL;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS)
static STUB int settings_store_result;
static STUB int settings_stores;
int led_pattern_settings_store(enum led_pattern_field field, int32_t value) {
    ARG_UNUSED(field);
    ARG_UNUSED(value);
    settings_stores++;
    return settings_store_result;
}
#endif

/* Runs the pattern work if it is due; returns whether it was scheduled. */
static STUB bool run_pattern(void) {
    const bool was = pattern_work.scheduled;
    pattern_work.scheduled = false;
    pattern_work.work.handler(&pattern_work.work);
    return was;
}

static STUB const zmk_event_t other_event = {EVENT_OTHER};
