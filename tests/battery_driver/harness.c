/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <zmk-led-patterns/battery_adc_offset.h>

enum sensor_channel { SENSOR_CHAN_ALL, SENSOR_CHAN_VOLTAGE, SENSOR_CHAN_OTHER };
struct sensor_value { int32_t val1; int32_t val2; };
struct device { const void *config; void *data; };
struct led_battery_config { const struct device *source; uint16_t full_duty_offset_mv; };
struct led_battery_data { uint16_t sampled_offset_mv; };

static int fetch_result;
static int get_result;
static bool source_ready;
static uint8_t duty;
static struct sensor_value source_voltage;

static uint8_t led_pattern_applied_brightness(void) { return duty; }
static int sensor_sample_fetch_chan(const struct device *dev, enum sensor_channel chan) {
    (void)dev;
    (void)chan;
    return fetch_result;
}
static int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
                              struct sensor_value *value) {
    (void)dev;
    (void)chan;
    *value = source_voltage;
    return get_result;
}
static bool device_is_ready(const struct device *dev) {
    (void)dev;
    return source_ready;
}

/* DRIVER_FUNCTIONS */

int main(void) {
    struct device source = {0};
    struct led_battery_config config = {.source = &source, .full_duty_offset_mv = 50};
    struct led_battery_data data = {0};
    struct device proxy = {.config = &config, .data = &data};
    struct sensor_value value;

    source_ready = false;
    assert(led_battery_init(&proxy) == -ENODEV);
    source_ready = true;
    assert(led_battery_init(&proxy) == 0);

    duty = 100;
    fetch_result = -EIO;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_VOLTAGE) == -EIO);
    assert(data.sampled_offset_mv == 0);

    fetch_result = 0;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_OTHER) == 0);
    assert(data.sampled_offset_mv == 0);
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_VOLTAGE) == 0);
    assert(data.sampled_offset_mv == 50);

    get_result = -EIO;
    source_voltage = (struct sensor_value){.val1 = 3, .val2 = 975000};
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_VOLTAGE, &value) == -EIO);
    get_result = 0;
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_OTHER, &value) == 0);
    assert(value.val1 == 3 && value.val2 == 975000);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_VOLTAGE, &value) == 0);
    assert(value.val1 == 4 && value.val2 == 25000);

    duty = 0;
    assert(led_battery_sample_fetch(&proxy, SENSOR_CHAN_ALL) == 0);
    assert(data.sampled_offset_mv == 0);
    assert(led_battery_channel_get(&proxy, SENSOR_CHAN_VOLTAGE, &value) == 0);
    assert(value.val1 == 3 && value.val2 == 975000);
    puts("LED battery sensor: PASS");
    return 0;
}
