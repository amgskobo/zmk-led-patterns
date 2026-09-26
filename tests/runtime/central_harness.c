/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * behavior_led_pattern.c built as the central of a split: the connection
 * indicator, the connect-time state exchange and the mirror to the peripheral.
 */
#define CONFIG_ZMK_SPLIT 1
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#define CONFIG_ZMK_BLE 1
#include "led_stubs.h"

#define BT_CONN_ROLE_CENTRAL 0
#define BT_CONN_ROLE_PERIPHERAL 1
struct bt_conn { uint8_t role; bool has_info; };
struct bt_conn_info { uint8_t role; };
struct bt_conn_cb {
    void (*connected)(struct bt_conn *conn, uint8_t err);
    void (*disconnected)(struct bt_conn *conn, uint8_t reason);
};
struct ack_event { zmk_event_t header; struct zmk_led_pattern_ack ack; };

static int register_result;
static const struct bt_conn_cb *registered;
static int mirrors;
static struct zmk_led_pattern_mirror last_mirror;

static int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    if (!conn->has_info) {
        return -ENOTCONN;
    }
    info->role = conn->role;
    return 0;
}
static int bt_conn_cb_register(struct bt_conn_cb *cb) {
    registered = cb;
    return register_result;
}
static int raise_zmk_led_pattern_mirror(struct zmk_led_pattern_mirror event) {
    last_mirror = event;
    mirrors++;
    return 0;
}
static const struct zmk_led_pattern_ack *as_zmk_led_pattern_ack(const zmk_event_t *eh) {
    return eh->kind == EVENT_ACK ? &((const struct ack_event *)(const void *)eh)->ack : NULL;
}

static bool peripheral_link_up;
static uint8_t sync_attempts;
static void mirror_work_handler(struct k_work *work);
static struct k_work_delayable mirror_work = {.work = {.handler = mirror_work_handler}};
static void split_conn_connected(struct bt_conn *conn, uint8_t err);
static void split_conn_disconnected(struct bt_conn *conn, uint8_t reason);
static struct bt_conn_cb split_conn_callbacks = {
    .connected = split_conn_connected,
    .disconnected = split_conn_disconnected,
};

/* DRIVER_FUNCTIONS */

static struct bt_conn peripheral = {.role = BT_CONN_ROLE_CENTRAL, .has_info = true};
static struct bt_conn host = {.role = BT_CONN_ROLE_PERIPHERAL, .has_info = true};
static struct bt_conn gone = {.has_info = false};

static void run_mirror(void) {
    mirror_work.scheduled = false;
    mirror_work.work.handler(&mirror_work.work);
}

static void ack(uint8_t source) {
    const struct ack_event event = {{EVENT_ACK}, {source, active_pattern, 0}};
    assert(led_pattern_ack_listener(&event.header) == ZMK_EV_EVENT_BUBBLE);
}

static void test_boot(void) {
    /* Nothing is mirrored before init. */
    peripheral_link_up = true;
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
    assert(!mirror_work.scheduled);
    peripheral_link_up = false;

    /* Init takes the activity state as it finds it. */
    reported_activity = ZMK_ACTIVITY_IDLE;
    idle_off = false;
    assert(behavior_led_pattern_init(NULL) == 0 && !animation_suspended);
    idle_off = true;
    assert(behavior_led_pattern_init(NULL) == 0 && animation_suspended);
    reported_activity = ZMK_ACTIVITY_ACTIVE;

    /* A failed callback registration is reported, not fatal. */
    register_result = -ENOMEM;
    assert(behavior_led_pattern_init(NULL) == 0 && errors == 1);
    register_result = 0;
    now_ms = 4321;
    assert(behavior_led_pattern_init(NULL) == 0 && registered == &split_conn_callbacks);
    assert(split_stage_started_at == 4321);

    /* Both halves start on the waiting blink, at full brightness and fixed
     * rate whatever the settings say, and name it as the mask. */
    brightness_percent = 0;
    now_ms = split_stage_started_at;
    assert(split_status_showing() && strstr(masking_indicator(), "split") != NULL);
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 500);
    now_ms += 500;
    assert(run_pattern() && led_level == 0 && pattern_work.delay_ms == 500);
    now_ms = split_stage_started_at + 499;
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 1);
    brightness_percent = 100;

    /* No link, or not ready: no mirror is scheduled. */
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
    assert(!mirror_work.scheduled);
}

static void test_link(void) {
    /* Host links, links without info and failed connects are not the peer. */
    split_conn_connected(&host, 0);
    split_conn_connected(&gone, 0);
    split_conn_connected(&peripheral, 13);
    assert(!peripheral_link_up && split_stage == SPLIT_STAGE_WAITING && warnings == 1);
    split_conn_disconnected(&host, 0);
    assert(split_stage == SPLIT_STAGE_WAITING);

    /* The peer links: Steady, which schedules nothing, and the exchange starts
     * after the subscription delay. */
    now_ms += 1000;
    split_conn_connected(&peripheral, 0);
    assert(peripheral_link_up && split_stage == SPLIT_STAGE_LINKED);
    assert(split_stage_started_at == now_ms);
    assert(mirror_work.scheduled && mirror_work.delay_ms == SPLIT_SYNC_START_DELAY_MS);
    assert(run_pattern() && led_level == 100 && !pattern_work.scheduled);

    /* Retries until the attempts run out, then leaves the LED on Steady. */
    for (int attempt = 1; attempt <= SPLIT_SYNC_MAX_ATTEMPTS; attempt++) {
        run_mirror();
        assert(mirrors == attempt && last_mirror.source == ZMK_RELAY_EVENT_SOURCE_SELF);
        assert(mirror_work.scheduled == (attempt < SPLIT_SYNC_MAX_ATTEMPTS));
    }
    assert(split_stage == SPLIT_STAGE_LINKED);

    /* A reconnect starts over; an answer ends the exchange and releases the
     * LED to the pattern. A locally raised ack or another event is ignored. */
    split_conn_disconnected(&peripheral, 8);
    assert(!peripheral_link_up && split_stage == SPLIT_STAGE_WAITING);
    run_mirror();
    assert(mirrors == SPLIT_SYNC_MAX_ATTEMPTS);
    split_conn_connected(&peripheral, 0);
    assert(mirror_work.scheduled && sync_attempts == 0);
    split_conn_disconnected(&peripheral, 8);
    assert(!mirror_work.scheduled);
    split_conn_connected(&peripheral, 0);
    run_mirror();
    assert(led_pattern_ack_listener(&other_event) == ZMK_EV_EVENT_BUBBLE);
    ack(ZMK_RELAY_EVENT_SOURCE_SELF);
    assert(split_stage == SPLIT_STAGE_LINKED);
    ack(0);
    assert(split_stage == SPLIT_STAGE_SYNCED && !mirror_work.scheduled && sync_attempts == 0);
    assert(!split_status_showing());
    /* Every mirror is acked; once synced an ack changes nothing. */
    ack(0);
    assert(split_stage == SPLIT_STAGE_SYNCED);
}

static void test_synced(void) {
    /* A state change sends one mirror, with no retry. */
    set_split_stage(SPLIT_STAGE_SYNCED); /* unchanged: nothing to do */
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
    assert(mirror_work.scheduled && mirror_work.delay_ms == MIRROR_DEBOUNCE_MS);
    const int sent = mirrors;
    activity_state = ZMK_ACTIVITY_IDLE;
    run_mirror();
    assert(mirrors == sent + 1 && !mirror_work.scheduled && !last_mirror.central_active);
    activity_state = ZMK_ACTIVITY_ACTIVE;

    /* The pattern itself, as in a standalone build. */
    advertising_blink = true;
    assert(strstr(masking_indicator(), "advertising") != NULL);
    assert(run_pattern() && pattern_work.delay_ms > 0);
    advertising_blink = false;
    assert(strcmp(masking_indicator(), "") == 0);
    active_pattern = LED_PATTERN_BLINK;
    pattern_started_at = now_ms;
    assert(run_pattern() && led_level == 100 && pattern_work.delay_ms == 500);
    active_pattern = LED_PATTERN_STEADY;
    assert(run_pattern() && !pattern_work.scheduled);
    brightness_percent = 0;
    run_pattern();
    assert(led_level == 0 && !pattern_work.scheduled);
    brightness_percent = 1;
    active_pattern = LED_PATTERN_BLINK;
    pattern_started_at = now_ms;
    run_pattern();
    assert(led_level == 1 && pattern_work.scheduled);
    active_pattern = LED_PATTERN_SAWTOOTH;
    pattern_started_at = now_ms - 1799;
    brightness_percent = 100;
    run_pattern();
    assert(pattern_work.scheduled && pattern_work.delay_ms == 1);
    active_pattern = LED_PATTERN_STEADY;
    run_pattern();
    assert(led_level == 100);
    animation_suspended = true;
    run_pattern();
    assert(led_level == 0 && !pattern_work.scheduled);
    animation_suspended = false;
}

static void test_power_off(void) {
    /* Power-off stops the mirror as well as the pattern, and mirrors nothing. */
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
    pattern_work.scheduled = true;
    led_level = 77;
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(!mirror_work.scheduled && !pattern_work.scheduled && led_level == 0);
    const int sent = mirrors;
    run_mirror();
    pattern_work.work.handler(&pattern_work.work);
    assert(mirrors == sent);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND) == 0);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0 && pattern_work.scheduled);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME) == 0);
    assert(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_TURN_OFF) == -ENOTSUP);
}

int main(void) {
    now_ms = 100;
    test_boot();
    test_link();
    test_synced();
    test_power_off();
    puts("led pattern (split central): PASS");
    return 0;
}
