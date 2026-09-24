/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_led_battery_adc_offset

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>

#include <zmk-led-patterns/battery_adc_offset.h>
#include <zmk-led-patterns/led_pattern.h>

struct led_battery_config {
    const struct device *source;
    uint16_t full_duty_offset_mv;
};

struct led_battery_data {
    uint16_t sampled_offset_mv;
};

static int led_battery_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    const struct led_battery_config *config = dev->config;
    struct led_battery_data *data = dev->data;
    const uint8_t duty = led_pattern_applied_brightness();
    const int rc = sensor_sample_fetch_chan(config->source, chan);

    if (rc == 0 && (chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_VOLTAGE)) {
        data->sampled_offset_mv =
            led_pattern_battery_offset_mv(duty, config->full_duty_offset_mv);
    }
    return rc;
}

static int led_battery_channel_get(const struct device *dev, enum sensor_channel chan,
                                   struct sensor_value *val) {
    const struct led_battery_config *config = dev->config;
    const struct led_battery_data *data = dev->data;
    const int rc = sensor_channel_get(config->source, chan, val);

    if (rc != 0 || chan != SENSOR_CHAN_VOLTAGE) {
        return rc;
    }

    const int64_t corrected_uv = (int64_t)val->val1 * 1000000 + val->val2 +
                                 (int64_t)data->sampled_offset_mv * 1000;
    val->val1 = (int32_t)(corrected_uv / 1000000);
    val->val2 = (int32_t)(corrected_uv % 1000000);
    return 0;
}

static int led_battery_init(const struct device *dev) {
    const struct led_battery_config *config = dev->config;
    return device_is_ready(config->source) ? 0 : -ENODEV;
}

static DEVICE_API(sensor, led_battery_api) = {
    .sample_fetch = led_battery_sample_fetch,
    .channel_get = led_battery_channel_get,
};

#define LED_BATTERY_DEFINE(inst)                                                                    \
    BUILD_ASSERT(DT_INST_PROP(inst, full_duty_offset_mv) <= 1000,                                   \
                 "LED battery offset must be at most 1000 mV");                                   \
    static const struct led_battery_config led_battery_config_##inst = {                             \
        .source = DEVICE_DT_GET(DT_INST_PHANDLE(inst, source_sensor)),                              \
        .full_duty_offset_mv = DT_INST_PROP(inst, full_duty_offset_mv),                             \
    };                                                                                              \
    static struct led_battery_data led_battery_data_##inst;                                         \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, led_battery_init, NULL, &led_battery_data_##inst,             \
                                 &led_battery_config_##inst, POST_KERNEL,                           \
                                 CONFIG_SENSOR_INIT_PRIORITY, &led_battery_api);

DT_INST_FOREACH_STATUS_OKAY(LED_BATTERY_DEFINE)
