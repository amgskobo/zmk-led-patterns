/*
 * SPDX-License-Identifier: MIT
 *
 * Publishes the LED state through zmk-feature-custom-settings.
 *
 * Register a subsystem to own the namespace, register the values, listen for
 * the two settings events, and apply. Nothing else: no work item, no poll, no
 * gating on a flag of its own, and no private split relay. Every one of those
 * was tried and every one of them broke something -- a keyboard that would not
 * boot, and then an edit in a client that silently did nothing.
 *
 * Only what ZMK has no equivalent of is registered here. Brightness and on/off
 * are not: those are `&bl`, ZMK's own backlight behavior, which already saves
 * its value, relays it to both split halves and draws itself properly in a
 * keymap editor. What is left is the shape of the animation and how fast it
 * runs, one pair per transport, because the two hosts are not looked at under
 * the same conditions. The live pair follows zmk_endpoint_changed, so
 * unplugging USB changes the light rather than changing nothing.
 *
 * The split halves are ZMK's business, not this file's. A keymap press is
 * relayed by BEHAVIOR_LOCALITY_GLOBAL, which is ZMK's own mechanism and needs
 * no code here. To have a client's edit reach the peripheral as well, the
 * supported route is custom-settings' own relay:
 * CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY on both halves, with the
 * peripheral registering these same settings. That is deliberately not turned
 * on yet -- it is a real change to the peripheral's firmware -- but it is the
 * official one, and it replaces rather than joins anything here.
 */

#define DT_DRV_COMPAT zmk_behavior_led_pattern

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/custom.h>

#include <zmk-led-patterns/custom_settings.h>
#include <zmk-led-patterns/led_pattern.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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
                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100))

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
};

static const struct transport_settings usb_transport = {
    .name = "usb",
    .pattern = &led_pattern_cs_usb_pattern,
    .speed = &led_pattern_cs_usb_speed,
    .brightness = &led_pattern_cs_usb_brightness,
};

static const struct transport_settings ble_transport = {
    .name = "ble",
    .pattern = &led_pattern_cs_ble_pattern,
    .speed = &led_pattern_cs_ble_speed,
    .brightness = &led_pattern_cs_ble_brightness,
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
    bool adv_blink;

    led_pattern_get_state(&state);

    if (read_int32(live->pattern, &pattern) && pattern >= 0 && pattern < LED_PATTERN_COUNT) {
        state.pattern = (uint8_t)pattern;
    }
    if (read_int32(live->speed, &speed)) {
        state.speed = (uint16_t)CLAMP(speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    }
    if (read_int32(live->brightness, &brightness)) {
        led_pattern_set_brightness((uint8_t)CLAMP(brightness, 0, 100));
    }
    if (read_bool(&led_pattern_cs_adv_blink, &adv_blink)) {
        state.advertising_indicator = adv_blink;
    }

    LOG_INF("led: applying %s -> pattern %u speed %u indicator %d", live->name, state.pattern,
            state.speed, (int)state.advertising_indicator);

    led_pattern_set_state(&state);
}

static int led_pattern_settings_event_cb(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    /*
     * Every subscribed event means the same thing here -- what the LED should
     * be showing may have moved -- and re-reading five scalars is cheaper
     * than working out which one did.
     */
    led_pattern_apply_settings();

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
