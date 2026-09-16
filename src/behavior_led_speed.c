/*
 * SPDX-License-Identifier: MIT
 *
 * &led_speed: the speed setting, as a behavior of its own.
 */

#define DT_DRV_COMPAT zmk_behavior_led_speed

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk-led-patterns/led_pattern.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* The controller, and its init, belong to &led_pattern's node, so that node has
 * to exist for this one to have anything to drive. */
BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_led_pattern),
             "&led_speed needs a zmk,behavior-led-pattern node as well");
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "declare exactly one zmk,behavior-led-speed node");

/*
 * The same shape as a brightness step: it lands on a multiple of the step, so a
 * value set from a client -- 95, say -- rejoins the tens, and the published
 * bounds clamp it at either end.
 */
static int32_t step_speed(int32_t current, bool up) {
    const int32_t step = LED_PATTERN_SPEED_STEP;

    if (up) {
        return MIN(LED_PATTERN_SPEED_MAX, (current / step + 1) * step);
    }
    return MAX(LED_PATTERN_SPEED_MIN, ((current + step - 1) / step - 1) * step);
}

/* The parameter is the percentage itself, in the units the setting uses, or one
 * of the named commands outside that range. */
static int on_speed_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (!led_pattern_ready()) {
        return -ENODEV;
    }

    const uint32_t param = binding->param1;
    int32_t speed;

    if (param >= LED_PATTERN_SPEED_MIN && param <= LED_PATTERN_SPEED_MAX) {
        speed = (int32_t)param;
    } else {
        switch (param) {
        case LED_SPEED_DOWN:
        case LED_SPEED_UP:
            speed = step_speed(led_pattern_field_value(LED_PATTERN_FIELD_SPEED),
                               param == LED_SPEED_UP);
            break;
        case LED_SPEED_MIN:
            speed = LED_PATTERN_SPEED_MIN;
            break;
        case LED_SPEED_DEFAULT:
            speed = LED_PATTERN_SPEED_NOMINAL;
            break;
        case LED_SPEED_MAX:
            speed = LED_PATTERN_SPEED_MAX;
            break;
        default:
            LOG_WRN("led: &led_speed has no command %u", param);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    led_pattern_request(LED_PATTERN_FIELD_SPEED, speed);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_speed_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

#define LED_SPEED_NAMED_VALUE(name, param)                                                         \
    {.display_name = name, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = param}

/* Without metadata a Studio client rejects every binding with a non-zero
 * parameter; with it, the commands are named and the percentage is a bounded
 * number input. */
static const struct behavior_parameter_value_metadata param1_values[] = {
    LED_SPEED_NAMED_VALUE("Speed down (-10%)", LED_SPEED_DOWN),
    LED_SPEED_NAMED_VALUE("Speed up (+10%)", LED_SPEED_UP),
    LED_SPEED_NAMED_VALUE("Minimum speed (10%)", LED_SPEED_MIN),
    LED_SPEED_NAMED_VALUE("Default speed (100%)", LED_SPEED_DEFAULT),
    LED_SPEED_NAMED_VALUE("Maximum speed (400%)", LED_SPEED_MAX),
    {
        .display_name = "Set speed (percent)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = LED_PATTERN_SPEED_MIN, .max = LED_PATTERN_SPEED_MAX},
    },
};

static const struct behavior_parameter_metadata_set metadata_set = {
    .param1_values = param1_values,
    .param1_values_len = ARRAY_SIZE(param1_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_led_speed_driver_api = {
    .binding_pressed = on_speed_pressed,
    .binding_released = on_speed_released,
    /* Central, like &led_pattern: the central owns the value and mirrors the
     * result to a peripheral as final state. */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_led_speed_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
