/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern.c built as a split peripheral: it shows what the
 * central mirrors to it, answers each mirror, and trusts the central's word on
 * activity while the two are linked.
 */
#define CONFIG_ZMK_SPLIT 1
#include "led_stubs.h"

struct mirror_event { zmk_event_t header; struct zmk_led_pattern_mirror mirror; };
struct zmk_split_peripheral_status_changed { zmk_event_t header; bool connected; };

static int acks;
static struct zmk_led_pattern_ack last_ack;

static int raise_zmk_led_pattern_ack(struct zmk_led_pattern_ack event) {
    last_ack = event;
    acks++;
    return 0;
}
static const struct zmk_led_pattern_mirror *as_zmk_led_pattern_mirror(const zmk_event_t *eh) {
    return eh->kind == EVENT_MIRROR ? &((const struct mirror_event *)(const void *)eh)->mirror
                                    : NULL;
}
static const struct zmk_split_peripheral_status_changed *
as_zmk_split_peripheral_status_changed(const zmk_event_t *eh) {
    return eh->kind == EVENT_PERIPHERAL_STATUS ? (const void *)eh : NULL;
}

static bool central_active;
static void ack_work_handler(struct k_work *work);
static struct k_work ack_work = {.handler = ack_work_handler};

/* DRIVER_FUNCTIONS */

static void mirror(uint8_t source, uint8_t pattern, uint8_t brightness, uint16_t speed,
                   uint32_t elapsed_ms, bool central_is_active) {
    const struct mirror_event event = {
        {EVENT_MIRROR},
        {.source = source, .pattern = pattern, .brightness = brightness, .speed = speed,
         .elapsed_ms = elapsed_ms, .advertising_blink = false, .idle_off = true,
         .central_active = central_is_active}};
    assert(led_pattern_mirror_listener(&event.header) == ZMK_EV_EVENT_BUBBLE);
}

static void link(bool connected) {
    const struct zmk_split_peripheral_status_changed event = {{EVENT_PERIPHERAL_STATUS},
                                                              connected};
    assert(led_pattern_peripheral_status_listener(&event.header) == ZMK_EV_EVENT_BUBBLE);
}

static void test_boot_and_link(void) {
    reported_activity = ZMK_ACTIVITY_IDLE;
    idle_off = false;
    assert(behavior_led_pattern_init(NULL) == 0 && !animation_suspended);
    idle_off = true;
    assert(behavior_led_pattern_init(NULL) == 0 && animation_suspended);
    reported_activity = ZMK_ACTIVITY_ACTIVE;
    now_ms = 4321;
    assert(behavior_led_pattern_init(NULL) == 0 && !animation_suspended);
    assert(controller_ready && pattern_work.scheduled && split_stage_started_at == 4321);

    /* Waiting blink, then Steady once linked; a peripheral never mirrors. */
    assert(strstr(masking_indicator(), "split") != NULL);
    now_ms = split_stage_started_at;
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 500);
    now_ms += 500;
    assert(run_pattern() && led_level == 0 && pattern_work.delay_ms == 500);
    now_ms = split_stage_started_at + 499;
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 1);
    brightness_percent = 1; /* the status ignores the brightness */
    assert(run_pattern() && led_level == 100);
    brightness_percent = 100;
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
    assert(led_pattern_peripheral_status_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    now_ms += 1000;
    pattern_work.scheduled = false;
    link(true);
    assert(split_stage == SPLIT_STAGE_LINKED && split_stage_started_at == now_ms);
    assert(pattern_work.scheduled); /* redrawn at once */
    assert(run_pattern() && led_level == 100 && !pattern_work.scheduled);
    link(true); /* the same stage again */
}

static void test_mirror(void) {
    /* Its own relay send, or another event, is not a mirror. */
    assert(led_pattern_mirror_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    mirror(ZMK_RELAY_EVENT_SOURCE_SELF, LED_PATTERN_BLINK, 50, 100, 0, true);
    assert(split_stage == SPLIT_STAGE_LINKED && acks == 0);

    /* A mirror syncs the pair, takes the central's phase and is answered. */
    now_ms = 50000;
    mirror(0, LED_PATTERN_BLINK, 50, 100, 250, true);
    assert(split_stage == SPLIT_STAGE_SYNCED && active_pattern == LED_PATTERN_BLINK);
    assert(brightness_percent == 50 && speed_percent == 100 && central_active);
    assert(pattern_started_at == 50000 - 250 && ack_work.pending);
    ack_work.pending = false;
    ack_work.handler(&ack_work);
    assert(acks == 1 && last_ack.source == ZMK_RELAY_EVENT_SOURCE_SELF &&
           last_ack.pattern == LED_PATTERN_BLINK && last_ack.elapsed_ms == 250);
    assert(strcmp(masking_indicator(), "") == 0);
    assert(run_pattern() && led_level == 50 && pattern_work.delay_ms == 250);

    const struct mirror_event flags = {
        {EVENT_MIRROR},
        {.source = 0, .pattern = LED_PATTERN_BLINK, .brightness = 50, .speed = 100,
         .advertising_blink = true, .idle_off = false, .central_active = true}};
    pattern_work.scheduled = false;
    assert(led_pattern_mirror_listener(&flags.header) == ZMK_EV_EVENT_BUBBLE);
    assert(advertising_blink && !idle_off && pattern_work.scheduled);
    /* The advertising blink holds for its own fixed on-time. */
    pattern_started_at = now_ms;
    assert(run_pattern() && pattern_work.delay_ms == ADVERTISING_BLINK_ON_MS);
    advertising_blink = false;
    idle_off = true;

    /* Out-of-range values from the wire are brought into range. */
    mirror(0, 200, 250, 1, 0, true);
    assert(active_pattern == LED_PATTERN_STEADY && brightness_percent == 100);
    assert(speed_percent == LED_PATTERN_SPEED_MIN);
    mirror(0, LED_PATTERN_BREATHE, 30, 9999, 0, true);
    assert(speed_percent == LED_PATTERN_SPEED_MAX && active_pattern == LED_PATTERN_BREATHE);
    mirror(0, LED_PATTERN_COUNT, 30, 9999, 0, true);
    assert(active_pattern == LED_PATTERN_STEADY);

    /* The advertising blink it was told about masks the pattern. */
    advertising_blink = true;
    assert(strstr(masking_indicator(), "advertising") != NULL);
    assert(run_pattern() && led_level == ADVERTISING_BLINK_BRIGHTNESS * 30 / 100);
    advertising_blink = false;
    brightness_percent = 0;
    run_pattern();
    assert(led_level == 0 && !pattern_work.scheduled);
    brightness_percent = 1;
    active_pattern = LED_PATTERN_BLINK;
    pattern_started_at = now_ms;
    run_pattern();
    assert(led_level == 1 && pattern_work.scheduled);
    brightness_percent = 100;
    active_pattern = LED_PATTERN_SAWTOOTH;
    pattern_started_at = now_ms - 1799;
    run_pattern();
    assert(pattern_work.scheduled && pattern_work.delay_ms == 1);
    active_pattern = LED_PATTERN_STEADY;
    run_pattern();
    assert(led_level == 100 && !pattern_work.scheduled);
}

static void test_activity(void) {
    /* Either half's word keeps the LED lit. */
    activity_state = ZMK_ACTIVITY_IDLE;
    apply_activity();
    assert(!animation_suspended); /* the central says active */
    mirror(0, LED_PATTERN_STEADY, 100, 100, 0, false);
    assert(animation_suspended && !central_active);
    run_pattern();
    assert(led_level == 0);
    activity_state = ZMK_ACTIVITY_ACTIVE;
    now_ms += 10;
    pattern_work.scheduled = false;
    apply_activity();
    assert(!animation_suspended && pattern_started_at == now_ms && pattern_work.scheduled);

    /* With idle-off cleared nothing darkens it, whoever is idle. */
    activity_state = ZMK_ACTIVITY_IDLE;
    central_active = false;
    idle_off = false;
    apply_activity();
    assert(!animation_suspended);
    idle_off = true;
    activity_state = ZMK_ACTIVITY_ACTIVE;

    /* Losing the link drops the central's word and goes back to waiting. */
    activity_state = ZMK_ACTIVITY_IDLE;
    mirror(0, LED_PATTERN_STEADY, 100, 100, 0, true);
    assert(!animation_suspended); /* kept lit by the central's word */
    link(false);
    assert(!central_active && split_stage == SPLIT_STAGE_WAITING);
    assert(animation_suspended); /* that word is gone with the link */
    activity_state = ZMK_ACTIVITY_ACTIVE;
}

static void test_power_off(void) {
    pattern_work.scheduled = true;
    led_level = 77;
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(!pattern_work.scheduled && led_level == 0);
    pattern_work.work.handler(&pattern_work.work);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0 && pattern_work.scheduled);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_TURN_OFF) == -ENOTSUP);
}

int main(void) {
    now_ms = 100;
    test_boot_and_link();
    test_mirror();
    test_activity();
    test_power_off();
    puts("led pattern (split peripheral): PASS");
    return 0;
}
