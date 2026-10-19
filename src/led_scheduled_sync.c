/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 * Kernel adapter: metadata is short-held, PWM and potentially blocking relay
 * sends never run under its mutex. Rendering and reservation commits share one
 * queue; transport work uses ZMK's separate low-priority queue.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zmk/event_manager.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zephyr/bluetooth/conn.h>
#include <zmk/split/central.h>
#else
#include <zmk/events/split_peripheral_status_changed.h>
#endif
#include "led_scheduled_sync.h"
#include "led_sync_wire.h"

LOG_MODULE_REGISTER(zmk_led_scheduled_sync, CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS == 1,
             "LED scheduled synchronization currently supports exactly one split peer");
#endif
BUILD_ASSERT(LED_SYNC_WIRE_SIZE <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "LED scheduled packet exceeds selected relay capacity");

/* Existing low-priority policy, separate stack. No input/HID priority increase. */
K_THREAD_STACK_DEFINE(led_render_stack, 1536);
static struct k_work_q render_queue;
static K_MUTEX_DEFINE(sync_mutex);
static struct led_sync_engine engine;
static struct led_sync_values desired = {LED_PATTERN_BREATHE, 100, 100,
                                         LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE | LED_SYNC_INDICATOR};
static bool started, link_up, local_active = true;
static void (*redraw_callback)(void);
static enum led_sync_stage reported_stage;
static int64_t status_started_at;

/* Same event safely has a directional sender and a receive descriptor: the
 * pinned DYA macros give these distinct symbols and gate sends on sourceSELF. */
/* source must be a direct field for the relay macros. */
struct zmk_led_scheduled_wire {
    uint8_t source;
    uint8_t bytes[LED_SYNC_WIRE_SIZE - 1];
} __packed;
BUILD_ASSERT(sizeof(struct zmk_led_scheduled_wire) == LED_SYNC_WIRE_SIZE,
             "LED relay event must include source in its56-byte total");
ZMK_EVENT_DECLARE(zmk_led_scheduled_wire);
ZMK_EVENT_IMPL(zmk_led_scheduled_wire);
ZMK_RELAY_EVENT_HANDLE(zmk_led_scheduled_wire, ls4, source);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_led_scheduled_wire, ls4, source);
#else
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_led_scheduled_wire, ls4, source);
#endif

static void tx_handler(struct k_work *work);
static void apply_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_work, tx_handler);
static K_WORK_DELAYABLE_DEFINE(apply_work, apply_handler);

static void schedule_locked(uint64_t now) {
    if (!started || engine.suspended)
        return;
    uint64_t tx = led_sync_next_tx(&engine, now);
    uint64_t apply = led_sync_next_render(&engine, now);
    /* Park an obsolete timeout without canceling a running relay call. Full
     * cancellation would reject a new request until that call exits. K_FOREVER
     * retains the delayable descriptor but registers no timeout; a previously
     * queued/running invocation remains harmless and new work can reschedule. */
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &tx_work,
                                tx == LED_SYNC_NEVER ? K_FOREVER :
                                K_MSEC(tx > now ? tx - now : 0));
    if (apply != LED_SYNC_NEVER)
        k_work_reschedule_for_queue(&render_queue, &apply_work,
                                    K_MSEC(apply > now ? apply - now : 0));
}
static void report_stage_locked(uint64_t now) {
    if (engine.stage == reported_stage)
        return;
    if ((reported_stage == LED_SYNC_RUNNING_UNCONFIRMED || reported_stage == LED_SYNC_SYNCED) &&
        (engine.stage == LED_SYNC_RUNNING_UNCONFIRMED || engine.stage == LED_SYNC_SYNCED)) {
        LOG_DBG("led: scheduled split confirmation %d -> %d", reported_stage, engine.stage);
    } else {
        LOG_INF("led: scheduled split stage %d -> %d", reported_stage, engine.stage);
    }
    reported_stage = engine.stage;
    status_started_at = now;
}
static void tx_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct led_sync_packet packet;
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool send = led_sync_send(&engine, k_uptime_get(), &packet);
    schedule_locked(k_uptime_get());
    k_mutex_unlock(&sync_mutex);
    if (send) {
        struct zmk_led_scheduled_wire event;
        if (!led_sync_wire_encode((uint8_t *)&event, sizeof(event), &packet))
            return;
        /* Measured RTT includes residence between this capture/raise and the
         * listener-entry captures, as well as core queues/radio delivery. */
        raise_zmk_led_scheduled_wire(event);
    }
}
static void apply_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (redraw_callback)
        redraw_callback();
}
static int receive_listener(const zmk_event_t *eh) {
    const uint64_t received_at = k_uptime_get();
    const struct zmk_led_scheduled_wire *event = as_zmk_led_scheduled_wire(eh);
    if (!event || event->source != 1)
        return ZMK_EV_EVENT_BUBBLE;
    struct led_sync_packet packet;
    if (!led_sync_wire_decode(&packet, (const uint8_t *)event, sizeof(*event)))
        return ZMK_EV_EVENT_BUBBLE;
    k_mutex_lock(&sync_mutex, K_FOREVER);
    if (started) {
        bool accepted = led_sync_receive(&engine, &packet, received_at);
        if (accepted && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
            /* Accepted future desired state is immediately observable locally;
             * it never feeds a registry write or a desired-state event. */
            desired = led_sync_wire_values(&engine.pending.wire);
        }
        schedule_locked(k_uptime_get());
    }
    k_mutex_unlock(&sync_mutex);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(led_scheduled_sync, receive_listener);
ZMK_SUBSCRIPTION(led_scheduled_sync, zmk_led_scheduled_wire);

static void link_changed(bool up) {
    uint32_t nonce = sys_rand32_get();
    k_mutex_lock(&sync_mutex, K_FOREVER);
    link_up = up;
    if (started) {
        led_sync_link(&engine, up, k_uptime_get(), nonce);
        schedule_locked(k_uptime_get());
    }
    bool redraw = started && !engine.suspended;
    k_mutex_unlock(&sync_mutex);
    /* Asynchronous redraw only; a caller may be the Bluetooth RX thread. */
    if (redraw)
        k_work_reschedule_for_queue(&render_queue, &apply_work, K_NO_WAIT);
}
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool peer_connection(struct bt_conn *conn) {
    struct bt_conn_info info;
    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}
static void connected(struct bt_conn *conn, uint8_t err) {
    if (!err && peer_connection(conn))
        link_changed(true);
}
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (peer_connection(conn))
        link_changed(false);
}
static struct bt_conn_cb connection_callbacks = {.connected = connected,
                                                 .disconnected = disconnected};
#else
static int peripheral_status_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *event =
        as_zmk_split_peripheral_status_changed(eh);
    if (event)
        link_changed(event->connected);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(led_scheduled_link, peripheral_status_listener);
ZMK_SUBSCRIPTION(led_scheduled_link, zmk_split_peripheral_status_changed);
#endif

struct k_work_q *led_scheduled_render_queue(void) { return &render_queue; }
static struct led_sync_values effective_desired(void) {
    struct led_sync_values effective = desired;
    if (!(effective.flags & LED_SYNC_IDLE_OFF))
        effective.flags |= LED_SYNC_ACTIVE;
    return effective;
}
void led_scheduled_start(void (*redraw)(void)) {
    const uint32_t nonce = sys_rand32_get();
    k_mutex_lock(&sync_mutex, K_FOREVER);
    redraw_callback = redraw;
    led_sync_init(&engine, IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL), nonce,
                  2U * MAX(1U, DIV_ROUND_UP(1000U, CONFIG_SYS_CLOCK_TICKS_PER_SEC)),
                  effective_desired());
    k_work_queue_start(&render_queue, led_render_stack, K_THREAD_STACK_SIZEOF(led_render_stack),
                       CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY, NULL);
    started = true;
    reported_stage = LED_SYNC_WAITING;
    status_started_at = k_uptime_get();
    if (link_up)
        led_sync_link(&engine, true, k_uptime_get(), nonce);
    schedule_locked(k_uptime_get());
    k_mutex_unlock(&sync_mutex);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    int err = bt_conn_cb_register(&connection_callbacks);
    if (err < 0)
        LOG_ERR("led: split callbacks registration failed (%d)", err);
#endif
}
void led_scheduled_get_desired(struct led_sync_values *out) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    *out = desired;
    k_mutex_unlock(&sync_mutex);
}
static void publish_locked(bool reset) {
    if (!started)
        return;
    /* ACTIVE cannot affect normal output when idle-off is disabled. Keep the
     * actual desired activity locally, but one canonical wire value avoids
     * unnecessary transactions and still captures it when idle-off is enabled. */
    led_sync_desire(&engine, effective_desired(), reset);
    led_sync_reserve(&engine, k_uptime_get());
    schedule_locked(k_uptime_get());
}
bool led_scheduled_set_brightness(uint8_t brightness) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint8_t next = MIN(brightness, 100);
    bool changed = desired.brightness != next;
    desired.brightness = next;
    publish_locked(false);
    k_mutex_unlock(&sync_mutex);
    return changed;
}
static bool set_state_locked(const struct led_pattern_state *state, bool replace_brightness,
                             uint8_t brightness) {
    uint8_t pattern = state->pattern < LED_PATTERN_COUNT ? state->pattern : LED_PATTERN_STEADY;
    uint16_t speed = CLAMP(state->speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    bool pattern_changed = desired.pattern != pattern;
    bool changed = pattern_changed || desired.speed != speed;
    if (replace_brightness) {
        uint8_t next = MIN(brightness, 100);
        changed |= desired.brightness != next;
        desired.brightness = next;
    }
    bool was_idle = (desired.flags & (LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE)) == LED_SYNC_IDLE_OFF;
    desired.pattern = pattern;
    desired.speed = speed;
    desired.flags = (desired.flags & ~(LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR)) |
                    (state->idle_off ? LED_SYNC_IDLE_OFF : 0) |
                    (state->advertising_indicator ? LED_SYNC_INDICATOR : 0);
    if (pattern_changed || !state->advertising_indicator)
        desired.flags &= ~LED_SYNC_ADVERTISING;
    bool now_idle = (desired.flags & (LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE)) == LED_SYNC_IDLE_OFF;
    publish_locked(pattern_changed || (was_idle && !now_idle));
    return changed;
}
bool led_scheduled_set_state(const struct led_pattern_state *state) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool changed = set_state_locked(state, false, 0);
    k_mutex_unlock(&sync_mutex);
    return changed;
}
bool led_scheduled_set_presentation(const struct led_pattern_state *state, uint8_t brightness) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool changed = set_state_locked(state, true, brightness);
    k_mutex_unlock(&sync_mutex);
    return changed;
}
bool led_scheduled_request(enum led_pattern_field field, int32_t value) {
    if (field != LED_PATTERN_FIELD_BRIGHTNESS && field != LED_PATTERN_FIELD_PATTERN &&
        field != LED_PATTERN_FIELD_SPEED)
        return false;
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool changed, reset = false;
    if (field == LED_PATTERN_FIELD_BRIGHTNESS) {
        uint8_t brightness = (uint8_t)CLAMP(value, 0, 100);
        changed = brightness != desired.brightness;
        desired.brightness = brightness;
    } else if (field == LED_PATTERN_FIELD_PATTERN) {
        uint8_t pattern = (uint8_t)CLAMP(value, 0, LED_PATTERN_COUNT - 1);
        changed = pattern != desired.pattern;
        desired.pattern = pattern;
        if (changed)
            desired.flags &= ~LED_SYNC_ADVERTISING;
        reset = changed;
    } else {
        uint16_t speed = (uint16_t)CLAMP(value, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
        changed = speed != desired.speed;
        desired.speed = speed;
    }
    publish_locked(reset);
    k_mutex_unlock(&sync_mutex);
    return changed;
}
void led_scheduled_activity(bool active) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    local_active = active;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    bool waking = !(desired.flags & LED_SYNC_ACTIVE) && active;
    bool changed = ((desired.flags & LED_SYNC_ACTIVE) != 0) != active;
    desired.flags = (desired.flags & ~LED_SYNC_ACTIVE) | (active ? LED_SYNC_ACTIVE : 0);
    if (changed && (desired.flags & LED_SYNC_IDLE_OFF))
        publish_locked(waking);
#else
    ARG_UNUSED(active); /* The central sees positions from both halves. */
#endif
    bool redraw = started && !engine.have_active && !engine.suspended;
    k_mutex_unlock(&sync_mutex);
    if (redraw)
        k_work_reschedule_for_queue(&render_queue, &apply_work, K_NO_WAIT);
}
void led_scheduled_endpoint(bool advertising) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint8_t previous = desired.flags;
    desired.flags =
        (desired.flags & ~LED_SYNC_ADVERTISING) |
        (advertising && (desired.flags & LED_SYNC_INDICATOR) ? LED_SYNC_ADVERTISING : 0);
    if (previous != desired.flags)
        publish_locked(true);
    k_mutex_unlock(&sync_mutex);
}
void led_scheduled_pm_prepare(void) {
    const uint32_t nonce = sys_rand32_get();
    k_mutex_lock(&sync_mutex, K_FOREVER);
    if (started && !engine.suspended) {
        engine.suspended = true;
        led_sync_link(&engine, link_up, k_uptime_get(), nonce);
    }
    k_mutex_unlock(&sync_mutex);
}
void led_scheduled_pm(bool suspended) {
    if (suspended) {
        led_scheduled_pm_prepare();
        struct k_work_sync sync;
        k_work_cancel_delayable_sync(&tx_work, &sync);
        k_work_cancel_delayable_sync(&apply_work, &sync);
    } else {
        const uint32_t nonce = sys_rand32_get();
        k_mutex_lock(&sync_mutex, K_FOREVER);
        if (started && engine.suspended) {
            engine.suspended = false;
            led_sync_link(&engine, link_up, k_uptime_get(), nonce);
            if (!IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
                led_sync_request_clock(&engine, k_uptime_get());
            schedule_locked(k_uptime_get());
        }
        k_mutex_unlock(&sync_mutex);
    }
}
void led_scheduled_render_begin(struct led_scheduled_view *out) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    const uint64_t now = k_uptime_get();
    bool applied = led_sync_advance(&engine, now);
    report_stage_locked(now);
    *out = (struct led_scheduled_view){
        .values = engine.have_active ? engine.visible : desired,
        .normal = engine.have_active && !engine.suspended,
        .elapsed_ms = engine.last_logical >= engine.phase_epoch
                          ? engine.last_logical - engine.phase_epoch
                          : 0,
        .stage = engine.stage,
        .status_started_at = status_started_at,
        .applied_sequence = applied && engine.have_active ? engine.active.sequence : 0,
    };
    if (!out->normal)
        out->values.flags =
            (out->values.flags & ~LED_SYNC_ACTIVE) | (local_active ? LED_SYNC_ACTIVE : 0);
    schedule_locked(now);
    k_mutex_unlock(&sync_mutex);
}
void led_scheduled_render_end(uint32_t applied_sequence, bool succeeded) {
    if (!applied_sequence)
        return;
    k_mutex_lock(&sync_mutex, K_FOREVER);
    if (engine.active.sequence == applied_sequence)
        led_sync_rendered(&engine, succeeded, k_uptime_get());
    bool replay = engine.replay_previous && !engine.suspended;
    engine.replay_previous = false;
    schedule_locked(k_uptime_get());
    k_mutex_unlock(&sync_mutex);
    if (replay)
        k_work_reschedule_for_queue(&render_queue, &apply_work, K_NO_WAIT);
}
