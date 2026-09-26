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
#include <string.h>

#include <zmk-led-patterns/led_pattern.h>
#include "usb_pending.h"

#define IS_ENABLED(option) option
#define CONFIG_ZMK_USB 1
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
typedef struct { int unused; } zmk_event_t;
typedef struct { int unused; } zmk_custom_CallRequest;
typedef struct { int unused; } pb_callback_t;
struct k_mutex { int depth; };
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
};
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
        key, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (v)}, 0, 0, 0, 0}
#define SETTING_BOOL(name, key, v)                                                                 \
    static struct zmk_custom_setting name = {                                                      \
        key, {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = (v)}, 0, 0, 0, 0}
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
static int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                                   struct zmk_custom_setting_value *value) {
    *value = setting->value;
    return setting->read_result;
}
static int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode) {
    struct zmk_custom_setting *mutable = (struct zmk_custom_setting *)setting;
    if (mutable->write_result != 0) {
        return mutable->write_result;
    }
    mutable->value = *value;
    if (mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) {
        mutable->persists++;
    } else {
        mutable->memory_writes++;
    }
    suppressed_writes += suppress_depth;
    return 0;
}
static void zmk_custom_settings_notify_suppress_begin(void) { assert(suppress_depth++ == 0); }
static void zmk_custom_settings_notify_suppress_end(void) { assert(--suppress_depth == 0); }
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

    /* On USB the USB set is live, with key presses not yet written on top. */
    selected = ZMK_TRANSPORT_USB;
    led_pattern_usb_pending_put(&usb_pending, LED_PATTERN_FIELD_PATTERN, LED_PATTERN_CANDLE);
    apply();
    assert(led_state.pattern == LED_PATTERN_CANDLE && led_state.speed == 100);
    assert(led_brightness == 100);
    usb_pending.mask = 0;
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

    /* A failed read or a failed flash write keeps the mark and retries. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_SPEED, 120) == 0);
    led_pattern_cs_ble_speed.read_result = -EIO;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] != 0 && persist_work.scheduled);
    led_pattern_cs_ble_speed.read_result = 0;
    led_pattern_cs_ble_speed.write_result = -ENOSPC;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] != 0 && persist_work.scheduled);
    led_pattern_cs_ble_speed.write_result = 0;
    run(&persist_work);
    assert(unsaved[TRANSPORT_BLE] == 0 && led_pattern_cs_ble_speed.persists == 1);
}

static void test_usb_store(void) {
    selected = ZMK_TRANSPORT_USB;

    /* Key presses on USB are coalesced: the caller lights the LED, the
     * setting is written once the keys go quiet. */
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 5) == 1);
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_PATTERN, 6) == 1);
    assert(usb_coalesce_work.scheduled &&
           usb_coalesce_work.delay_ms == LED_PATTERN_USB_COALESCE_MS);
    assert(led_pattern_cs_usb_pattern.memory_writes == 0);

    /* With Studio listening on the port, the write notifies as usual. */
    dtr_level = 1;
    run(&usb_coalesce_work);
    assert(led_pattern_cs_usb_pattern.value.int32_value == 6 && suppressed_writes == 0);
    assert(led_pattern_cs_usb_pattern.memory_writes == 1 && !usb_coalesce_work.scheduled);
    assert(atomic_test_bit(&unsaved[TRANSPORT_USB], LED_PATTERN_FIELD_PATTERN));
    run(&persist_work);
    assert(led_pattern_cs_usb_pattern.persists == 1);

    /* With the port closed, or its state unknown, notifications are held. */
    static const struct { bool ready; int result; uint32_t dtr; } closed[] = {
        {false, 0, 1}, {true, -ENOTSUP, 1}, {true, 0, 0},
    };
    for (size_t i = 0; i < ARRAY_SIZE(closed); i++) {
        studio_uart.ready = closed[i].ready;
        dtr_result = closed[i].result;
        dtr_level = closed[i].dtr;
        const int held = suppressed_writes;
        assert(led_pattern_settings_store(LED_PATTERN_FIELD_SPEED, 150 + (int32_t)i) == 1);
        run(&usb_coalesce_work);
        assert(suppressed_writes == held + 1);
    }
    studio_uart.ready = true;
    dtr_result = 0;
    dtr_level = 1;

    /* A failed write, or a newer press during the write, is retried. */
    led_pattern_cs_usb_brightness.write_result = -EIO;
    assert(led_pattern_settings_store(LED_PATTERN_FIELD_BRIGHTNESS, 30) == 1);
    const int before = warnings;
    run(&usb_coalesce_work);
    assert(usb_coalesce_work.scheduled && warnings == before + 1);
    led_pattern_cs_usb_brightness.write_result = 0;
    run(&usb_coalesce_work);
    assert(!usb_coalesce_work.scheduled && led_pattern_cs_usb_brightness.value.int32_value == 30);
    assert(warnings == before + 1); /* a write that lands warns about nothing */
    run(&usb_coalesce_work); /* nothing pending */
    assert(!usb_coalesce_work.scheduled);

    selected = ZMK_TRANSPORT_BLE;
}

int main(void) {
    assert(!led_patterns_namespace_handler(NULL, NULL));
    test_apply();
    test_ble_store();
    test_usb_store();
    assert(suppress_depth == 0 && usb_pending_mutex.depth == 0 && persist_mutex.depth == 0);
    puts("led settings bridge: PASS");
    return 0;
}
