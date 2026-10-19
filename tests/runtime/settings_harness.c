/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern_custom_settings.c built with USB and Studio over the
 * USB serial port: per-transport sets, the USB write coalescing, the deferred
 * flash save and the notification suppression while Studio is not listening.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk-led-patterns/led_pattern.h>
#include "usb_pending.h"

#define IS_ENABLED(option) option
#define CONFIG_ZMK_USB 1
#ifndef LED_SETTINGS_LEGACY_API
#define ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES 1
#endif
#define ZMK_CUSTOM_SETTING_SOURCE_LOCAL 0U
#define CONFIG_ZMK_STUDIO_TRANSPORT_UART 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ARG_UNUSED(x) ((void)(x))
#define LOG_DBG(fmt, ...) ((void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define LOG_INF(fmt, ...) LOG_DBG(fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) ((void)warnings++, LOG_DBG(fmt, ##__VA_ARGS__))
#define K_FOREVER ((k_timeout_t){-1})
#define K_MSEC(ms) ((k_timeout_t){(ms)})
#define ZMK_EV_EVENT_BUBBLE 0
#define DT_CHOSEN(node) node
#define DEVICE_DT_GET(node) (&studio_uart)
#define UART_LINE_CTRL_DTR 4
#define LED_PATTERN_USB_COALESCE_MS 300
#define LED_PATTERN_PERSIST_DELAY_MS 3000
#define ZMK_CUSTOM_SETTING_VALUE_INT32(v)                                                          \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                \
                                       .int32_value = (v)})

typedef long atomic_t;
typedef struct { int64_t ms; } k_timeout_t;
typedef struct { int unused; const struct zmk_custom_setting *setting; } zmk_event_t;
typedef struct { int unused; } zmk_custom_CallRequest;
typedef struct { int unused; } pb_callback_t;
struct k_mutex { int depth; };
static struct k_mutex settings_apply_mutex;
struct k_work { void (*handler)(struct k_work *work); };
struct k_work_delayable { struct k_work work; bool scheduled; int64_t delay_ms; };
struct k_work_q { int unused; };
struct device { bool ready; };
enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance { enum zmk_transport transport; };
enum zmk_custom_setting_value_type {
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 = 1,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
};
enum zmk_custom_setting_write_mode {
    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
};
struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    union {
        int32_t int32_value;
        bool bool_value;
    };
};
/* The descriptor, plus what this harness makes of it. */
struct zmk_custom_setting {
    const char *key;
    struct zmk_custom_setting_value value;
    int read_result;
    int write_result;
    int memory_writes;
    int persists;
    const char *custom_subsystem_id;
    int save_result;
    int32_t persistent_value;
};
#define ZMK_LED_PATTERNS_SUBSYSTEM "amgskobo__led"
enum zmk_custom_setting_changed_kind {
    ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
    ZMK_CUSTOM_SETTING_CHANGED_SAVED,
};
struct zmk_custom_setting_changed {
    const struct zmk_custom_setting *setting;
    enum zmk_custom_setting_changed_kind kind;
    uint32_t source;
    bool suppress_studio_notification;
};
static struct zmk_custom_setting_changed changed_event;
static const struct zmk_custom_setting_changed *as_zmk_custom_setting_changed(const zmk_event_t *eh) {
    if (eh->unused != 1) return NULL;
    changed_event.setting = eh->setting;
    return &changed_event;
}
struct transport_settings {
    const char *name;
    const struct zmk_custom_setting *pattern;
    const struct zmk_custom_setting *speed;
    const struct zmk_custom_setting *brightness;
    const struct zmk_custom_setting *idle_off;
};

static int warnings;
static struct k_work_q lowprio_queue;
static enum zmk_transport selected = ZMK_TRANSPORT_BLE;
static struct device studio_uart = {.ready = true};
static int dtr_result;
static uint32_t dtr_level = 1;
static int suppress_depth;
static int suppressed_writes;
static struct led_pattern_state led_state = {LED_PATTERN_BREATHE, LED_PATTERN_SPEED_NOMINAL,
                                             true, true};
static uint8_t led_brightness = 100;
static int set_states;
static int set_brightnesses;

#define SETTING_INT(name, key, v)                                                                  \
    static struct zmk_custom_setting name = {                                                      \
        key, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (v)}, 0, 0, 0, 0, ZMK_LED_PATTERNS_SUBSYSTEM, 0, 0}
#define SETTING_BOOL(name, key, v)                                                                 \
    static struct zmk_custom_setting name = {                                                      \
        key, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = (v)}, 0, 0, 0, 0, ZMK_LED_PATTERNS_SUBSYSTEM, 0, 0}
SETTING_INT(led_pattern_cs_usb_pattern, "led.usb_pattern", LED_PATTERN_BREATHE);
SETTING_INT(led_pattern_cs_usb_speed, "led.usb_speed", LED_PATTERN_SPEED_NOMINAL);
SETTING_INT(led_pattern_cs_usb_brightness, "led.usb_brightness", 100);
SETTING_BOOL(led_pattern_cs_usb_idle_off, "led.usb_idle_off", true);
SETTING_INT(led_pattern_cs_ble_pattern, "led.ble_pattern", LED_PATTERN_BREATHE);
SETTING_INT(led_pattern_cs_ble_speed, "led.ble_speed", LED_PATTERN_SPEED_NOMINAL);
SETTING_INT(led_pattern_cs_ble_brightness, "led.ble_brightness", 100);
SETTING_BOOL(led_pattern_cs_ble_idle_off, "led.ble_idle_off", true);
SETTING_BOOL(led_pattern_cs_adv_blink, "led.adv_blink", true);

static const struct transport_settings usb_transport = {
    "usb", &led_pattern_cs_usb_pattern, &led_pattern_cs_usb_speed,
    &led_pattern_cs_usb_brightness, &led_pattern_cs_usb_idle_off};
static const struct transport_settings ble_transport = {
    "ble", &led_pattern_cs_ble_pattern, &led_pattern_cs_ble_speed,
    &led_pattern_cs_ble_brightness, &led_pattern_cs_ble_idle_off};
enum { TRANSPORT_USB, TRANSPORT_BLE };
static const struct transport_settings *const transports[] = {
    [TRANSPORT_USB] = &usb_transport,
    [TRANSPORT_BLE] = &ble_transport,
};
static atomic_t unsaved[ARRAY_SIZE(transports)];
static struct k_mutex usb_pending_mutex;
static struct k_mutex persist_mutex;
static struct led_pattern_usb_pending usb_pending;
static void persist_work_handler(struct k_work *work);
static struct k_work_delayable persist_work = {.work = {.handler = persist_work_handler}};
static void usb_coalesce_work_handler(struct k_work *work);
static struct k_work_delayable usb_coalesce_work = {.work = {.handler = usb_coalesce_work_handler}};

static int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout) {
    ARG_UNUSED(timeout);
    assert(mutex->depth == 0);
    mutex->depth++;
    return 0;
}
static int k_mutex_unlock(struct k_mutex *mutex) {
    assert(mutex->depth == 1);
    mutex->depth--;
    return 0;
}
static bool atomic_test_bit(const atomic_t *target, int bit) { return (*target >> bit) & 1; }
static void atomic_set_bit(atomic_t *target, int bit) { *target |= 1L << bit; }
static void atomic_clear_bit(atomic_t *target, int bit) { *target &= ~(1L << bit); }
static struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &lowprio_queue; }
static int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                       k_timeout_t delay) {
    assert(queue == &lowprio_queue);
    work->scheduled = true;
    work->delay_ms = delay.ms;
    return 1;
}
static struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){.transport = selected};
}
static bool device_is_ready(const struct device *dev) { return dev->ready; }
static int uart_line_ctrl_get(const struct device *dev, uint32_t ctrl, uint32_t *value) {
    assert(dev == &studio_uart && ctrl == UART_LINE_CTRL_DTR);
    *value = dtr_level;
    return dtr_result;
}
static int led_pattern_settings_event_cb(const zmk_event_t *eh);
static bool inject_client_on_save;
static int injected_client_writes;
static int core_events;
static int notification_count;
static int notification_result;
static void (*notification_hook)(void);
static void inject_newer_client_value(void);

static int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                                   struct zmk_custom_setting_value *value) {
    *value = setting->value;
    if (inject_client_on_save && setting == &led_pattern_cs_ble_brightness) {
        inject_newer_client_value();
    }
    return setting->read_result;
}
static int raise_zmk_custom_setting_changed(struct zmk_custom_setting_changed event) {
    core_events++;
    if (!event.suppress_studio_notification && suppress_depth == 0) {
        notification_count++;
        if (notification_hook != NULL) {
            void (*hook)(void) = notification_hook;
            notification_hook = NULL;
            hook();
        }
        if (notification_result < 0) return notification_result;
    }
    const zmk_event_t eh = {1, event.setting};
    return led_pattern_settings_event_cb(&eh);
}
static int setting_write(const struct zmk_custom_setting *setting,
                         const struct zmk_custom_setting_value *value,
                         enum zmk_custom_setting_write_mode mode, bool quiet) {
    struct zmk_custom_setting *mutable = (struct zmk_custom_setting *)setting;
    if (mutable->write_result != 0) return mutable->write_result;
    mutable->value = *value;
    if (mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) {
        mutable->persists++;
        mutable->persistent_value = value->int32_value;
    } else {
        mutable->memory_writes++;
    }
    suppressed_writes += quiet || suppress_depth > 0;
    return raise_zmk_custom_setting_changed((struct zmk_custom_setting_changed){
        .setting = setting,
        .kind = mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                    ? ZMK_CUSTOM_SETTING_CHANGED_SAVED : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
        .suppress_studio_notification = quiet,
    });
}
static int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode) {
    return setting_write(setting, value, mode, false);
}
#if defined(ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES)
static int zmk_custom_setting_write_quiet(const struct zmk_custom_setting *setting,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode) {
    return setting_write(setting, value, mode, true);
}
#endif
static int setting_save(const struct zmk_custom_setting *setting, bool quiet) {
    if (inject_client_on_save && setting == &led_pattern_cs_ble_brightness) {
        inject_newer_client_value();
    }
    struct zmk_custom_setting *mutable = (struct zmk_custom_setting *)setting;
    if (mutable->save_result != 0) return mutable->save_result;
    if (mutable->write_result != 0) return mutable->write_result;
    mutable->persists++;
    mutable->persistent_value = mutable->value.int32_value;
    suppressed_writes += quiet || suppress_depth > 0;
    return raise_zmk_custom_setting_changed((struct zmk_custom_setting_changed){
        .setting = setting, .kind = ZMK_CUSTOM_SETTING_CHANGED_SAVED,
        .suppress_studio_notification = quiet,
    });
}
static int zmk_custom_setting_save(const struct zmk_custom_setting *setting) {
    return setting_save(setting, false);
}
#if defined(ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES)
static int zmk_custom_setting_save_quiet(const struct zmk_custom_setting *setting) {
    return setting_save(setting, true);
}
#endif
#if !defined(ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES)
static void zmk_custom_settings_notify_suppress_begin(void) { assert(suppress_depth++ == 0); }
static void zmk_custom_settings_notify_suppress_end(void) { assert(--suppress_depth == 0); }
#endif
void led_pattern_get_state(struct led_pattern_state *out) { *out = led_state; }
void led_pattern_set_state(const struct led_pattern_state *state) {
    led_state = *state;
    set_states++;
}
uint8_t led_pattern_get_brightness(void) { return led_brightness; }
void led_pattern_set_brightness(uint8_t brightness) {
    led_brightness = brightness;
    set_brightnesses++;
}
void led_pattern_set_presentation(const struct led_pattern_state *state, uint8_t brightness) {
    if (led_state.pattern != state->pattern || led_state.speed != state->speed ||
        led_state.idle_off != state->idle_off ||
        led_state.advertising_indicator != state->advertising_indicator)
        set_states++;
    if (led_brightness != brightness)
        set_brightnesses++;
    led_state = *state;
    led_brightness = brightness;
}

/* DRIVER_FUNCTIONS */

static void apply(void) {
    static const zmk_event_t event;
    assert(led_pattern_settings_event_cb(&event) == ZMK_EV_EVENT_BUBBLE);
}

static void set_int(struct zmk_custom_setting *setting, int32_t value) {
    setting->value = ZMK_CUSTOM_SETTING_VALUE_INT32(value);
}

static void run(struct k_work_delayable *work) {
    work->scheduled = false;
    work->work.handler(&work->work);
}

static void inject_newer_client_value(void) {
    inject_client_on_save = false;
    injected_client_writes++;
    const struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(80);
    assert(zmk_custom_setting_write(&led_pattern_cs_ble_brightness, &value,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
}

static void test_newer_client_edit_wins(void) {
    selected = ZMK_TRANSPORT_USB;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 20) >= 0);
    led_pattern_set_brightness(20); /* The existing deferred path's caller. */
    const struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(80);
    assert(zmk_custom_setting_write(&led_pattern_cs_usb_brightness, &value,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
    assert(led_brightness == 80); /* A later client edit applies immediately. */
    run(&usb_coalesce_work);
    assert(led_pattern_cs_usb_brightness.value.int32_value == 80 && led_brightness == 80);
    run(&persist_work);
    assert(led_pattern_cs_usb_brightness.value.int32_value == 80 && led_brightness == 80);
    puts("newer client value survives USB notification and persistence: PASS");
}

static void test_newer_client_during_save(void) {
    selected = ZMK_TRANSPORT_BLE;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 20) == 0);
    inject_client_on_save = true;
    run(&persist_work);
    assert(injected_client_writes == 1 && !inject_client_on_save);
    assert(led_pattern_cs_ble_brightness.value.int32_value == 80 && led_brightness == 80);
    assert(led_pattern_cs_ble_brightness.persistent_value == 80);
    puts("newer client value survives delayed save: PASS");
}

static void test_apply(void) {
    /* The live BLE set, applied onto what the LED runs now. */
    set_int(&led_pattern_cs_ble_pattern, LED_PATTERN_SOS);
    set_int(&led_pattern_cs_ble_brightness, 40);
    apply();
    assert(led_state.pattern == LED_PATTERN_SOS && led_brightness == 40);
    assert(set_states == 1 && set_brightnesses == 1);

    /* Nothing changed: nothing is set. */
    apply();
    assert(set_states == 1 && set_brightnesses == 1);

    /* Each field that changes on its own is enough to set the state. */
    set_int(&led_pattern_cs_ble_speed, 250);
    apply();
    assert(led_state.speed == 250 && set_states == 2);
    led_pattern_cs_adv_blink.value.bool_value = false;
    apply();
    assert(!led_state.advertising_indicator && set_states == 3);
    led_pattern_cs_ble_idle_off.value.bool_value = false;
    apply();
    assert(!led_state.idle_off && set_states == 4);

    /* Out-of-range values: a pattern is ignored, speed and brightness are
     * clamped. */
    set_int(&led_pattern_cs_ble_pattern, -1);
    set_int(&led_pattern_cs_ble_speed, 1);
    set_int(&led_pattern_cs_ble_brightness, 500);
    apply();
    assert(led_state.pattern == LED_PATTERN_SOS && led_state.speed == LED_PATTERN_SPEED_MIN);
    assert(led_brightness == 100);
    set_int(&led_pattern_cs_ble_pattern, LED_PATTERN_COUNT);
    set_int(&led_pattern_cs_ble_speed, 9999);
    set_int(&led_pattern_cs_ble_brightness, -3);
    apply();
    assert(led_state.pattern == LED_PATTERN_SOS && led_state.speed == LED_PATTERN_SPEED_MAX);
    assert(led_brightness == 0);

    /* A value that cannot be read, or reads back as another type, leaves its
     * field alone and says so. */
    const int before = warnings;
    led_pattern_cs_ble_pattern.read_result = -EIO;
    led_pattern_cs_ble_speed.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;
    led_pattern_cs_ble_brightness.read_result = -EIO;
    led_pattern_cs_adv_blink.read_result = -EIO;
    led_pattern_cs_ble_idle_off.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
    const int states = set_states;
    apply();
    assert(set_states == states && warnings == before + 3);
    led_pattern_cs_ble_pattern.read_result = 0;
    led_pattern_cs_ble_speed.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
    led_pattern_cs_ble_brightness.read_result = 0;
    led_pattern_cs_adv_blink.read_result = 0;
    led_pattern_cs_ble_idle_off.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;

    /* A read error alone is enough, even on a value of the right type. */
    led_pattern_cs_adv_blink.read_result = -EIO;
    led_pattern_cs_adv_blink.value.bool_value = true;
    apply();
    assert(set_states == states && !led_state.advertising_indicator);
    led_pattern_cs_adv_blink.read_result = 0;
    /* ...and an unreadable value leaves a set field alone, too. */
    led_pattern_cs_adv_blink.value.bool_value = true;
    led_pattern_cs_ble_idle_off.value.bool_value = true;
    apply();
    assert(led_state.advertising_indicator && led_state.idle_off);
    const int lit = set_states;
    led_pattern_cs_adv_blink.read_result = -EIO;
    led_pattern_cs_ble_idle_off.read_result = -EIO;
    apply();
    assert(set_states == lit && led_state.advertising_indicator && led_state.idle_off);
    led_pattern_cs_adv_blink.read_result = 0;
    led_pattern_cs_ble_idle_off.read_result = 0;
    led_pattern_cs_adv_blink.value.bool_value = false;
    led_pattern_cs_ble_idle_off.value.bool_value = false;
    apply();

    /* Pattern zero is a pattern. */
    set_int(&led_pattern_cs_ble_pattern, 0);
    apply();
    assert(led_state.pattern == 0);

    /* On USB the registry's USB set is live. */
    selected = ZMK_TRANSPORT_USB;
    set_int(&led_pattern_cs_usb_pattern, LED_PATTERN_CANDLE);
    apply();
    assert(led_state.pattern == LED_PATTERN_CANDLE && led_state.speed == 100);
    assert(led_brightness == 100);
    selected = ZMK_TRANSPORT_BLE;
}

static void test_ble_store(void) {
    /* An unknown field has no setting. */
    assert(led_pattern_settings_store((enum led_pattern_field)7, 1) == -EINVAL);

    /* BLE writes memory now and flash later. */
    const int writes = led_pattern_cs_ble_brightness.memory_writes;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 70) == 0);
    assert(led_pattern_cs_ble_brightness.memory_writes == writes + 1);
    assert(atomic_test_bit(&unsaved[TRANSPORT_BLE], LED_PATTERN_FIELD_BRIGHTNESS));
    assert(persist_work.scheduled && persist_work.delay_ms == LED_PATTERN_PERSIST_DELAY_MS);

    /* A refused memory write is reported to the caller, which applies the
     * LED itself. */
    led_pattern_cs_ble_pattern.write_result = -EIO;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 3) == -EIO);
    led_pattern_cs_ble_pattern.write_result = 0;

    /* The save runs once the keys are quiet and clears the mark. */
    run(&persist_work);
    assert(led_pattern_cs_ble_brightness.persists == 1 && unsaved[TRANSPORT_BLE] == 0);
    assert(!persist_work.scheduled);

    /* A failed registry save or storage write keeps the mark and retries. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_SPEED, 120) == 0);
    led_pattern_cs_ble_speed.save_result = -EIO;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] != 0 && persist_work.scheduled);
    led_pattern_cs_ble_speed.save_result = 0;
    led_pattern_cs_ble_speed.write_result = -ENOSPC;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] != 0 && persist_work.scheduled);
    led_pattern_cs_ble_speed.write_result = 0;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] == 0 && led_pattern_cs_ble_speed.persists == 1);
}

static void key_during_notification(void) {
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 40) == 0);
}
static void client_during_notification(void) {
    const struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(80);
    assert(zmk_custom_setting_write(&led_pattern_cs_usb_brightness, &value,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) == 0);
}
static void test_usb_store(void) {
    selected = ZMK_TRANSPORT_USB;
    const int writes = led_pattern_cs_usb_pattern.memory_writes;
    const int notices = notification_count;
    const int events = core_events;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 5) == 0);
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 6) == 0);
    assert(led_pattern_cs_usb_pattern.value.int32_value == 6 && led_state.pattern == 6);
    assert(led_pattern_cs_usb_pattern.memory_writes == writes + 2);
    assert(notification_count == notices && core_events == events + 2);
    assert(usb_coalesce_work.scheduled && usb_coalesce_work.delay_ms == LED_PATTERN_USB_COALESCE_MS);
    run(&usb_coalesce_work);
    assert(notification_count == notices + 1 && !usb_coalesce_work.scheduled);
    run(&persist_work);
    assert(led_pattern_cs_usb_pattern.persistent_value == 6);

    /* A controlled event-dispatch failure retries. This does not model the
     * Studio queue or transport's asynchronous delivery failures. A newer key
     * during dispatch must also remain pending. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 30) == 0);
    const int warned = warnings;
    notification_result = -EIO;
    run(&usb_coalesce_work);
    assert(usb_coalesce_work.scheduled && warnings == warned + 1);
    notification_result = 0;
    notification_hook = key_during_notification;
    run(&usb_coalesce_work);
    assert(usb_coalesce_work.scheduled && led_brightness == 40);
    run(&usb_coalesce_work);
    assert(!usb_coalesce_work.scheduled && led_brightness == 40);
    const int emitted = notification_count;
    run(&usb_coalesce_work);
    assert(notification_count == emitted);

    /* A client commits between a queued notification peek and its delivery. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 20) == 0);
    notification_hook = client_during_notification;
    run(&usb_coalesce_work);
    assert(led_pattern_cs_usb_brightness.value.int32_value == 80 && led_brightness == 80);
    assert(!usb_coalesce_work.scheduled);

    static const struct { bool ready; int result; uint32_t dtr; } closed[] = {
        {false, 0, 1}, {true, -ENOTSUP, 1}, {true, 0, 0},
    };
    for (size_t i = 0; i < ARRAY_SIZE(closed); i++) {
        studio_uart.ready = closed[i].ready;
        dtr_result = closed[i].result;
        dtr_level = closed[i].dtr;
        const int count = notification_count;
        assert(led_pattern_settings_store(LED_PATTERN_FIELD_SPEED, 150 + (int32_t)i) == 0);
        run(&usb_coalesce_work);
        run(&persist_work);
        assert(notification_count == count);
        assert(led_pattern_cs_usb_speed.persistent_value == 150 + (int32_t)i);
    }
    studio_uart.ready = true;
    dtr_result = 0;
    dtr_level = 1;
    const int32_t old_registry_brightness = led_pattern_cs_usb_brightness.value.int32_value;
    const int old_brightness = led_brightness;
    assert(unsaved[TRANSPORT_USB] == 0 && usb_pending.mask == 0 && !persist_work.scheduled);
    led_pattern_cs_usb_brightness.write_result = -EIO;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 30) == -EIO);
    assert(led_pattern_cs_usb_brightness.value.int32_value == old_registry_brightness);
    assert(led_brightness == old_brightness);
    assert(unsaved[TRANSPORT_USB] == 0 && usb_pending.mask == 0);
    assert(!usb_coalesce_work.scheduled && !persist_work.scheduled);
    led_pattern_cs_usb_brightness.write_result = 0;
    /* led_pattern_request's actual fallback is tested in standalone_harness.
     * Once storage recovers, this bridge applies the next committed value. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 71) == 0);
    assert(led_pattern_cs_usb_brightness.value.int32_value == 71 && led_brightness == 71);
    assert(atomic_test_bit(&unsaved[TRANSPORT_USB], LED_PATTERN_FIELD_BRIGHTNESS));
    assert(usb_pending.mask != 0 && usb_coalesce_work.scheduled && persist_work.scheduled);
    run(&usb_coalesce_work);
    run(&persist_work);
    assert(usb_pending.mask == 0 && unsaved[TRANSPORT_USB] == 0);
    assert(led_pattern_cs_usb_brightness.persistent_value == 71);
    /* Bounded repeat pressure retains the final registry value. */
    for (int i = 0; i < 1000; i++) {
        assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, i % 101) == 0);
    }
    assert(led_pattern_cs_usb_brightness.value.int32_value == 90 && led_brightness == 90);
    run(&usb_coalesce_work);
    run(&persist_work);
    assert(led_pattern_cs_usb_brightness.persistent_value == 90);
    selected = ZMK_TRANSPORT_BLE;
}

static void test_pending_usb_save_after_transport_change(void) {
    selected = ZMK_TRANSPORT_USB;
    dtr_level = 0;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 28) == 0);
    selected = ZMK_TRANSPORT_BLE;
    apply();
    const uint8_t ble_brightness = led_brightness;
    const int notifications = notification_count;
    run(&persist_work);
    run(&usb_coalesce_work);
    assert(led_pattern_cs_usb_brightness.persistent_value == 28);
    assert(unsaved[TRANSPORT_USB] == 0 && usb_pending.mask == 0);
    assert(led_brightness == ble_brightness); /* The inactive set cannot take over. */
    assert(notification_count == notifications + 2);
    dtr_level = 1;
}

static void test_registry_fields_are_independent(void) {
    selected = ZMK_TRANSPORT_USB;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 5) == 0);
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 80) == 0);
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_SPEED, 175) == 0);
    assert(led_state.pattern == 5 && led_state.speed == 175 && led_brightness == 80);
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 40) == 0);
    assert(led_state.pattern == 5 && led_state.speed == 175 && led_brightness == 40);
    run(&usb_coalesce_work);
    run(&persist_work);
    assert(led_pattern_cs_usb_pattern.persistent_value == 5);
    assert(led_pattern_cs_usb_speed.persistent_value == 175);
    assert(led_pattern_cs_usb_brightness.persistent_value == 40);
    assert(unsaved[TRANSPORT_USB] == 0 && usb_pending.mask == 0);
}

int main(void) {
    const char *regression = getenv("LED_SETTINGS_REGRESSION");
    if (regression != NULL) {
        if (strcmp(regression, "client") == 0) test_newer_client_edit_wins();
        else if (strcmp(regression, "persist") == 0) test_newer_client_during_save();
        else abort();
        return 0;
    }
    const struct zmk_custom_setting foreign = {.custom_subsystem_id="other__module"};
    const zmk_event_t unrelated = {1, &foreign};
    led_pattern_cs_ble_brightness.value.int32_value = 73;
    const int before_brightnesses = set_brightnesses;
    assert(led_pattern_settings_event_cb(&unrelated) == ZMK_EV_EVENT_BUBBLE);
    assert(set_brightnesses == before_brightnesses && led_brightness == 100);
    const zmk_event_t own = {1, &led_pattern_cs_ble_brightness};
    assert(led_pattern_settings_event_cb(&own) == ZMK_EV_EVENT_BUBBLE);
    assert(led_brightness == 73);
    led_pattern_cs_ble_brightness.value.int32_value = 100;
    const zmk_event_t endpoint_changed = {2, NULL};
    assert(led_pattern_settings_event_cb(&endpoint_changed) == ZMK_EV_EVENT_BUBBLE);
    assert(led_brightness == 100);
    set_states = 0; set_brightnesses = 0;
    assert(!led_patterns_namespace_handler(NULL, NULL));
    test_apply();
    test_ble_store();
    test_usb_store();
    test_pending_usb_save_after_transport_change();
    test_registry_fields_are_independent();
    assert(suppress_depth == 0 && usb_pending_mutex.depth == 0 && persist_mutex.depth == 0);
    test_newer_client_edit_wins();
    test_newer_client_during_save();
    puts("led settings bridge: PASS");
    return 0;
}
