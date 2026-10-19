/* SPDX-License-Identifier: MIT
 * Controlled adapter interface for controller-only branch/curve checks.
 * The actual adapter is tested with real Zephyr in native/run_scheduled.sh.
 */
#include "led_sync_protocol.h"
struct led_scheduled_view {
    struct led_sync_values values;
    uint64_t elapsed_ms;
    int64_t status_started_at;
    enum led_sync_stage stage;
    uint32_t applied_sequence;
    bool normal;
};
static struct led_sync_values mock_desired = {1, 100, 100, LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE | LED_SYNC_INDICATOR};
static struct led_scheduled_view mock_view;
static int mock_starts, mock_activity, mock_endpoint, mock_pm;
static bool mock_state_changed, mock_brightness_changed;
static uint32_t mock_completed_sequence;
static bool mock_completed_ok;
static STUB struct k_work_q *led_scheduled_render_queue(void) { return &lowprio_queue; }
static STUB void led_scheduled_start(void (*redraw)(void)) { assert(redraw == refresh_pattern_output); mock_starts++; }
static STUB void led_scheduled_get_desired(struct led_sync_values *out) { *out = mock_desired; }
static STUB bool led_scheduled_set_brightness(uint8_t value) { mock_desired.brightness=value; return mock_brightness_changed; }
static STUB bool led_scheduled_set_state(const struct led_pattern_state *value) {
    mock_desired.pattern=value->pattern; mock_desired.speed=value->speed;
    mock_desired.flags=(value->idle_off ? LED_SYNC_IDLE_OFF : 0) |
        (value->advertising_indicator ? LED_SYNC_INDICATOR : 0);
    return mock_state_changed;
}
static STUB bool led_scheduled_set_presentation(const struct led_pattern_state *value, uint8_t brightness) {
    (void)led_scheduled_set_state(value);
    mock_desired.brightness=brightness;
    return mock_state_changed || mock_brightness_changed;
}
static STUB bool led_scheduled_request(enum led_pattern_field field, int32_t value) {
    ARG_UNUSED(field); ARG_UNUSED(value); return mock_state_changed;
}
static STUB void led_scheduled_activity(bool value) { mock_activity=value ? 1 : 0; }
static STUB void led_scheduled_endpoint(bool value) { mock_endpoint=value ? 1 : 0; }
static STUB void led_scheduled_pm(bool value) { if(value) assert(led_level==0); mock_pm+=value ? 1 : -1; }
static STUB void led_scheduled_pm_prepare(void) { assert(atomic_get(&powering_off)); }
static STUB void led_scheduled_render_begin(struct led_scheduled_view *out) { *out=mock_view; }
static STUB void led_scheduled_render_end(uint32_t sequence, bool ok) { mock_completed_sequence=sequence; mock_completed_ok=ok; }
static STUB void schedule_pattern_work(k_timeout_t delay);
static STUB void apply_request(enum led_pattern_field field, int32_t value);
static STUB void notify_state_changed(void);
static STUB struct pattern_sample split_status_sample(enum led_sync_stage stage, int64_t elapsed);
