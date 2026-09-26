/*
 * Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * A battery sensor in front of ZMK's zmk,battery-voltage-divider that adds
 * the LED's measured ADC sag to its voltage.
 *
 * The divider's resistor values stay the board's fixed calibration; this adds
 * only the part that depends on the LED duty at the moment of the sample.
 * ZMK's battery reporting reads the state of charge, which the divider works
 * out inside its own fetch from the uncorrected voltage, so correcting the
 * voltage alone would never reach it. This wrapper therefore reads the
 * divider's voltage, corrects it, and computes the percentage again the way
 * the divider does: over its mv-to-pct-thresholds on the DYA fork, or with
 * ZMK's fixed lithium-ion line where it has none. Everything that reads the chosen
 * zmk,battery - BAS, the split peripheral's level, a display - sees the
 * corrected value, and ZMK itself is unchanged.
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
    const int16_t *mv_to_pct_thresholds;
    size_t mv_to_pct_thresholds_size;
    uint16_t full_duty_offset_mv;
};

struct led_battery_data {
    uint16_t millivolts;
    uint8_t state_of_charge;
};

/* The channels the divider serves, and so the ones this serves. */
static bool led_battery_channel_supported(enum sensor_channel chan) {
    return chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_GAUGE_VOLTAGE ||
           chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE;
}

static int led_battery_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    const struct led_battery_config *config = dev->config;
    struct led_battery_data *data = dev->data;

    if (!led_battery_channel_supported(chan)) {
        return -ENOTSUP;
    }

    int rc = sensor_sample_fetch_chan(config->source, SENSOR_CHAN_GAUGE_VOLTAGE);

    if (rc != 0) {
        return rc;
    }

    /* Read after the fetch: the divider waits for its power switch to settle
     * and samples at the end, so this is the duty the ADC just saw. */
    const uint8_t duty = led_pattern_applied_brightness();
    struct sensor_value voltage;

    rc = sensor_channel_get(config->source, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
    if (rc != 0) {
        return rc;
    }

    const uint16_t mv = led_pattern_battery_corrected_mv(
        voltage.val1, voltage.val2,
        led_pattern_battery_offset_mv(duty, config->full_duty_offset_mv));

    data->millivolts = mv;
    data->state_of_charge = led_pattern_battery_mv_to_pct(mv, config->mv_to_pct_thresholds,
                                                          config->mv_to_pct_thresholds_size);
    return 0;
}

static int led_battery_channel_get(const struct device *dev, enum sensor_channel chan,
                                   struct sensor_value *val) {
    const struct led_battery_data *data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val->val1 = data->millivolts / 1000;
        val->val2 = (data->millivolts % 1000) * 1000;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int led_battery_init(const struct device *dev) {
    const struct led_battery_config *config = dev->config;
    return device_is_ready(config->source) ? 0 : -ENODEV;
}

static DEVICE_API(sensor, led_battery_api) = {
    .sample_fetch = led_battery_sample_fetch,
    .channel_get = led_battery_channel_get,
};

#define LED_BATTERY_SOURCE(inst) DT_INST_PHANDLE(inst, source_sensor)

/* The thresholds come from the source node, so the two map a voltage the same
 * way. The DYA fork's divider binding gives mv-to-pct-thresholds a default;
 * upstream's divider and nrf-vddh have none and use ZMK's fixed lithium-ion
 * line, which a size of zero selects. */
#define LED_BATTERY_HAS_THRESHOLDS(inst)                                                            \
    DT_NODE_HAS_PROP(LED_BATTERY_SOURCE(inst), mv_to_pct_thresholds)

#define LED_BATTERY_DEFINE(inst)                                                                    \
    BUILD_ASSERT(DT_INST_PROP(inst, full_duty_offset_mv) <= 1000,                                   \
                 "LED battery offset must be at most 1000 mV");                                   \
    COND_CODE_1(LED_BATTERY_HAS_THRESHOLDS(inst),                                                   \
                (static const int16_t led_battery_thresholds_##inst[] =                             \
                     DT_PROP(LED_BATTERY_SOURCE(inst), mv_to_pct_thresholds);),                     \
                ())                                                                                 \
    static const struct led_battery_config led_battery_config_##inst = {                             \
        .source = DEVICE_DT_GET(LED_BATTERY_SOURCE(inst)),                                          \
        .mv_to_pct_thresholds = COND_CODE_1(LED_BATTERY_HAS_THRESHOLDS(inst),                       \
                                            (led_battery_thresholds_##inst), (NULL)),               \
        .mv_to_pct_thresholds_size =                                                                \
            DT_PROP_LEN_OR(LED_BATTERY_SOURCE(inst), mv_to_pct_thresholds, 0),                      \
        .full_duty_offset_mv = DT_INST_PROP(inst, full_duty_offset_mv),                             \
    };                                                                                              \
    static struct led_battery_data led_battery_data_##inst;                                         \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, led_battery_init, NULL, &led_battery_data_##inst,             \
                                 &led_battery_config_##inst, POST_KERNEL,                           \
                                 CONFIG_SENSOR_INIT_PRIORITY, &led_battery_api);

DT_INST_FOREACH_STATUS_OKAY(LED_BATTERY_DEFINE)
