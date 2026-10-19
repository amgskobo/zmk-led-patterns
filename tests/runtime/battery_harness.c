/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <zmk-led-patterns/battery_adc_offset.h>

enum sensor_channel {
    SENSOR_CHAN_VOLTAGE,
    SENSOR_CHAN_GAUGE_VOLTAGE,
    SENSOR_CHAN_GAUGE_STATE_OF_CHARGE,
    SENSOR_CHAN_ALL,
};
struct sensor_value { int32_t val1; int32_t val2; };
struct device { const void *config; void *data; };
struct led_battery_config {
    const struct device *source;
    const int16_t *mv_to_pct_thresholds;
    size_t mv_to_pct_thresholds_size;
    uint16_t full_duty_offset_mv;
};
struct led_battery_data { uint16_t millivolts; uint8_t state_of_charge; };

static int fetch_result;
static int get_result;
static bool source_ready;
static uint8_t duty;
static uint8_t duty_after_fetch;
static struct sensor_value source_voltage;

static uint8_t led_pattern_applied_brightness(void) { return duty; }

/* ZMK's zmk,battery-voltage-divider serves only the gauge channels and ALL;
 * a plain SENSOR_CHAN_VOLTAGE request is refused, as on the real board. */
static int sensor_sample_fetch_chan(const struct device *dev, enum sensor_channel chan) {
    (void)dev;
    if (chan == SENSOR_CHAN_VOLTAGE) {
        return -ENOTSUP;
    }
    /* The LED keeps animating while the divider settles and samples. */
    duty = duty_after_fetch;
    return fetch_result;
}
static int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
                              struct sensor_value *value) {
    (void)dev;
    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE) {
        return -ENOTSUP;
    }
    *value = source_voltage;
    return get_result;
}
static bool device_is_ready(const struct device *dev) {
    (void)dev;
    return source_ready;
}

/* DRIVER_FUNCTIONS */

int main(void) {
    static const int16_t thresholds[] = {3450, 4200};
    struct device source = {0};
    struct led_battery_config config = {
        .source = &source,
        .mv_to_pct_thresholds = thresholds,
        .mv_to_pct_thresholds_size = 2,
        .full_duty_offset_mv = 50,
    };
    struct led_battery_data data = {0};
    struct device proxy = {.config = &config, .data = &data};
    struct sensor_value value;

    source_ready = false;
    assert(led_battery_init(&proxy) == -ENODEV);
    source_ready = true;
    assert(led_battery_init(&proxy) == 0);

    source_voltage = (struct sensor_value){.val1 = 3, .val2 = 775000};

    /* Only the divider's channels are served. */
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_VOLTAGE) == -ENOTSUP);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_VOLTAGE, &value) == -ENOTSUP);

    /* A failed fetch or read leaves the last good sample alone. */
    data = (struct led_battery_data){.millivolts = 1234, .state_of_charge = 7};
    fetch_result = -EIO;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == -EIO);
    assert(data.millivolts == 1234 && data.state_of_charge == 7);
    fetch_result = 0;
    get_result = -EIO;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == -EIO);
    assert(data.millivolts == 1234 && data.state_of_charge == 7);
    get_result = 0;

    /* ZMK's default fetch mode: the percentage is recomputed from the
     * corrected voltage, with the duty seen after the sample. */
    duty = 0;
    duty_after_fetch = 100;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == 0);
    assert(data.millivolts == 3825);
    value = (struct sensor_value){.val1 = -1, .val2 = -1};
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &value) == 0);
    assert(value.val1 == 50 && value.val2 == 0);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_GAUGE_VOLTAGE, &value) == 0);
    assert(value.val1 == 3 && value.val2 == 825000);

    /* An unlit LED adds nothing: the divider's own reading. */
    duty_after_fetch = 0;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_VOLTAGE) == 0);
    assert(data.millivolts == 3775 && data.state_of_charge == 43);

    /* Half duty, fetched through ALL. */
    duty_after_fetch = 50;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_ALL) == 0);
    assert(data.millivolts == 3800 && data.state_of_charge == 46);

    /* Past the top threshold: full, and the volts split exactly at 1000 mV. */
    source_voltage = (struct sensor_value){.val1 = 4, .val2 = 945000};
    duty_after_fetch = 100;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == 0);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_GAUGE_VOLTAGE, &value) == 0);
    assert(value.val1 == 4 && value.val2 == 995000);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &value) == 0);
    assert(value.val1 == 100 && value.val2 == 0);

    /* A source without thresholds (upstream's divider): the lithium-ion line,
     * where 3825 mV is 51% rather than the interpolation's 50%. */
    config.mv_to_pct_thresholds = NULL;
    config.mv_to_pct_thresholds_size = 0;
    source_voltage = (struct sensor_value){.val1 = 3, .val2 = 775000};
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == 0);
    assert(data.millivolts == 3825 && data.state_of_charge == 51);

    puts("LED battery sensor: PASS");
    return 0;
}
