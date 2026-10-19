/*
 * Copyright (c) 2025-2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Publishes the LED state through zmk-feature-custom-settings, and makes those
 * settings the persistent owner of it.
 *
 * Register a subsystem to own the namespace, register the values, listen for
 * the settings events, and apply. The deferred USB write and flash-save work
 * items are defined at compile time: an item initialised from an init can be
 * submitted before it exists, and that has already cost this module a keyboard
 * that would not boot.
 *
 * The pattern, its brightness, its speed and the idle-off switch are
 * registered as one set per transport, because the two hosts are not looked at
 * under the same conditions, plus the one shared advertising indicator.
 * Brightness is here rather than left to `&bl` because this module owns the
 * LED and does not use ZMK's backlight subsystem. The live set follows
 * zmk_endpoint_changed, so unplugging USB changes the light rather than
 * changing nothing.
 *
 * A BLE keymap binding writes the live setting immediately. USB key presses
 * commit their setting immediately but coalesce Studio notifications: a burst of
 * Studio notifications can otherwise fill the USB transport and stall input.
 * The final value reaches Studio after the short quiet interval below.
 *
 * The split halves are not this file's business. The behaviors run on the
 * central, and the controller mirrors whatever the central ends up showing to
 * the peripheral as final state, so a peripheral registers none of these.
 */

#define DT_DRV_COMPAT zmk_behavior_led_pattern

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/custom.h>
#include <zmk/workqueue.h>

#include <zmk-led-patterns/custom_settings.h>
#include <zmk-led-patterns/led_pattern.h>
#include "usb_pending.h"

#ifndef CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL
#define CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_led_patterns_behavior_led_pattern_custom_settings, CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL);

static bool led_patterns_namespace_handler(const zmk_custom_CallRequest *request,
                                           pb_callback_t *encode_response);

/*
 * A setting belongs to a custom subsystem, and custom-settings resolves that
 * identifier to an index before it can put the setting on the wire: without a
 * registered subsystem of the same name, every one of these settings is
 * dropped with -ENOENT and never reaches a client, however correctly it was
 * defined. So the subsystem is registered here purely to own the namespace.
 *
 * It answers no calls of its own -- the values are read and written through
 * custom-settings' own RPC -- so the handler declines every request and the
 * module needs no protocol, no nanopb, and no page of its own.
 */
static struct zmk_rpc_custom_subsystem_meta led_patterns_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/amgskobo/zmk-led-patterns"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

/*
 * Through a wrapper so the token expands before it is stringified.
 *
 * ZMK_RPC_CUSTOM_SUBSYSTEM registers `#_identifier`, and `#` suppresses
 * expansion of its own argument, so passing the macro straight in would
 * register the literal text "ZMK_LED_PATTERNS_SUBSYSTEM_TOKEN". One more
 * layer of call expands it first.
 */
#define REGISTER_SUBSYSTEM(identifier, meta, handler)                                              \
    ZMK_RPC_CUSTOM_SUBSYSTEM(identifier, meta, handler)

REGISTER_SUBSYSTEM(ZMK_LED_PATTERNS_SUBSYSTEM_TOKEN, &led_patterns_meta,
                   led_patterns_namespace_handler);

static bool led_patterns_namespace_handler(const zmk_custom_CallRequest *request,
                                           pb_callback_t *encode_response) {
    ARG_UNUSED(request);
    ARG_UNUSED(encode_response);

    return false;
}

#define LED_PATTERN_SETTING(name, key, type, default_value, constraint)                            \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(                                                    \
        name, ZMK_LED_PATTERNS_SUBSYSTEM, ZMK_LED_PATTERNS_SETTING_KEY(key), type, default_value,  \
        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,     \
        ZMK_CUSTOM_SETTING_PERMISSION_SECURE, constraint);

ZMK_LED_PATTERNS_ASSERT_KEY_FITS("usb_pattern")

/*
 * A range and not a named list of the eighteen patterns.
 *
 * The Studio RPC schema caps an options constraint at eight values and eight
 * labels (SettingConstraintOptions in custom_settings.options), and the
 * handler silently clamps to that, so a list of eighteen would arrive at a
 * client showing the first eight and quietly losing the other ten. The names
 * live where they can be complete instead: in the behavior's parameter
 * metadata, which a keymap editor draws, and in the public dt-bindings header.
 *
 * The defaults repeat the ones the controller starts on rather than reading
 * them from it: a default has to be a constant expression, and it is also what
 * a client's "reset" restores to, so it belongs with the declaration.
 */
#define LED_PATTERN_RANGE ZMK_CUSTOM_SETTING_RANGE_INT32(0, LED_PATTERN_COUNT - 1)

#define LED_PATTERN_TRANSPORT_SETTINGS(prefix, default_pattern)                                    \
    LED_PATTERN_SETTING(led_pattern_cs_##prefix##_pattern, #prefix "_pattern",                     \
                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                       \
                        ZMK_CUSTOM_SETTING_VALUE_INT32(default_pattern), LED_PATTERN_RANGE)        \
    LED_PATTERN_SETTING(led_pattern_cs_##prefix##_speed, #prefix "_speed",                         \
                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                       \
                        ZMK_CUSTOM_SETTING_VALUE_INT32(LED_PATTERN_SPEED_NOMINAL),                 \
                        ZMK_CUSTOM_SETTING_RANGE_INT32(LED_PATTERN_SPEED_MIN,                      \
                                                        LED_PATTERN_SPEED_MAX))                      \
    LED_PATTERN_SETTING(led_pattern_cs_##prefix##_brightness, #prefix "_brightness",               \
                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(100),  \
                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100))                                    \
    LED_PATTERN_SETTING(led_pattern_cs_##prefix##_idle_off, #prefix "_idle_off",                   \
                        ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(true),   \
                        ZMK_CUSTOM_SETTING_NO_CONSTRAINT)

/*
 * Both transports default to Breathe, which is the resting state of the split
 * connection indicator: a pair that has linked and agreed its state ends on
 * pattern 1, the same value the controller starts on, so "healthy" looks the
 * same on a keyboard whose settings have never been touched and on one whose
 * client has saved the defaults back. Set them to different values in a client
 * if the two hosts should be told apart -- the settings are still per
 * transport, only the factory value is shared.
 */
LED_PATTERN_TRANSPORT_SETTINGS(usb, LED_PATTERN_BREATHE)
LED_PATTERN_TRANSPORT_SETTINGS(ble, LED_PATTERN_BREATHE)

/* Not per transport: a peripheral has no endpoint, and the indicator is about
 * having no connection at all rather than about which one. */
LED_PATTERN_SETTING(led_pattern_cs_adv_blink, "adv_blink", ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
                    ZMK_CUSTOM_SETTING_VALUE_BOOL(true), ZMK_CUSTOM_SETTING_NO_CONSTRAINT)

struct transport_settings {
    const char *name;
    const struct zmk_custom_setting *pattern;
    const struct zmk_custom_setting *speed;
    const struct zmk_custom_setting *brightness;
    /*
     * Per transport, and the one value here with a real running cost. USB
     * means a cable, where keeping the LED lit through idle costs nothing that
     * matters; BLE means a battery, where it is the only setting in this
     * module that really shortens the day.
     */
    const struct zmk_custom_setting *idle_off;
};

static const struct transport_settings usb_transport = {
    .name = "usb",
    .pattern = &led_pattern_cs_usb_pattern,
    .speed = &led_pattern_cs_usb_speed,
    .brightness = &led_pattern_cs_usb_brightness,
    .idle_off = &led_pattern_cs_usb_idle_off,
};

static const struct transport_settings ble_transport = {
    .name = "ble",
    .pattern = &led_pattern_cs_ble_pattern,
    .speed = &led_pattern_cs_ble_speed,
    .brightness = &led_pattern_cs_ble_brightness,
    .idle_off = &led_pattern_cs_ble_idle_off,
};

/*
 * Which set the live transport is showing.
 *
 * USB is the only transport that is not BLE here, so anything else -- a BLE
 * profile, or no endpoint at all while advertising -- reads the BLE set. That
 * is the useful reading rather than a strict one: while advertising, BLE is
 * what the keyboard is trying to be on.
 */
static const struct transport_settings *active_transport(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        return &usb_transport;
    }
#endif

    return &ble_transport;
}

/* Only notifications are deferred. The registry is updated on the key thread,
 * so a later Studio edit can never be overwritten by an older queued value. */
#define LED_PATTERN_USB_COALESCE_MS 300
K_MUTEX_DEFINE(usb_pending_mutex);
static struct led_pattern_usb_pending usb_pending;

static bool read_int32(const struct zmk_custom_setting *setting, int32_t *out) {
    struct zmk_custom_setting_value value;

    int ret = zmk_custom_setting_read(setting, &value);
    if (ret != 0) {
        LOG_WRN("led: read of \"%s\" failed (%d)", setting->key, ret);
        return false;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        LOG_WRN("led: \"%s\" came back as type %d, not int32 -- ignored", setting->key,
                (int)value.type);
        return false;
    }

    *out = value.int32_value;

    return true;
}

static bool read_bool(const struct zmk_custom_setting *setting, bool *out) {
    struct zmk_custom_setting_value value;

    if (zmk_custom_setting_read(setting, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return false;
    }

    *out = value.bool_value;

    return true;
}

/* Registry writes dispatch after releasing their state lock. Serialize this
 * complete read/apply operation so an older callback cannot finish after a
 * newer callback and leave the immediate desired getters behind the registry.
 * Static initialization also covers endpoint events before controller init;
 * no registry or controller metadata lock is held while acquiring this mutex. */
K_MUTEX_DEFINE(settings_apply_mutex);

/*
 * Applied as a set, starting from what the LED is running now, so a value that
 * cannot be read leaves its field alone instead of resetting it.
 */
static void led_pattern_apply_settings(void) {
    const struct transport_settings *live = active_transport();
    struct led_pattern_state state;
    int32_t pattern;
    int32_t speed;
    int32_t brightness;
    /* Only used after a successful read; initialised so that nothing read
     * from the stack can ever reach the LED state. */
    bool adv_blink = false;
    bool idle_off = false;

    led_pattern_get_state(&state);

    if (read_int32(live->pattern, &pattern) && pattern >= 0 && pattern < LED_PATTERN_COUNT) {
        state.pattern = (uint8_t)pattern;
    }
    if (read_int32(live->speed, &speed)) {
        state.speed = (uint16_t)CLAMP(speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    }
    uint8_t next_brightness = led_pattern_get_brightness();
    if (read_int32(live->brightness, &brightness)) {
        next_brightness = (uint8_t)CLAMP(brightness, 0, 100);
    }
    if (read_bool(&led_pattern_cs_adv_blink, &adv_blink)) {
        state.advertising_indicator = adv_blink;
    }
    if (read_bool(live->idle_off, &idle_off)) {
        state.idle_off = idle_off;
    }

    LOG_DBG("led: applying %s -> pattern %u speed %u indicator %d idle-off %d", live->name,
            state.pattern, state.speed, (int)state.advertising_indicator, (int)state.idle_off);

    struct led_pattern_state current;
    led_pattern_get_state(&current);
    if (state.pattern != current.pattern || state.speed != current.speed ||
        state.advertising_indicator != current.advertising_indicator ||
        state.idle_off != current.idle_off ||
        next_brightness != led_pattern_get_brightness()) {
        led_pattern_set_presentation(&state, next_brightness);
    }
}

static int led_pattern_settings_event_cb(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *changed = as_zmk_custom_setting_changed(eh);

    if (changed != NULL &&
        strcmp(changed->setting->custom_subsystem_id, ZMK_LED_PATTERNS_SUBSYSTEM) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /*
     * Initialization, an endpoint change or a change to our own settings may
     * alter what the LED should show. Re-reading five scalars is cheaper
     * than working out which one did.
     */
    k_mutex_lock(&settings_apply_mutex, K_FOREVER);
    led_pattern_apply_settings();
    k_mutex_unlock(&settings_apply_mutex);

    return ZMK_EV_EVENT_BUBBLE;
}

/*
 * Applied on these three signals, and deliberately not from a SYS_INIT.
 *
 * zmk_custom_settings_initialized fires from the settings-subtree commit that
 * ends the boot settings_load pass, which is the only point at which a stored
 * value is both present and readable. A SYS_INIT is too early: it runs before
 * settings_load(), so it would read the declared default and leave the LED on
 * it for the rest of the session -- the value would persist and show correctly
 * in a client while having no effect on the hardware.
 *
 * The load path stores values without raising zmk_custom_setting_changed, so
 * that event alone would never deliver a stored value either. Together the two
 * cover boot and every later edit. zmk_endpoint_changed is the third because
 * the live set is selected by transport, and it is the one that can arrive
 * during init -- harmlessly, since applying is only reads and assignments.
 */
ZMK_LISTENER(led_pattern_custom_settings, led_pattern_settings_event_cb);
ZMK_SUBSCRIPTION(led_pattern_custom_settings, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(led_pattern_custom_settings, zmk_custom_settings_initialized);
ZMK_SUBSCRIPTION(led_pattern_custom_settings, zmk_endpoint_changed);

/* ===== Keymap bindings write the live setting ===== */

/*
 * How long the keys have to be quiet before a changed value goes to flash.
 *
 * A PERSIST write goes straight to settings_save_one() with no debounce of its
 * own, so writing it on every press would be one flash write per press. In
 * memory first and flash once the run is over is one per setting, and the
 * dirty mark a client shows in between clears itself when the save lands.
 */
#define LED_PATTERN_PERSIST_DELAY_MS 3000

enum { TRANSPORT_USB, TRANSPORT_BLE };

static const struct transport_settings *const transports[] = {
    [TRANSPORT_USB] = &usb_transport,
    [TRANSPORT_BLE] = &ble_transport,
};

/* Per transport, the fields a key press has changed in memory and not saved.
 * Set on the thread the press arrives on, cleared on the low-priority queue. */
static atomic_t unsaved[ARRAY_SIZE(transports)];

/* Serialize this module's key writes and dirty bookkeeping with its saver.
 * Direct clients use the registry lock; save() atomically persists the latest
 * registry value without writing an earlier read back over a newer edit. */
K_MUTEX_DEFINE(persist_mutex);

/* USB key writes always defer their Studio notification; core observers still
 * receive the change synchronously. Saves suppress Studio notifications only
 * while its USB CDC port is closed. BLE events retain their ordinary path. */
static bool usb_studio_port_closed(void) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART) && IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zmk_studio_rpc_uart));
        uint32_t dtr = 0;
        const bool ready = device_is_ready(uart);
        const bool dtr_read_succeeded =
            ready && uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) == 0;
        return led_pattern_usb_port_closed(true, ready, dtr_read_succeeded, dtr);
    }
#endif
    return false;
}

/* Quiet writes still dispatch core events immediately. Older supported DYA
 * custom-settings revisions expose only the legacy suppression bracket; that
 * compatibility path inherits its global-suppression concurrency limitation. */
static int write_led_setting(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value, bool quiet) {
    if (!quiet) {
        return zmk_custom_setting_write(setting, value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    }
#if defined(ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES)
    return zmk_custom_setting_write_quiet(setting, value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
#else
    zmk_custom_settings_notify_suppress_begin();
    int ret = zmk_custom_setting_write(setting, value, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    zmk_custom_settings_notify_suppress_end();
    return ret;
#endif
}

static int save_led_setting(const struct zmk_custom_setting *setting, bool quiet) {
    if (!quiet) {
        return zmk_custom_setting_save(setting);
    }
#if defined(ZMK_CUSTOM_SETTINGS_HAS_QUIET_WRITES)
    return zmk_custom_setting_save_quiet(setting);
#else
    zmk_custom_settings_notify_suppress_begin();
    int ret = zmk_custom_setting_save(setting);
    zmk_custom_settings_notify_suppress_end();
    return ret;
#endif
}

static const struct zmk_custom_setting *field_setting(const struct transport_settings *transport,
                                                      enum led_pattern_field field) {
    switch (field) {
    case LED_PATTERN_FIELD_PATTERN:
        return transport->pattern;
    case LED_PATTERN_FIELD_BRIGHTNESS:
        return transport->brightness;
    case LED_PATTERN_FIELD_SPEED:
        return transport->speed;
    default:
        return NULL;
    }
}

static void persist_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(persist_work, persist_work_handler);

static void usb_coalesce_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(usb_coalesce_work, usb_coalesce_work_handler);

static void usb_coalesce_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool retry = false;

    for (int field = LED_PATTERN_FIELD_PATTERN; field <= LED_PATTERN_FIELD_SPEED; field++) {
        k_mutex_lock(&usb_pending_mutex, K_FOREVER);
        uint32_t version = 0;
        const bool pending = led_pattern_usb_pending_peek(&usb_pending, field, &version);
        k_mutex_unlock(&usb_pending_mutex);
        if (!pending) {
            continue;
        }

        int ret = 0;
        if (!usb_studio_port_closed()) {
            ret = raise_zmk_custom_setting_changed((struct zmk_custom_setting_changed){
                .setting = field_setting(&usb_transport, field),
                .kind = ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
                .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
            });
        }
        k_mutex_lock(&usb_pending_mutex, K_FOREVER);
        if (led_pattern_usb_pending_finish(&usb_pending, field, version, ret >= 0)) {
            retry = true;
        }
        k_mutex_unlock(&usb_pending_mutex);
        if (ret < 0) {
            LOG_WRN("led: notifying USB field %d failed (%d)", field, ret);
        }
    }
    if (retry) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_coalesce_work,
                                    K_MSEC(LED_PATTERN_USB_COALESCE_MS));
    }
}

static void persist_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    bool retry = false;

    for (size_t t = 0; t < ARRAY_SIZE(transports); t++) {
        for (int field = LED_PATTERN_FIELD_PATTERN; field <= LED_PATTERN_FIELD_SPEED; field++) {
            if (!atomic_test_bit(&unsaved[t], field)) {
                continue;
            }

            const struct zmk_custom_setting *setting = field_setting(transports[t], field);

            /* Serialize with the memory write and clear the dirty bit only
             * after flash accepted the value. A transient storage failure must
             * not silently turn a live setting into a value lost at reboot. */
            k_mutex_lock(&persist_mutex, K_FOREVER);
            int ret = save_led_setting(setting, t == TRANSPORT_USB && usb_studio_port_closed());
            if (ret == 0) {
                atomic_clear_bit(&unsaved[t], field);
            } else {
                retry = true;
                LOG_WRN("led: saving \"%s\" failed (%d)", setting->key, ret);
            }
            k_mutex_unlock(&persist_mutex);
        }
    }

    if (retry) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &persist_work,
                                    K_MSEC(LED_PATTERN_PERSIST_DELAY_MS));
    }
}

/* Both transports commit their registry value immediately. Only USB Studio
 * notifications wait for a quiet interval; flash follows the final key write. */
int led_pattern_settings_store(enum led_pattern_field field, int32_t value) {
    const struct transport_settings *live = active_transport();
    const struct zmk_custom_setting *setting = field_setting(live, field);
    if (setting == NULL) {
        return -EINVAL;
    }

    const bool usb = live == &usb_transport;
    const struct zmk_custom_setting_value new_value = ZMK_CUSTOM_SETTING_VALUE_INT32(value);
    k_mutex_lock(&persist_mutex, K_FOREVER);
    int ret = write_led_setting(setting, &new_value, usb);
    if (ret == 0) {
        atomic_set_bit(&unsaved[usb ? TRANSPORT_USB : TRANSPORT_BLE], field);
        if (usb) {
            k_mutex_lock(&usb_pending_mutex, K_FOREVER);
            led_pattern_usb_pending_mark(&usb_pending, field);
            k_mutex_unlock(&usb_pending_mutex);
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_coalesce_work,
                                        K_MSEC(LED_PATTERN_USB_COALESCE_MS));
        }
    }
    k_mutex_unlock(&persist_mutex);
    if (ret < 0) {
        LOG_WRN("led: writing %d to \"%s\" failed (%d)", value, setting->key, ret);
        return ret;
    }
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &persist_work,
                                K_MSEC(LED_PATTERN_PERSIST_DELAY_MS));
    return 0;
}
