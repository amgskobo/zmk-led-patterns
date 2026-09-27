/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * ZMK's zmk,battery-voltage-divider with the hardware in front of it modelled,
 * for nrf52_bsim, which has no SAADC. See the binding for what is modelled.
 * The sample arithmetic from the pin voltage on, and the channels served,
 * follow app/module/drivers/sensor/battery/battery_voltage_divider.c and
 * battery_common.c.
 */

#define DT_DRV_COMPAT zmk_test_battery_divider

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <zmk-led-patterns/led_pattern.h>

LOG_MODULE_REGISTER(test_battery_divider, LOG_LEVEL_INF);

struct tbd_config {
    int32_t battery_mv;
    int32_t led_sag_mv;
    uint32_t actual_output_ohm;
    uint32_t actual_full_ohm;
    uint32_t output_ohm;
    uint32_t full_ohm;
    const int16_t *thresholds;
    size_t thresholds_size;
};

struct tbd_data {
    uint16_t millivolts;
    uint8_t state_of_charge;
    int last_logged;
};

/* battery_common.c's mv_to_pct_linear_interpolation, float and all, with its
 * lithium_ion_mv_to_pct for a sensor without thresholds, as upstream has. */
static uint8_t tbd_mv_to_pct(int16_t bat_mv, const int16_t *mv_thresholds, size_t size) {
    if (size == 0) {
        if (bat_mv >= 4200) {
            return 100;
        } else if (bat_mv <= 3450) {
            return 0;
        }
        return bat_mv * 2 / 15 - 459;
    }
    if (bat_mv < mv_thresholds[0]) {
        return 0;
    }
    if (bat_mv >= mv_thresholds[size - 1]) {
        return 100;
    }
    for (size_t i = 1; i < size; i++) {
        if (bat_mv < mv_thresholds[i]) {
            int low = mv_thresholds[i - 1];
            int high = mv_thresholds[i];
            float deno = ((bat_mv - low) + (i - 1) * (high - low)) * 100;
            return deno / (float)((high - low) * (size - 1));
        }
    }
    return 100;
}

static int tbd_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    const struct tbd_config *cfg = dev->config;
    struct tbd_data *data = dev->data;

    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    /* The battery sags under the LED's current as the ADC samples it. */
    const uint8_t duty = led_pattern_applied_brightness();
    const int64_t battery_uv = (int64_t)cfg->battery_mv * 1000 -
                               (int64_t)cfg->led_sag_mv * 1000 * duty / 100;
    const int64_t pin_uv = battery_uv * cfg->actual_output_ohm / cfg->actual_full_ohm;
    /* SAADC, 12 bits at gain 1/6 on the 0.6 V reference: 3.6 V full scale. */
    int32_t val = (int32_t)(pin_uv * 4096 / 3600000);

    /* From here on, bvd_sample_fetch: adc_raw_to_millivolts, then the divider. */
    val = (val * 600 * 6) >> 12;
    const uint16_t millivolts = val * (uint64_t)cfg->full_ohm / cfg->output_ohm;

    data->millivolts = millivolts;
    data->state_of_charge = tbd_mv_to_pct(millivolts, cfg->thresholds, cfg->thresholds_size);

    const int logged = duty << 16 | millivolts;
    if (logged != data->last_logged) {
        data->last_logged = logged;
        LOG_INF("divider: LED %u%%, battery %lld mV, pin %d mV, reads %u mV = %u%% uncorrected",
                duty, battery_uv / 1000, val, millivolts, data->state_of_charge);
    }
    return 0;
}

static int tbd_channel_get(const struct device *dev, enum sensor_channel chan,
                           struct sensor_value *val) {
    const struct tbd_data *data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val->val1 = data->millivolts / 1000;
        val->val2 = (data->millivolts % 1000) * 1000U;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int tbd_init(const struct device *dev) {
    struct tbd_data *data = dev->data;
    data->last_logged = -1;
    return 0;
}

static DEVICE_API(sensor, tbd_api) = {
    .sample_fetch = tbd_sample_fetch,
    .channel_get = tbd_channel_get,
};

#if DT_INST_NODE_HAS_PROP(0, mv_to_pct_thresholds)
static const int16_t tbd_thresholds[] = DT_INST_PROP(0, mv_to_pct_thresholds);
#define TBD_THRESHOLDS tbd_thresholds
#define TBD_THRESHOLDS_SIZE ARRAY_SIZE(tbd_thresholds)
#else
#define TBD_THRESHOLDS NULL
#define TBD_THRESHOLDS_SIZE 0
#endif

static const struct tbd_config tbd_config = {
    .battery_mv = DT_INST_PROP(0, battery_millivolts),
    .led_sag_mv = DT_INST_PROP(0, led_sag_mv),
    .actual_output_ohm = DT_INST_PROP(0, actual_output_ohms),
    .actual_full_ohm = DT_INST_PROP(0, actual_full_ohms),
    .output_ohm = DT_INST_PROP(0, output_ohms),
    .full_ohm = DT_INST_PROP(0, full_ohms),
    .thresholds = TBD_THRESHOLDS,
    .thresholds_size = TBD_THRESHOLDS_SIZE,
};
static struct tbd_data tbd_data;

SENSOR_DEVICE_DT_INST_DEFINE(0, tbd_init, NULL, &tbd_data, &tbd_config, POST_KERNEL,
                             CONFIG_SENSOR_INIT_PRIORITY, &tbd_api);
