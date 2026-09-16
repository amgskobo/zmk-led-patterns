/*
 * SPDX-License-Identifier: MIT
 *
 * &led_brightness: the brightness setting, as a behavior of its own.
 */

#define DT_DRV_COMPAT zmk_behavior_led_brightness

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
             "&led_brightness needs a zmk,behavior-led-pattern node as well");
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "declare exactly one zmk,behavior-led-brightness node");

/*
 * A step lands on a multiple of the step, so a value set from a client -- 95,
 * say -- rejoins the tens instead of carrying its offset along. Down goes all
 * the way to 0: that is the LED's low-power state, with the PWM output stopped
 * and nothing redrawn, and a key is the natural way to reach it.
 */
static int32_t step_brightness(int32_t current, bool up) {
    const int32_t step = LED_PATTERN_BRIGHTNESS_STEP;

    if (up) {
        return MIN(100, (current / step + 1) * step);
    }
    return MAX(0, ((current + step - 1) / step - 1) * step);
}

static int on_brightness_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (!led_pattern_ready()) {
        return -ENODEV;
    }

    int32_t brightness;

    if (binding->param1 <= 100) {
        brightness = (int32_t)binding->param1;
    } else if (binding->param1 == LED_BRIGHTNESS_UP || binding->param1 == LED_BRIGHTNESS_DOWN) {
        brightness = step_brightness(led_pattern_field_value(LED_PATTERN_FIELD_BRIGHTNESS),
                                     binding->param1 == LED_BRIGHTNESS_UP);
    } else {
        LOG_WRN("led: &led_brightness has no command %u", binding->param1);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    led_pattern_request(LED_PATTERN_FIELD_BRIGHTNESS, brightness);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_brightness_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* Without metadata a Studio client rejects every binding with a non-zero
 * parameter; with it, the steps are named and the range is a number input. */
static const struct behavior_parameter_value_metadata param1_values[] = {
    {
        .display_name = "Brightness down (-10%)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = LED_BRIGHTNESS_DOWN,
    },
    {
        .display_name = "Brightness up (+10%)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = LED_BRIGHTNESS_UP,
    },
    {
        .display_name = "Set brightness (percent)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = 100},
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

static const struct behavior_driver_api behavior_led_brightness_driver_api = {
    .binding_pressed = on_brightness_pressed,
    .binding_released = on_brightness_released,
    /* Central, like &led_pattern: the central owns the value and mirrors the
     * result to a peripheral as final state. */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_led_brightness_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
