/* SPDX-License-Identifier: MIT */
#pragma once

#include "led_sync_protocol.h"
#include <zephyr/kernel.h>
#include <zmk-led-patterns/led_pattern.h>

struct led_scheduled_view {
    struct led_sync_values values;
    uint64_t elapsed_ms;
    int64_t status_started_at;
    enum led_sync_stage stage;
    uint32_t applied_sequence;
    bool normal;
};
struct k_work_q *led_scheduled_render_queue(void);
void led_scheduled_start(void (*redraw)(void));
void led_scheduled_get_desired(struct led_sync_values *out);
bool led_scheduled_set_brightness(uint8_t brightness);
bool led_scheduled_set_state(const struct led_pattern_state *state);
bool led_scheduled_set_presentation(const struct led_pattern_state *state, uint8_t brightness);
bool led_scheduled_request(enum led_pattern_field field, int32_t value);
void led_scheduled_activity(bool active);
void led_scheduled_endpoint(bool advertising);
void led_scheduled_pm_prepare(void);
void led_scheduled_pm(bool suspended);
void led_scheduled_render_begin(struct led_scheduled_view *out);
void led_scheduled_render_end(uint32_t applied_sequence, bool succeeded);
