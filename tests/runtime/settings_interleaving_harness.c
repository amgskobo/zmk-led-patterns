/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Actual settings bridge callbacks with two deterministic writer threads.
 * The registry and desired-state stand-ins do not implement LED rendering.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <zmk-led-patterns/led_pattern.h>

#define IS_ENABLED(option) option
#define CONFIG_ZMK_USB 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LED_PATTERNS_SUBSYSTEM "amgskobo__led"
#define K_FOREVER 0

struct k_mutex {
    pthread_mutex_t native;
};
static struct k_mutex settings_apply_mutex = {PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t coordination = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t desired_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int writer_role;
static bool older_paused;
static bool newer_blocked;
static bool newer_finished;
static bool pause_pending;

/* A real contention result, rather than a time delay, releases the older
 * callback. Without serialization the newer callback completes first.
 * The negative control changes only this kernel stand-in, never source. */
static __attribute__((unused)) int k_mutex_lock(struct k_mutex *mutex, int timeout) {
    (void)timeout;
    assert(mutex == &settings_apply_mutex);
#ifdef LED_SETTINGS_SKIP_APPLY_SERIALIZATION
    return 0;
#else
    if (writer_role == 2) {
        int result = pthread_mutex_trylock(&mutex->native);
        if (result == 0)
            return 0;
        assert(result == EBUSY);
        assert(pthread_mutex_lock(&coordination) == 0);
        newer_blocked = true;
        assert(pthread_cond_broadcast(&changed) == 0);
        assert(pthread_mutex_unlock(&coordination) == 0);
    }
    return pthread_mutex_lock(&mutex->native);
#endif
}
static __attribute__((unused)) int k_mutex_unlock(struct k_mutex *mutex) {
    assert(mutex == &settings_apply_mutex);
#ifdef LED_SETTINGS_SKIP_APPLY_SERIALIZATION
    return 0;
#else
    return pthread_mutex_unlock(&mutex->native);
#endif
}

enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance {
    enum zmk_transport transport;
};
static enum zmk_transport selected;
static struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){.transport = selected};
}
enum zmk_custom_setting_value_type {
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
};
struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    union {
        int32_t int32_value;
        bool bool_value;
    };
};
struct zmk_custom_setting {
    const char *key;
    const char *custom_subsystem_id;
    struct zmk_custom_setting_value value;
};
struct zmk_custom_setting_changed {
    const struct zmk_custom_setting *setting;
};
typedef struct {
    struct zmk_custom_setting_changed change;
} zmk_event_t;
static const struct zmk_custom_setting_changed *
as_zmk_custom_setting_changed(const zmk_event_t *event) {
    return &event->change;
}
struct transport_settings {
    const char *name;
    const struct zmk_custom_setting *pattern;
    const struct zmk_custom_setting *speed;
    const struct zmk_custom_setting *brightness;
    const struct zmk_custom_setting *idle_off;
};
#define INT_SETTING(name)                                                                          \
    static struct zmk_custom_setting name = {.key = #name,                                         \
                                             .custom_subsystem_id = ZMK_LED_PATTERNS_SUBSYSTEM,    \
                                             .value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32}
#define BOOL_SETTING(name)                                                                         \
    static struct zmk_custom_setting name = {.key = #name,                                         \
                                             .custom_subsystem_id = ZMK_LED_PATTERNS_SUBSYSTEM,    \
                                             .value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL}
INT_SETTING(usb_pattern);
INT_SETTING(usb_speed);
INT_SETTING(usb_brightness);
BOOL_SETTING(usb_idle_off);
INT_SETTING(ble_pattern);
INT_SETTING(ble_speed);
INT_SETTING(ble_brightness);
BOOL_SETTING(ble_idle_off);
BOOL_SETTING(led_pattern_cs_adv_blink);
static const struct transport_settings usb_transport = {"usb", &usb_pattern, &usb_speed,
                                                        &usb_brightness, &usb_idle_off};
static const struct transport_settings ble_transport = {"ble", &ble_pattern, &ble_speed,
                                                        &ble_brightness, &ble_idle_off};
static struct zmk_custom_setting *live_pattern;
static struct led_pattern_state desired;
static uint8_t desired_brightness;
static struct {
    int role;
    uint8_t pattern;
    uint16_t speed;
} applied[4];
static size_t applied_count;

static int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                                   struct zmk_custom_setting_value *value) {
    assert(pthread_mutex_lock(&registry_lock) == 0);
    *value = setting->value;
    assert(pthread_mutex_unlock(&registry_lock) == 0);
    if (writer_role == 1 && setting == live_pattern && pause_pending) {
        pause_pending = false;
        assert(pthread_mutex_lock(&coordination) == 0);
        older_paused = true;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (!newer_blocked && !newer_finished)
            assert(pthread_cond_wait(&changed, &coordination) == 0);
        assert(pthread_mutex_unlock(&coordination) == 0);
    }
    return 0;
}
void led_pattern_get_state(struct led_pattern_state *out) {
    assert(pthread_mutex_lock(&desired_lock) == 0);
    *out = desired;
    assert(pthread_mutex_unlock(&desired_lock) == 0);
}
uint8_t led_pattern_get_brightness(void) {
    assert(pthread_mutex_lock(&desired_lock) == 0);
    uint8_t value = desired_brightness;
    assert(pthread_mutex_unlock(&desired_lock) == 0);
    return value;
}
void led_pattern_set_state(const struct led_pattern_state *state) {
    assert(pthread_mutex_lock(&desired_lock) == 0);
    desired = *state;
    assert(applied_count < sizeof(applied) / sizeof(applied[0]));
    applied[applied_count].role = writer_role;
    applied[applied_count].pattern = state->pattern;
    applied[applied_count++].speed = state->speed;
    assert(pthread_mutex_unlock(&desired_lock) == 0);
}
void led_pattern_set_presentation(const struct led_pattern_state *state, uint8_t brightness) {
    assert(pthread_mutex_lock(&desired_lock) == 0);
    desired = *state;
    desired_brightness = brightness;
    assert(applied_count < sizeof(applied) / sizeof(applied[0]));
    applied[applied_count].role = writer_role;
    applied[applied_count].pattern = state->pattern;
    applied[applied_count++].speed = state->speed;
    assert(pthread_mutex_unlock(&desired_lock) == 0);
}
void led_pattern_set_brightness(uint8_t value) {
    assert(pthread_mutex_lock(&desired_lock) == 0);
    desired_brightness = value;
    assert(pthread_mutex_unlock(&desired_lock) == 0);
}

/* DRIVER_FUNCTIONS */

static void *older_callback(void *unused) {
    (void)unused;
    writer_role = 1;
    const zmk_event_t event = {.change.setting = live_pattern};
    assert(led_pattern_settings_event_cb(&event) == ZMK_EV_EVENT_BUBBLE);
    return NULL;
}
static void *newer_writer(void *unused) {
    (void)unused;
    writer_role = 2;
    assert(pthread_mutex_lock(&coordination) == 0);
    while (!older_paused)
        assert(pthread_cond_wait(&changed, &coordination) == 0);
    assert(pthread_mutex_unlock(&coordination) == 0);
    assert(pthread_mutex_lock(&registry_lock) == 0);
    live_pattern->value.int32_value = LED_PATTERN_CANDLE;
    assert(pthread_mutex_unlock(&registry_lock) == 0);
    const zmk_event_t event = {.change.setting = live_pattern};
    assert(led_pattern_settings_event_cb(&event) == ZMK_EV_EVENT_BUBBLE);
    assert(pthread_mutex_lock(&coordination) == 0);
    newer_finished = true;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&coordination) == 0);
    return NULL;
}
static void check_transport(enum zmk_transport transport) {
    printf("starting %s callback overlap\n", transport == ZMK_TRANSPORT_USB ? "USB" : "BLE");
    fflush(stdout);
    selected = transport;
    struct zmk_custom_setting *pattern =
        transport == ZMK_TRANSPORT_USB ? &usb_pattern : &ble_pattern;
    struct zmk_custom_setting *speed = transport == ZMK_TRANSPORT_USB ? &usb_speed : &ble_speed;
    struct zmk_custom_setting *brightness =
        transport == ZMK_TRANSPORT_USB ? &usb_brightness : &ble_brightness;
    struct zmk_custom_setting *idle =
        transport == ZMK_TRANSPORT_USB ? &usb_idle_off : &ble_idle_off;
    live_pattern = pattern;
    pattern->value.int32_value = LED_PATTERN_BREATHE;
    speed->value.int32_value = 200;
    brightness->value.int32_value = 50;
    idle->value.bool_value = false;
    led_pattern_cs_adv_blink.value.bool_value = false;
    desired = (struct led_pattern_state){LED_PATTERN_BREATHE, 100, true, true};
    desired_brightness = 100;
    applied_count = 0;
    older_paused = newer_blocked = newer_finished = false;
    pause_pending = true;
    pthread_t older, newer;
    assert(pthread_create(&older, NULL, older_callback, NULL) == 0);
    assert(pthread_create(&newer, NULL, newer_writer, NULL) == 0);
    assert(pthread_join(older, NULL) == 0);
    assert(pthread_join(newer, NULL) == 0);
    printf("%s callbacks:", transport == ZMK_TRANSPORT_USB ? "USB" : "BLE");
    for (size_t i = 0; i < applied_count; i++)
        printf(" role=%d pattern=%u speed=%u;", applied[i].role, applied[i].pattern,
               applied[i].speed);
    printf(" registry=%d desired=%u brightness=%u blocked=%d completed=%d\n",
           pattern->value.int32_value, desired.pattern, desired_brightness, newer_blocked,
           newer_finished);
    fflush(stdout);
    assert(pattern->value.int32_value == LED_PATTERN_CANDLE);
    assert(desired.pattern == LED_PATTERN_CANDLE);
    assert(desired.speed == 200 && desired_brightness == 50);
    assert(!desired.advertising_indicator && !desired.idle_off);
    assert(applied_count == 2 && applied[0].role == 1 && applied[1].role == 2);
    assert(newer_blocked && newer_finished);
}
int main(int argc, char **argv) {
    assert(argc == 1 ||
           (argc == 2 && (strcmp(argv[1], "usb") == 0 || strcmp(argv[1], "ble") == 0)));
    /* Failure-only watchdog: it never chooses or asserts callback order. */
    alarm(15);
    if (argc == 1 || strcmp(argv[1], "usb") == 0)
        check_transport(ZMK_TRANSPORT_USB);
    if (argc == 1 || strcmp(argv[1], "ble") == 0)
        check_transport(ZMK_TRANSPORT_BLE);
    alarm(0);
    puts("settings callback overlap preserves latest registry and desired "
         "values: PASS");
    return 0;
}
