/* SPDX-License-Identifier: MIT
 * Actual Zephyr queues/mutex/cancellation and unmodified owner functions.
 * Relay, remote PWM and connection APIs are controlled stand-ins; no radio,
 * physical edge, power or target stack-usage claim follows from this fixture.
 */
#define CONFIG_ZMK_SPLIT 1
#ifdef LED_NATIVE_PERIPHERAL
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 0
#else
#define CONFIG_ZMK_SPLIT_ROLE_CENTRAL 1
#endif
#define CONFIG_ZMK_BLE 1
#define CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY 10
#define LED_NATIVE_KERNEL 1
#include "led_stubs.h"
#include "led_scheduled_sync.h"
#include "led_sync_wire.h"
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#define BT_CONN_ROLE_CENTRAL 0
struct bt_conn { uint8_t role; bool has_info; };
struct bt_conn_info { uint8_t role; };
struct bt_conn_cb {
    void (*connected)(struct bt_conn *, uint8_t);
    void (*disconnected)(struct bt_conn *, uint8_t);
};
struct zmk_split_peripheral_status_changed { zmk_event_t header; bool connected; };
struct zmk_led_scheduled_wire { uint8_t source; uint8_t bytes[LED_SYNC_WIRE_SIZE - 1]; } __packed;
_Static_assert(sizeof(struct zmk_led_scheduled_wire) == LED_SYNC_WIRE_SIZE, "native uses actual wire size");
struct wire_event { zmk_event_t header; struct zmk_led_scheduled_wire wire; };
static int receive_listener(const zmk_event_t *);
static void connected(struct bt_conn *, uint8_t);
static void disconnected(struct bt_conn *, uint8_t);
static int peripheral_status_listener(const zmk_event_t *);
static struct bt_conn_cb connection_callbacks = {.connected = connected, .disconnected = disconnected};
static int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    info->role = conn->role; return conn->has_info ? 0 : -ENOTCONN;
}
static int bt_conn_cb_register(struct bt_conn_cb *cb) { zassert_equal(cb, &connection_callbacks); return 0; }
static const struct zmk_split_peripheral_status_changed *
as_zmk_split_peripheral_status_changed(const zmk_event_t *event) {
    return event->kind == EVENT_PERIPHERAL_STATUS ? (const void *)event : NULL;
}
static const struct zmk_led_scheduled_wire *as_zmk_led_scheduled_wire(const zmk_event_t *event) {
    return event->kind == EVENT_MIRROR ? &((const struct wire_event *)event)->wire : NULL;
}

static struct pattern_sample segment_sample(const struct pattern_segment *, size_t, int64_t);

/* Production adapter metadata, using real kernel objects and the target's
 * separate1536-byte render stack. Controller metadata comes from led_stubs. */
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
static void tx_handler(struct k_work *);
static void apply_handler(struct k_work *);
static void observe_tx_handler(struct k_work *);
static void observe_apply_handler(struct k_work *);
static K_WORK_DELAYABLE_DEFINE(tx_work, observe_tx_handler);
static K_WORK_DELAYABLE_DEFINE(apply_work, observe_apply_handler);
static atomic_t tx_callbacks, apply_callbacks, relay_sends, peer_sends, fail_pwm_once;
K_THREAD_STACK_DEFINE(tx_stack, 4096);
K_THREAD_STACK_DEFINE(peer_stack, 4096);
static struct k_work_q peer_queue;
static K_MUTEX_DEFINE(peer_mutex);
static struct led_sync_engine remote_peer;
static void peer_tx_handler(struct k_work *);
static void peer_apply_handler(struct k_work *);
static K_WORK_DELAYABLE_DEFINE(peer_tx_work, peer_tx_handler);
static K_WORK_DELAYABLE_DEFINE(peer_apply_work, peer_apply_handler);
static K_SEM_DEFINE(tx_entered, 0, 1);
static K_SEM_DEFINE(tx_release, 0, 1);
static K_SEM_DEFINE(pwm_event, 0, 128);
static K_SEM_DEFINE(pwm_entered, 0, 1);
static K_SEM_DEFINE(pwm_release, 0, 1);
static atomic_t block_pwm_level;
static K_SEM_DEFINE(render_entered, 0, 1);
static K_SEM_DEFINE(render_release, 0, 1);
static K_SEM_DEFINE(pm_done, 0, 1);
static atomic_t block_tx, tx_is_blocked, auto_peer;
static K_MUTEX_DEFINE(trace_mutex);
struct pwm_trace { uint64_t time; uint8_t level; uint32_t sequence; int result; unsigned order; };
static struct pwm_trace pwm_trace[128];
static size_t pwm_count;
static unsigned trace_sequence;
static struct led_sync_packet last_packet;
static uint64_t tx_blocked_at, tx_released_at;
static struct k_thread pm_thread;
K_THREAD_STACK_DEFINE(pm_stack, 2048);
static bool pm_thread_started;
static uint64_t peer_clock_anchor;
static bool peer_clock_slow;
static bool observe_rebind_stage;
static uint64_t peer_now(void) {
    uint64_t elapsed = (uint64_t)k_uptime_get() - peer_clock_anchor;
    return peer_clock_anchor + (peer_clock_slow ? elapsed * 999U / 1000U : elapsed);
}

static struct zmk_led_scheduled_wire encode_packet(struct led_sync_packet packet) {
    struct zmk_led_scheduled_wire wire;
    packet.source = 1;
    zassert_true(led_sync_wire_encode((uint8_t *)&wire, sizeof(wire), &packet));
    return wire;
}
static void inject_wire(struct zmk_led_scheduled_wire wire) {
    struct wire_event event = {.header = {EVENT_MIRROR}, .wire = wire};
    event.wire.source = 1; /* Actual relay metadata replaces the transmitted source. */
    printk("WIRE time=%lld RX role=%c size=%u version=%u kind=%u\n",
           (long long)k_uptime_get(), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 'C' : 'P',
           (unsigned)sizeof(wire), wire.bytes[0], wire.bytes[1]);
    zassert_equal(receive_listener(&event.header), ZMK_EV_EVENT_BUBBLE);
}
static void inject(struct led_sync_packet packet) { inject_wire(encode_packet(packet)); }
static void peer_schedule_locked(uint64_t now) {
    uint64_t next = led_sync_next_tx(&remote_peer, now);
    if (next != LED_SYNC_NEVER)
        k_work_reschedule_for_queue(&peer_queue, &peer_tx_work, K_MSEC(next > now ? next - now : 0));
    next = led_sync_next_render(&remote_peer, now);
    if (next != LED_SYNC_NEVER)
        k_work_reschedule_for_queue(&peer_queue, &peer_apply_work, K_MSEC(next > now ? next - now : 0));
}
static int raise_zmk_led_scheduled_wire(struct zmk_led_scheduled_wire wire) {
    struct led_sync_packet packet;
    atomic_inc(&relay_sends);
    zassert_true(led_sync_wire_decode(&packet, (const uint8_t *)&wire, sizeof(wire)));
    k_mutex_lock(&trace_mutex, K_FOREVER);
    last_packet = packet;
    printk("TRACE %u time=%lld TX role=%c kind=%u seq=%u central_epoch=%u peer_epoch=%u size=56\n",
           trace_sequence++, (long long)k_uptime_get(), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 'C' : 'P',
           packet.kind, packet.sequence, packet.central_epoch, packet.peer_epoch);
    k_mutex_unlock(&trace_mutex);
    if (atomic_clear(&block_tx)) {
        tx_blocked_at = k_uptime_get();
        atomic_set(&tx_is_blocked, 1);
        k_sem_give(&tx_entered);
        zassert_ok(k_sem_take(&tx_release, K_SECONDS(10)), "fixture TX release missing");
        tx_released_at = k_uptime_get();
        atomic_clear(&tx_is_blocked);
    }
    if (atomic_get(&auto_peer)) {
        packet.source = 1;
        k_mutex_lock(&peer_mutex, K_FOREVER);
        led_sync_receive(&remote_peer, &packet, peer_now());
        if (observe_rebind_stage && remote_peer.have_active &&
            (!remote_peer.active.valid ||
             remote_peer.active.sequence <= remote_peer.resume_after_sequence)) {
            printk("SESSION actual=%lld peer=%llu role=%c invalidated=1 stage=%d\n",
                   (long long)k_uptime_get(), (unsigned long long)peer_now(),
                   remote_peer.central ? 'C' : 'P', remote_peer.stage);
            zassert_true(remote_peer.active_rendered &&
                             (!remote_peer.central || remote_peer.active.confirmed),
                         "observer requires the completed confirmed prior A");
            zassert_equal(remote_peer.stage, LED_SYNC_COASTING,
                          "completed A lost confirmed COASTING before any draw");
        }
        peer_schedule_locked(peer_now());
        k_mutex_unlock(&peer_mutex);
    }
    return 0;
}
static void peer_tx_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct led_sync_packet packet;
    k_mutex_lock(&peer_mutex, K_FOREVER);
    bool send = atomic_get(&auto_peer) && led_sync_send(&remote_peer, peer_now(), &packet);
    peer_schedule_locked(peer_now());
    k_mutex_unlock(&peer_mutex);
    if (send) {
        atomic_inc(&peer_sends);
        printk("PEER actual=%lld local=%llu kind=%u seq=%u\n",
               (long long)k_uptime_get(), (unsigned long long)peer_now(), packet.kind, packet.sequence);
        inject(packet);
    }
}
static void peer_apply_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    if (atomic_get(&auto_peer) && led_sync_advance(&remote_peer, peer_now()))
        led_sync_rendered(&remote_peer, true, peer_now());
    peer_schedule_locked(peer_now());
    k_mutex_unlock(&peer_mutex);
}

/* Identified negative control: route potentially blocking TX onto the very
 * render queue it must be independent from. Production definitions stay intact. */
#ifdef LED_NATIVE_SHARE_TX_QUEUE
static struct k_work_q *negative_tx_queue(void) { return &render_queue; }
#define zmk_workqueue_lowprio_work_q negative_tx_queue
#endif
#ifdef LED_NATIVE_OLD_RX_DISPOSITION
/* Identified old adapter disposition: the core still rejects malformed,
 * wrong-session and duplicate STATE, but its matching pending value is copied
 * into desired as though that receive had newly admitted a state. */
static bool negative_receive_disposition(struct led_sync_engine *state,
                                          const struct led_sync_packet *packet, uint64_t now) {
    bool accepted = led_sync_receive(state, packet, now);
    return accepted || (!state->central && packet->kind == LED_SYNC_STATE &&
        state->pending.valid && state->pending.sequence == packet->sequence &&
        led_sync_state_equal(&state->pending.wire, &packet->data.applied.state));
}
#define led_sync_receive negative_receive_disposition
#endif
#ifdef LED_NATIVE_ACCEPT_NONCANONICAL_WIRE
/* Identified validator control: accept one nonzero unused STATE tail byte.
 * The actual codec still processes all meaningful fields and protocol rules. */
static bool negative_wire_decode(struct led_sync_packet *out, const uint8_t *bytes, size_t size) {
    if (bytes && size == LED_SYNC_WIRE_SIZE && bytes[2] == LED_SYNC_STATE && bytes[55]) {
        uint8_t normalized[LED_SYNC_WIRE_SIZE];
        memcpy(normalized, bytes, sizeof(normalized));
        normalized[55] = 0;
        return led_sync_wire_decode(out, normalized, sizeof(normalized));
    }
    return led_sync_wire_decode(out, bytes, size);
}
#define led_sync_wire_decode negative_wire_decode
#endif
#ifdef LED_NATIVE_NO_COAST_REBASE
/* Identified old floor policy: keep enforcing the stale presentation floor on
 * a newly accepted reset-phase plan. The production clock/receive definitions
 * and the exact future wire state remain unchanged. */
static bool negative_no_rebase(struct led_sync_engine *state,
                               const struct led_sync_packet *packet, uint64_t now) {
    bool accepted = led_sync_receive(state, packet, now);
    if (accepted && !state->central && state->pending.reset_phase && led_sync_coasting(state))
        state->pending.reset_phase = false;
    return accepted;
}
#define led_sync_receive negative_no_rebase
#endif
#ifdef LED_NATIVE_NO_RENDER_ROLLBACK
/* Identified missing recovery: discard the prior successful presentation just
 * before the real completion handler sees a failed PWM. Desired/session/clock
 * fields and production definitions remain unchanged. */
static void negative_no_rollback(struct led_sync_engine *state, bool succeeded, uint64_t now) {
    if (!succeeded) state->previous.valid = false;
    led_sync_rendered(state, succeeded, now);
}
#define led_sync_rendered negative_no_rollback
#endif
#ifdef LED_NATIVE_SKIP_REPLAY
/* Identified missing adapter recovery: retain the actual restored metadata but
 * suppress only its explicit request for an asynchronous old-view write. */
static void negative_skip_replay(struct led_sync_engine *state, bool succeeded, uint64_t now) {
    led_sync_rendered(state, succeeded, now);
    if (!succeeded) state->replay_previous = false;
}
#define led_sync_rendered negative_skip_replay
#endif
#ifdef LED_NATIVE_CANCEL_RUNNING_TX
/* Identified unsafe full-cancel control: replace only the adapter's parked
 * timeout operation with cancellation. The selected kernel refuses a new
 * submission until the blocked old handler exits; protocol definitions stay
 * unchanged. */
static int negative_cancel_instead_of_park(struct k_work_q *queue,
                                           struct k_work_delayable *dwork,
                                           k_timeout_t delay) {
    if (dwork == &tx_work && K_TIMEOUT_EQ(delay, K_FOREVER))
        return k_work_cancel_delayable(dwork);
    return k_work_reschedule_for_queue(queue, dwork, delay);
}
#define k_work_reschedule_for_queue negative_cancel_instead_of_park
#endif
/* DRIVER_FUNCTIONS */
#ifdef LED_NATIVE_CANCEL_RUNNING_TX
#undef k_work_reschedule_for_queue
#endif
#ifdef LED_NATIVE_SKIP_REPLAY
#undef led_sync_rendered
#endif
#ifdef LED_NATIVE_NO_RENDER_ROLLBACK
#undef led_sync_rendered
#endif
#ifdef LED_NATIVE_NO_COAST_REBASE
#undef led_sync_receive
#endif
#ifdef LED_NATIVE_ACCEPT_NONCANONICAL_WIRE
#undef led_sync_wire_decode
#endif
#ifdef LED_NATIVE_OLD_RX_DISPOSITION
#undef led_sync_receive
#endif
#ifdef LED_NATIVE_SHARE_TX_QUEUE
#undef zmk_workqueue_lowprio_work_q
#endif

#ifdef LED_NATIVE_IDLE_OFF_ACTIVITY
/* Identified old radio trigger: publish raw ACTIVE changes even when idle-off
 * is disabled and the effective LED state is unchanged. */
static void negative_activity(bool active) {
    led_scheduled_activity(active);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    if (engine.central && !(desired.flags & LED_SYNC_IDLE_OFF)) {
        led_sync_desire(&engine, desired, false);
        led_sync_reserve(&engine, k_uptime_get());
        schedule_locked(k_uptime_get());
    }
    k_mutex_unlock(&sync_mutex);
}
#define led_scheduled_activity negative_activity
#endif

/* Count actual kernel callbacks, not only relay frames: a consumed expiry must
 * not silently turn into a steady-output workqueue spin. */
static void observe_tx_handler(struct k_work *work) {
    atomic_inc(&tx_callbacks);
    tx_handler(work);
#ifdef LED_NATIVE_RETRY_EXHAUSTED
    k_mutex_lock(&sync_mutex, K_FOREVER);
    if (engine.central && engine.exhausted) {
        led_sync_begin_transaction(&engine);
        engine.dirty = true;
        engine.clock_ready = false;
        engine.clock_due = (uint64_t)k_uptime_get() + 30000;
        schedule_locked(k_uptime_get());
    }
    k_mutex_unlock(&sync_mutex);
#endif
}
static void observe_apply_handler(struct k_work *work) {
    atomic_inc(&apply_callbacks);
    apply_handler(work);
#ifdef LED_NATIVE_COAST_WAKEUP
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint64_t now = k_uptime_get();
    uint64_t logical = led_sync_logical_raw(&engine, now);
    bool coasting = engine.have_active && !engine.suspended &&
        (led_sync_coasting(&engine) || !led_sync_clock_valid_at(&engine.active_clock, logical, 0));
    k_mutex_unlock(&sync_mutex);
    if (coasting)
        k_work_reschedule_for_queue(&render_queue, &apply_work, K_MSEC(1000));
#endif
}
static int native_pwm(const struct device *dev, uint32_t index, uint8_t level) {
    if (level && atomic_cas(&block_pwm_level, level, 0)) {
#ifdef LED_NATIVE_EARLY_SYNC_STAGE
        /* Identified old semantic: publish completion before the own PWM call. */
        k_mutex_lock(&sync_mutex, K_FOREVER);
        engine.stage = LED_SYNC_SYNCED;
        k_mutex_unlock(&sync_mutex);
#endif
        k_sem_give(&pwm_entered);
        zassert_ok(k_sem_take(&pwm_release, K_SECONDS(5)), "fixture PWM release missing");
    }
    int result = atomic_clear(&fail_pwm_once) ? -EIO : led_set_brightness(dev, index, level);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint32_t sequence = engine.have_active ? engine.active.sequence : 0;
    k_mutex_unlock(&sync_mutex);
    k_mutex_lock(&trace_mutex, K_FOREVER);
    zassert_true(pwm_count < ARRAY_SIZE(pwm_trace));
    pwm_trace[pwm_count++] = (struct pwm_trace){k_uptime_get(), level, sequence, result, trace_sequence};
    printk("TRACE %u time=%lld PWM role=%c level=%u seq=%u result=%d\n", trace_sequence++,
           (long long)k_uptime_get(), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 'C' : 'P',
           level, sequence, result);
    k_mutex_unlock(&trace_mutex);
    k_sem_give(&pwm_event);
    return result;
}
#ifdef LED_NATIVE_OLD_DRAW
/* Identified old-view mutation: the due commit occurs, but a controller uses
 * the state it copied before that commit for its first draw. */
static void negative_render_begin(struct led_scheduled_view *out) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool due = engine.pending.valid && (uint64_t)k_uptime_get() >= engine.pending.local_deadline;
    struct led_sync_values old = engine.visible;
    k_mutex_unlock(&sync_mutex);
    led_scheduled_render_begin(out);
    if (due && out->normal) out->values = old;
}
#define led_scheduled_render_begin negative_render_begin
#endif
#define led_set_brightness native_pwm
/* DRIVER_CONTROLLER_FUNCTIONS */
#undef led_set_brightness
#ifdef LED_NATIVE_OLD_DRAW
#undef led_scheduled_render_begin
#endif

static void render_blocker(struct k_work *work) {
    ARG_UNUSED(work);
    k_sem_give(&render_entered);
    zassert_ok(k_sem_take(&render_release, K_SECONDS(5)));
}
static K_WORK_DEFINE(render_block_work, render_blocker);
static void pm_entry(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND));
    k_sem_give(&pm_done);
}
static void clear_trace(void) {
    k_mutex_lock(&trace_mutex, K_FOREVER);
    pwm_count = 0;
    k_mutex_unlock(&trace_mutex);
    k_sem_reset(&pwm_event);
}
/* Native simulated time remains the actual Zephyr uptime. Keeping only the
 * last500ms of the in-memory PWM index bounds fixture storage; every ordered
 * timestamp/level remains in the raw log. No previous scenario uses this helper. */
static void run_for(uint32_t milliseconds) {
    while (milliseconds) {
        uint32_t slice = MIN(milliseconds, 500U);
        k_sleep(K_MSEC(slice));
        clear_trace();
        milliseconds -= slice;
    }
}
static void enable_peer(void) {
    atomic_set(&auto_peer, 1);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    peer_schedule_locked(peer_now());
    k_mutex_unlock(&peer_mutex);
}
static void assert_coasting(uint8_t brightness) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool normal = engine.have_active && engine.active_rendered &&
                  engine.visible.brightness == brightness;
    enum led_sync_stage stage = engine.stage;
    uint64_t next = led_sync_next_render(&engine, k_uptime_get());
    k_mutex_unlock(&sync_mutex);
    printk("COAST actual=%lld role=%c stage=%d tx=%ld peer_tx=%ld tx_work=%ld apply_work=%ld\n",
           (long long)k_uptime_get(), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 'C' : 'P',
           stage, (long)atomic_get(&relay_sends), (long)atomic_get(&peer_sends),
           (long)atomic_get(&tx_callbacks), (long)atomic_get(&apply_callbacks));
    zassert_true(normal, "expired clock stopped the prior successful presentation");
    zassert_equal(stage, LED_SYNC_COASTING);
    zassert_equal(next, LED_SYNC_NEVER, "consumed expiry retained a render deadline");
}

static struct pwm_trace wait_level(uint8_t level, uint32_t timeout_ms) {
    int64_t until = k_uptime_get() + timeout_ms;
    for (;;) {
        k_mutex_lock(&trace_mutex, K_FOREVER);
        for (size_t i = 0; i < pwm_count; ++i) {
            if (pwm_trace[i].level == level) {
                struct pwm_trace result = pwm_trace[i];
                k_mutex_unlock(&trace_mutex);
                return result;
            }
        }
        k_mutex_unlock(&trace_mutex);
        int64_t remaining = until - k_uptime_get();
        zassert_true(remaining > 0, "reserved PWM level%u never arrived", level);
        zassert_ok(k_sem_take(&pwm_event, K_MSEC(remaining)), "reserved PWM did not complete");
    }
}
/* An unchanged stand-in hardware level is insufficient: require the actual
 * successful recovery API call, ordered after the failed new-view API call. */
static void require_successful_replay_write(void) {
    int64_t until = k_uptime_get() + 100;
    for (;;) {
        struct pwm_trace failed = {0}, replay = {0};
        size_t failures = 0, replays = 0;
        k_mutex_lock(&trace_mutex, K_FOREVER);
        for (size_t i = 0; i < pwm_count; ++i) {
            struct pwm_trace entry = pwm_trace[i];
            if (entry.level == 81 && entry.result == -EIO) { failed = entry; failures++; }
            if (failures && entry.level == 23 && entry.result == 0 && entry.order > failed.order) {
                replay = entry; replays++;
            }
        }
        k_mutex_unlock(&trace_mutex);
        if (replays) {
            zassert_equal(failures, 1);
            zassert_equal(replays, 1, "duplicate asynchronous recovery write");
            zassert_true(replay.time >= failed.time && replay.order > failed.order);
            zassert_equal(replay.sequence, 1);
            printk("REPLAY failed_time=%llu failed_order=%u result=%d replay_time=%llu replay_order=%u result=%d\n",
                   (unsigned long long)failed.time, failed.order, failed.result,
                   (unsigned long long)replay.time, replay.order, replay.result);
            return;
        }
        zassert_true(k_uptime_get() < until,
                     "failed81 API call had no later successful23 recovery WRITE");
        k_sleep(K_MSEC(1));
    }
}

static void established(struct led_sync_engine *state, bool central, uint64_t now) {
    struct led_sync_values values = {LED_PATTERN_STEADY, 23, 100,
                                    LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR};
    const uint32_t quantization =
        2U * MAX(1U, DIV_ROUND_UP(1000U, CONFIG_SYS_CLOCK_TICKS_PER_SEC));
    led_sync_init(state, central, central ? 101 : 202, quantization, values);
    zassert_equal(state->quantization_ms, 20U, "native100Hz requires20ms timestamp budget");
    state->linked = state->clock_ready = state->have_active = true;
    state->epoch = 1;
    state->peer_boot = 202; state->peer_epoch = 1;
    state->central_boot = 101; state->central_epoch = 1;
    state->sequence = state->clock_sequence = 1;
    state->dirty = state->reset_phase = false;
    state->stage = LED_SYNC_SYNCED;
    state->visible = values;
    zassert_true(led_sync_clock_sample(&state->fit, now, now, now, now, quantization));
    state->active_clock = state->fit;
    state->active.valid = state->active.confirmed = true;
    state->active.sequence = 1;
    state->active.clock = state->fit;
    state->active.local_deadline = now;
    state->active.wire = (struct led_sync_state_wire){1, now, now, values.pattern,
                                                      values.brightness, values.speed, values.flags};
    state->active_rendered = true;
    state->rendered_at = now;
    state->last_logical = state->phase_floor = state->phase_epoch = now;
}
static uint64_t reserve_brightness(uint8_t brightness) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zassert_true(led_scheduled_set_brightness(brightness));
#else
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct led_sync_packet packet = led_sync_packet_new(&engine, LED_SYNC_STATE, engine.sequence + 1);
    packet.data.applied.state = (struct led_sync_state_wire){engine.clock_sequence,
        (uint64_t)k_uptime_get() + LED_SYNC_LEAD_MS, engine.phase_epoch,
        LED_PATTERN_STEADY, brightness, 100, LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR};
    k_mutex_unlock(&sync_mutex);
    inject(packet);
#endif
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool valid = engine.pending.valid;
    uint64_t deadline = engine.pending.local_deadline;
    k_mutex_unlock(&sync_mutex);
    zassert_true(valid);
    return deadline;
}
static void cancel_work(void) {
    struct k_work_sync sync;
    atomic_clear(&auto_peer);
    k_sem_give(&tx_release);
    k_sem_give(&pwm_release);
    k_sem_give(&render_release);
    k_work_cancel_sync(&render_block_work, &sync);
    k_work_cancel_delayable_sync(&tx_work, &sync);
    k_work_cancel_delayable_sync(&apply_work, &sync);
    k_work_cancel_delayable_sync(&pattern_work, &sync);
    k_work_cancel_delayable_sync(&peer_tx_work, &sync);
    k_work_cancel_delayable_sync(&peer_apply_work, &sync);
    if (pm_thread_started) {
        zassert_ok(k_thread_join(&pm_thread, K_SECONDS(1)));
        pm_thread_started = false;
    }
}
static void before(void *unused) {
    ARG_UNUSED(unused);
    cancel_work();
    k_sem_reset(&tx_entered); k_sem_reset(&tx_release);
    k_sem_reset(&pwm_entered); k_sem_reset(&pwm_release); atomic_clear(&block_pwm_level);
    k_sem_reset(&render_entered); k_sem_reset(&render_release); k_sem_reset(&pm_done);
    atomic_clear(&block_tx); atomic_clear(&tx_is_blocked); atomic_clear(&powering_off);
    atomic_clear(&fail_pwm_once); observe_rebind_stage = false; peer_clock_slow = false; peer_clock_anchor = k_uptime_get();
    led_result = 0; controller_ready = true; link_up = true; local_active = true;
    uint64_t now = k_uptime_get();
    k_mutex_lock(&sync_mutex, K_FOREVER);
    established(&engine, IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL), now);
    desired = engine.desired;
    reported_stage = LED_SYNC_SYNCED; status_started_at = now;
    k_mutex_unlock(&sync_mutex);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    established(&remote_peer, !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL), now);
    k_mutex_unlock(&peer_mutex);
    clear_trace();
    refresh_pattern_output();
    (void)wait_level(23, 100);
    clear_trace();
    atomic_clear(&tx_callbacks); atomic_clear(&apply_callbacks); atomic_clear(&relay_sends); atomic_clear(&peer_sends);
}
static void after(void *unused) { ARG_UNUSED(unused); cancel_work(); }
static void *setup(void) {
    k_work_queue_start(&lowprio_queue, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
                       CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY, NULL);
    k_work_queue_start(&peer_queue, peer_stack, K_THREAD_STACK_SIZEOF(peer_stack),
                       CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY, NULL);
    led_scheduled_start(refresh_pattern_output);
    zassert_equal(CONFIG_SYS_CLOCK_TICKS_PER_SEC, 100);
    zassert_equal(engine.quantization_ms, 20U, "actual adapter must use the native tick budget");
    printk("NATIVE ticks_per_second=%u quantization_ms=%u\n",
           CONFIG_SYS_CLOCK_TICKS_PER_SEC, engine.quantization_ms);
    return NULL;
}

struct completion_snapshot {
    enum led_sync_stage stage;
    bool have_active, active_rendered, confirmed;
    uint8_t active_failure;
    uint32_t sequence;
};
static struct completion_snapshot wait_render_completion(uint32_t timeout_ms);
static void trace_stage(const char *, struct completion_snapshot);

ZTEST(led_scheduled_kernel, test_blocked_tx_does_not_delay_reserved_draw) {
    atomic_set(&block_tx, 1);
    uint64_t deadline = reserve_brightness(37);
    zassert_ok(k_sem_take(&tx_entered, K_MSEC(100)));
    zassert_true(atomic_get(&tx_is_blocked));
    struct pwm_trace draw = wait_level(37, LED_SYNC_LEAD_MS + 300);
    zassert_true(atomic_get(&tx_is_blocked), "render waited for transport release");
    zassert_true(draw.time >= deadline && draw.time - deadline <= LED_SYNC_LATE_MS);
    zassert_true(draw.time >= tx_blocked_at && draw.sequence > 1);
    struct completion_snapshot rendered = wait_render_completion(100);
    trace_stage("own-pwm-without-peer-confirmation", rendered);
    zassert_true(rendered.active_rendered);
    zassert_equal(rendered.stage, IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
                                    ? LED_SYNC_RUNNING_UNCONFIRMED : LED_SYNC_SYNCED);
    k_sem_give(&tx_release);
}
/* The controller is paused immediately before its actual PWM API call, while
 * the adapter mutex remains free for a real receive-listener interleaving. */
static struct completion_snapshot engine_snapshot(void) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct completion_snapshot snapshot = {engine.stage, engine.have_active,
        engine.active_rendered, engine.active.confirmed, engine.active_failure, engine.active.sequence};
    k_mutex_unlock(&sync_mutex);
    return snapshot;
}
static struct completion_snapshot wait_render_completion(uint32_t timeout_ms) {
    int64_t until = k_uptime_get() + timeout_ms;
    for (;;) {
        struct completion_snapshot snapshot = engine_snapshot();
        if (snapshot.active_rendered || snapshot.active_failure) return snapshot;
        zassert_true(k_uptime_get() < until, "own PWM completion was never recorded");
        k_sleep(K_MSEC(1));
    }
}
static void trace_stage(const char *point, struct completion_snapshot state) {
    printk("STAGE time=%lld role=%c point=%s stage=%u rendered=%u confirmed=%u failure=%u seq=%u\n",
           (long long)k_uptime_get(), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 'C' : 'P',
           point, state.stage, state.active_rendered, state.confirmed,
           state.active_failure, state.sequence);
}
ZTEST(led_scheduled_kernel, test_stage_waits_for_local_render_completion) {
    atomic_set(&block_pwm_level, 79);
    uint64_t deadline = reserve_brightness(79);
    zassert_ok(k_sem_take(&pwm_entered, K_MSEC(LED_SYNC_LEAD_MS + 200)));
    struct completion_snapshot snapshot = engine_snapshot();
    trace_stage("before-own-pwm", snapshot);
    zassert_true(snapshot.have_active && !snapshot.active_rendered);
    zassert_equal(snapshot.stage, LED_SYNC_RUNNING_UNCONFIRMED);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct led_sync_packet early = led_sync_packet_new(&engine, LED_SYNC_STATE_ACK,
                                                       engine.active.sequence);
    early.data.applied.state = engine.active.wire;
    k_mutex_unlock(&sync_mutex);
    early.status = LED_SYNC_APPLIED;
    early.data.applied.actual_local = deadline;
    inject(early);
    snapshot = engine_snapshot();
    trace_stage("peer-applied-before-own-pwm", snapshot);
    zassert_true(snapshot.confirmed && !snapshot.active_rendered);
    zassert_equal(snapshot.stage, LED_SYNC_RUNNING_UNCONFIRMED,
                  "peer completion cannot complete the own PWM call");
#else
    ARG_UNUSED(deadline);
#endif
    k_sem_give(&pwm_release);
    snapshot = wait_render_completion(100);
    trace_stage("after-own-pwm", snapshot);
    zassert_true(snapshot.active_rendered && snapshot.have_active);
    zassert_equal(snapshot.stage, LED_SYNC_SYNCED);
}
ZTEST(led_scheduled_kernel, test_render_failure_never_reports_synced) {
    atomic_set(&block_pwm_level, 81);
    (void)reserve_brightness(81);
    zassert_ok(k_sem_take(&pwm_entered, K_MSEC(LED_SYNC_LEAD_MS + 200)));
    struct completion_snapshot snapshot = engine_snapshot();
    trace_stage("before-failing-own-pwm", snapshot);
    zassert_equal(snapshot.stage, LED_SYNC_RUNNING_UNCONFIRMED);
    zassert_false(snapshot.active_rendered);
    (void)led_scheduled_set_brightness(87); /* Must survive presentation rollback. */
    atomic_set(&fail_pwm_once, 1);
    k_sem_give(&pwm_release);
    int64_t until = k_uptime_get() + 100;
    bool restored;
    do {
        k_mutex_lock(&sync_mutex, K_FOREVER);
        restored = engine.have_active && engine.active_rendered && engine.active.sequence == 1;
        k_mutex_unlock(&sync_mutex);
        if (restored) break;
        k_sleep(K_MSEC(1));
    } while (k_uptime_get() < until);
    zassert_true(restored, "failed new PWM did not restore prior successful presentation");
    struct led_sync_values latest;
    led_scheduled_get_desired(&latest);
    zassert_equal(latest.brightness, 87, "rollback clobbered a later desired edit");
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool old_view = engine.visible.brightness == 23 && engine.active.sequence == 1 &&
                    engine.active.wire.brightness == 23;
    bool pending_latest = !engine.central || (engine.pending.valid &&
                                                engine.pending.wire.brightness == 87);
    k_mutex_unlock(&sync_mutex);
    zassert_true(old_view && pending_latest, "failed state was promoted or later request lost");
    require_successful_replay_write();
    snapshot = engine_snapshot();
    trace_stage("after-failure-prior-view-restored", snapshot);
    zassert_equal(snapshot.sequence, 1, "failed reservation must never be confirmed");
    zassert_equal(led_level, 23);
}

ZTEST(led_scheduled_kernel, test_codec_rejects_malformed_without_mutation) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    atomic_set(&block_tx, 1);
    uint64_t deadline = reserve_brightness(83);
    zassert_ok(k_sem_take(&tx_entered, K_MSEC(100)));
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct led_sync_packet packet = led_sync_packet_new(&engine, LED_SYNC_STATE_ACK,
                                                        engine.pending.sequence);
    packet.status = LED_SYNC_ACCEPTED;
    packet.data.applied.state = engine.pending.wire;
    uint64_t retry_at = engine.state_due;
    uint8_t attempts = engine.state_attempts;
    k_mutex_unlock(&sync_mutex);
    struct zmk_led_scheduled_wire canonical = encode_packet(packet);
    struct zmk_led_scheduled_wire malformed = canonical;
    ((uint8_t *)&malformed)[48] = 1; /* ACCEPTED must have actual_local == 0. */
    inject_wire(malformed);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool retained = engine.pending.valid && engine.pending.local_deadline == deadline &&
                    engine.state_due == retry_at && engine.state_attempts == attempts;
    k_mutex_unlock(&sync_mutex);
    zassert_true(retained, "malformed ACK retired or moved pending work");
    inject_wire(canonical);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool retired = engine.pending.valid && engine.pending.local_deadline == deadline &&
                   engine.state_due == LED_SYNC_NEVER &&
                   engine.state_attempts == LED_SYNC_STATE_ATTEMPTS;
    k_mutex_unlock(&sync_mutex);
    zassert_true(retired, "canonical ACK did not retire exact pending copies");
    zassert_equal(k_work_delayable_remaining_get(&tx_work), 0,
                  "accepted ACK left a detached retry timer");
    zassert_true(k_work_delayable_is_pending(&apply_work),
                 "retiring TX must preserve the reserved apply");
    k_sem_give(&tx_release);
    struct pwm_trace draw = wait_level(83, LED_SYNC_LEAD_MS + 200);
#else
    (void)led_scheduled_set_brightness(39);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint32_t sequence_before = engine.sequence;
    uint64_t deadline = (uint64_t)k_uptime_get() + LED_SYNC_LEAD_MS;
    struct led_sync_packet packet = led_sync_packet_new(&engine, LED_SYNC_STATE,
                                                        sequence_before + 1);
    packet.data.applied.state = (struct led_sync_state_wire){engine.clock_sequence,
        deadline, engine.phase_epoch, LED_PATTERN_STEADY, 85, 100,
        LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR};
    k_mutex_unlock(&sync_mutex);
    struct zmk_led_scheduled_wire canonical = encode_packet(packet);
    struct zmk_led_scheduled_wire malformed = canonical;
    ((uint8_t *)&malformed)[55] = 1; /* STATE unused tail must be canonical zero. */
    inject_wire(malformed);
    struct led_sync_values current;
    led_scheduled_get_desired(&current);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool retained = !engine.pending.valid && engine.sequence == sequence_before;
    k_mutex_unlock(&sync_mutex);
    zassert_equal(current.brightness, 39, "malformed codec STATE changed desired");
    zassert_true(retained, "malformed codec STATE consumed a reservation or sequence");
    zassert_equal(led_level, 23, "receive must not draw a future state");
    inject_wire(canonical); /* Rejection must not consume the same valid sequence. */
    led_scheduled_get_desired(&current);
    zassert_equal(current.brightness, 85);
    zassert_equal(led_level, 23);
    struct pwm_trace draw = wait_level(85, LED_SYNC_LEAD_MS + 200);
#endif
    zassert_true(draw.time >= deadline && draw.time - deadline <= LED_SYNC_LATE_MS);
}

ZTEST(led_scheduled_kernel, test_due_commit_precedes_queued_animation) {
    k_work_submit_to_queue(&render_queue, &render_block_work);
    zassert_ok(k_sem_take(&render_entered, K_MSEC(100)));
    uint64_t deadline = reserve_brightness(41);
    schedule_pattern_work(K_MSEC(100)); /* Older animation is ready before apply_work. */
    uint64_t now = k_uptime_get();
    zassert_true(deadline >= now);
    k_sleep(K_MSEC(deadline - now + 10));
    k_mutex_lock(&trace_mutex, K_FOREVER);
    size_t before_count = pwm_count;
    k_mutex_unlock(&trace_mutex);
    zassert_equal(before_count, 0);
    k_sem_give(&render_release);
    (void)wait_level(41, 100);
    k_mutex_lock(&trace_mutex, K_FOREVER);
    size_t count = pwm_count;
    struct pwm_trace first = pwm_trace[0];
    k_mutex_unlock(&trace_mutex);
    zassert_true(count > 0);
    zassert_equal(first.level, 41, "older animation drew stale state before commit");
    zassert_true(first.time >= deadline);
}
ZTEST(led_scheduled_kernel, test_frozen_mapping_keeps_accepted_deadline) {
    uint64_t deadline = reserve_brightness(43);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint64_t now = k_uptime_get();
    struct led_sync_clock_fit newer;
    zassert_true(led_sync_clock_sample(&newer, now, now + 8, now + 8, now, engine.quantization_ms));
    engine.fit = newer; /* Controlled newer fit; accepted reservation owns its old copy. */
    engine.clock_sequence++;
    zassert_equal(engine.pending.local_deadline, deadline);
    zassert_equal(engine.pending.clock.offset_ms, 0);
    schedule_locked(now);
    k_mutex_unlock(&sync_mutex);
    struct pwm_trace draw = wait_level(43, LED_SYNC_LEAD_MS + 200);
    zassert_true(draw.time >= deadline && draw.time - deadline <= LED_SYNC_LATE_MS);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    zassert_equal(engine.active.clock.offset_ms, 0);
    zassert_equal(engine.active.wire.apply_at, deadline);
    k_mutex_unlock(&sync_mutex);
}
ZTEST(led_scheduled_kernel, test_pm_darkens_before_blocked_tx_and_fences_resume) {
    atomic_set(&block_tx, 1);
    (void)reserve_brightness(47);
    zassert_ok(k_sem_take(&tx_entered, K_MSEC(100)));
    struct led_sync_packet old;
    k_mutex_lock(&trace_mutex, K_FOREVER); old = last_packet; k_mutex_unlock(&trace_mutex);
    pm_thread_started = true;
    k_thread_create(&pm_thread, pm_stack, K_THREAD_STACK_SIZEOF(pm_stack), pm_entry,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    struct pwm_trace dark = wait_level(0, 100);
    zassert_true(atomic_get(&tx_is_blocked));
    zassert_not_equal(k_sem_take(&pm_done, K_NO_WAIT), 0, "PM did not wait for blocked TX");
    zassert_true(dark.time >= tx_blocked_at);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    zassert_true(engine.suspended && !engine.pending.valid && !engine.clock_ready);
    k_mutex_unlock(&sync_mutex);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    (void)led_scheduled_set_brightness(53);
#endif
    k_work_reschedule_for_queue(&render_queue, &pattern_work, K_NO_WAIT);
    k_sleep(K_MSEC(20));
    zassert_equal(led_level, 0);
    k_sem_give(&tx_release);
    zassert_ok(k_sem_take(&pm_done, K_MSEC(200)));
    zassert_true(dark.time <= tx_released_at);
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME));
    old.source = 1;
    old.kind = IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? LED_SYNC_STATE_ACK : LED_SYNC_STATE;
    old.status = IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? LED_SYNC_APPLIED : LED_SYNC_OK;
    if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
        old.data.applied.actual_local = old.data.applied.state.apply_at;
    inject(old); /* Old epoch cannot revive a canceled reservation after resume. */
    k_mutex_lock(&sync_mutex, K_FOREVER);
    zassert_false(engine.suspended || engine.pending.valid || engine.clock_ready || engine.have_active);
    zassert_equal(engine.stage, LED_SYNC_LINKED);
    k_mutex_unlock(&sync_mutex);
}
ZTEST(led_scheduled_kernel, test_unilateral_resume_completes_new_session) {
    observe_rebind_stage = true;
    k_mutex_lock(&peer_mutex, K_FOREVER);
    uint32_t peer_boot = remote_peer.boot, peer_epoch = remote_peer.epoch;
    k_mutex_unlock(&peer_mutex);
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND));
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    (void)led_scheduled_set_brightness(61);
#else
    k_mutex_lock(&peer_mutex, K_FOREVER);
    struct led_sync_values next = remote_peer.desired;
    next.brightness = 61;
    led_sync_desire(&remote_peer, next, false);
    /* Actual peripheral resume REQUEST must trigger the untouched central. */
    remote_peer.dirty = false;
    remote_peer.clock_due = remote_peer.clock_timeout = LED_SYNC_NEVER;
    remote_peer.transaction_active = false;
    k_mutex_unlock(&peer_mutex);
#endif
    atomic_set(&auto_peer, 1);
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME));
    k_mutex_lock(&peer_mutex, K_FOREVER);
    peer_schedule_locked(peer_now());
    k_mutex_unlock(&peer_mutex);
    (void)wait_level(61, LED_SYNC_START_MS + LED_SYNC_LEAD_MS + 1500);
    int64_t until = k_uptime_get() + 300;
    for (;;) {
        k_mutex_lock(&sync_mutex, K_FOREVER);
        bool local_ok = engine.stage == LED_SYNC_SYNCED && engine.have_active;
        k_mutex_unlock(&sync_mutex);
        k_mutex_lock(&peer_mutex, K_FOREVER);
        bool peer_ok = remote_peer.stage == LED_SYNC_SYNCED && remote_peer.have_active;
        zassert_equal(remote_peer.boot, peer_boot);
        zassert_equal(remote_peer.epoch, peer_epoch, "untouched peer link was reset by fixture");
        zassert_true(remote_peer.linked);
        k_mutex_unlock(&peer_mutex);
        if (local_ok && peer_ok) break;
        zassert_true(k_uptime_get() < until, "new clock/state never mutually completed");
        k_sleep(K_MSEC(5));
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static K_SEM_DEFINE(request_ready, 0, 2);
static K_SEM_DEFINE(request_go, 0, 2);
static K_SEM_DEFINE(request_done, 0, 2);
static struct k_thread request_threads[2];
K_THREAD_STACK_ARRAY_DEFINE(request_stacks, 2, 2048);
static void request_entry(void *field_arg, void *value_arg, void *unused) {
    ARG_UNUSED(unused);
    enum led_pattern_field field = (enum led_pattern_field)(uintptr_t)field_arg;
    int32_t value = (int32_t)(uintptr_t)value_arg;
#ifdef LED_NATIVE_OLD_REQUEST
    struct led_sync_values snapshot;
    led_scheduled_get_desired(&snapshot);
#endif
    k_sem_give(&request_ready);
    zassert_ok(k_sem_take(&request_go, K_SECONDS(1)));
#ifdef LED_NATIVE_OLD_REQUEST
    if (field == LED_PATTERN_FIELD_PATTERN) snapshot.pattern = (uint8_t)value;
    else snapshot.speed = (uint16_t)value;
    struct led_pattern_state stale = {snapshot.pattern, snapshot.speed,
        (snapshot.flags & LED_SYNC_INDICATOR) != 0, (snapshot.flags & LED_SYNC_IDLE_OFF) != 0};
    (void)led_scheduled_set_state(&stale); /* Identified old get-modify-whole-state race. */
#else
    (void)led_scheduled_request(field, value);
#endif
    k_sem_give(&request_done);
}
ZTEST(led_scheduled_kernel, test_concurrent_requests_preserve_independent_fields) {
    k_sem_reset(&request_ready); k_sem_reset(&request_go); k_sem_reset(&request_done);
    for (uintptr_t i = 0; i < 2; ++i)
        k_thread_create(&request_threads[i], request_stacks[i], K_THREAD_STACK_SIZEOF(request_stacks[i]),
                        request_entry, (void *)(uintptr_t)(i ? LED_PATTERN_FIELD_SPEED : LED_PATTERN_FIELD_PATTERN),
                        (void *)(uintptr_t)(i ? 250 : 7), NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&request_ready, K_MSEC(100)));
    zassert_ok(k_sem_take(&request_ready, K_MSEC(100)));
    k_sem_give(&request_go); k_sem_give(&request_go);
    zassert_ok(k_sem_take(&request_done, K_MSEC(100)));
    zassert_ok(k_sem_take(&request_done, K_MSEC(100)));
    for (unsigned i = 0; i < 2; ++i) zassert_ok(k_thread_join(&request_threads[i], K_MSEC(100)));
    struct led_sync_values actual;
    led_scheduled_get_desired(&actual);
    zassert_equal(actual.pattern, 7);
    zassert_equal(actual.speed, 250);
    zassert_equal(actual.brightness, 23);
    struct led_pattern_state complete = {5, 175, false, false};
    zassert_true(led_scheduled_set_presentation(&complete, 47));
    led_scheduled_get_desired(&actual);
    zassert_equal(actual.pattern, 5);
    zassert_equal(actual.speed, 175);
    zassert_equal(actual.brightness, 47);
    zassert_equal(actual.flags & (LED_SYNC_IDLE_OFF | LED_SYNC_INDICATOR), 0);
    zassert_false(led_scheduled_set_presentation(&complete, 47));
    complete.pattern = 6;
    zassert_true(led_scheduled_set_state(&complete));
    led_scheduled_get_desired(&actual);
    zassert_equal(actual.pattern, 6);
    zassert_equal(actual.speed, 175);
    zassert_equal(actual.brightness, 47, "state-only setter replaced independent brightness");
    zassert_false(led_scheduled_set_state(&complete));
}
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZTEST(led_scheduled_kernel, test_ignored_relay_never_replays_pending_desired) {
    (void)reserve_brightness(37);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct led_sync_packet repeated = led_sync_packet_new(&engine, LED_SYNC_STATE, engine.pending.sequence);
    repeated.data.applied.state = engine.pending.wire;
    k_mutex_unlock(&sync_mutex);
    (void)led_scheduled_set_brightness(39);
    struct zmk_led_scheduled_wire invalid_version = encode_packet(repeated);
    invalid_version.bytes[0]++;
    inject_wire(invalid_version);
    struct led_sync_values current;
    led_scheduled_get_desired(&current);
    zassert_equal(current.brightness, 39, "bad-version wire changed desired");
    struct led_sync_packet invalid = repeated;
    invalid.central_epoch++;
    inject(invalid);
    led_scheduled_get_desired(&current);
    zassert_equal(current.brightness, 39, "wrong-session STATE replayed pending desired");
    inject(repeated); /* Valid duplicate ACKs the old plan; it cannot replay its value. */
    led_scheduled_get_desired(&current);
    zassert_equal(current.brightness, 39, "duplicate STATE replayed pending desired");
    (void)wait_level(37, LED_SYNC_LEAD_MS + 200);
    clear_trace();
    (void)reserve_brightness(67);
    led_scheduled_get_desired(&current);
    zassert_equal(current.brightness, 67);
    zassert_equal(led_level, 37); /* Desired updates before the future edge. */
    (void)wait_level(67, LED_SYNC_LEAD_MS + 200);
}
#endif

ZTEST(led_scheduled_kernel, test_unchanged_coast_has_no_transport_or_worker_spin) {
    enable_peer();
    run_for(9000);
    assert_coasting(23);
    atomic_val_t applies = atomic_get(&apply_callbacks);
    atomic_val_t tx_work_count = atomic_get(&tx_callbacks);
    run_for(60000);
    assert_coasting(23);
    zassert_equal(led_level, 23);
    zassert_equal(atomic_get(&relay_sends), 0, "unchanged LED triggered background TX");
    zassert_equal(atomic_get(&peer_sends), 0, "unchanged peer triggered background TX");
    zassert_equal(atomic_get(&tx_callbacks), tx_work_count, "idle TX worker woke again");
    zassert_equal(atomic_get(&apply_callbacks), applies, "consumed expiry kept waking render worker");
    zassert_true(applies <= 1, "steady presentation needed repeated expiry callbacks");
    /* A delayed old completion confirms history only; its original render time
     * is valid, but it cannot make this69-second-old fit fresh again. */
    k_mutex_lock(&sync_mutex, K_FOREVER);
    struct led_sync_packet old = led_sync_packet_new(&engine,
        engine.central ? LED_SYNC_STATE_ACK : LED_SYNC_STATE, engine.active.sequence);
    old.data.applied.state = engine.active.wire;
    old.status = engine.central ? LED_SYNC_APPLIED : LED_SYNC_OK;
    old.data.applied.actual_local = engine.central ? engine.active.local_deadline : 0;
    if (engine.central) {
        engine.active.confirmed = false;
        led_sync_presentation_stage(&engine, k_uptime_get());
        zassert_equal(engine.stage, LED_SYNC_COASTING_UNCONFIRMED);
    }
    k_mutex_unlock(&sync_mutex);
    inject(old);
    run_for(10);
    assert_coasting(23);
    zassert_equal(atomic_get(&relay_sends), IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? 0 : 1,
                  "historical duplicate caused more than its single reactive ACK");
}
ZTEST(led_scheduled_kernel, test_animation_continues_after_clock_expiry_without_tx) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    engine.visible.pattern = engine.desired.pattern = engine.active.wire.pattern = LED_PATTERN_BREATHE;
    desired = engine.desired;
    k_mutex_unlock(&sync_mutex);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    remote_peer.visible.pattern = remote_peer.desired.pattern = remote_peer.active.wire.pattern = LED_PATTERN_BREATHE;
    k_mutex_unlock(&peer_mutex);
    enable_peer();
    refresh_pattern_output();
    bool changed_after_expiry = false;
    uint8_t first = 255;
    for (unsigned part = 0; part < 36; ++part) {
        k_sleep(K_MSEC(500));
        k_mutex_lock(&trace_mutex, K_FOREVER);
        if (part >= 18) {
            for (size_t i = 0; i < pwm_count; ++i) {
                if (first == 255) first = pwm_trace[i].level;
                else if (first != pwm_trace[i].level) changed_after_expiry = true;
            }
        }
        k_mutex_unlock(&trace_mutex);
        clear_trace();
    }
    assert_coasting(23);
    zassert_true(changed_after_expiry, "normal animation stopped after the fit expired");
    zassert_equal(atomic_get(&relay_sends), 0);
    zassert_equal(atomic_get(&peer_sends), 0);
    zassert_equal(atomic_get(&tx_callbacks), 0);
    zassert_true(atomic_get(&apply_callbacks) <= 1);
}
ZTEST(led_scheduled_kernel, test_long_coast_refit_rebases_only_at_future_edge) {
    peer_clock_slow = true; /* Monotonic counterpart rate999/1000, never a clock jump. */
    enable_peer();
    printk("ANCHOR start actual=%lld counterpart=%llu rate=999/1000\n",
           (long long)k_uptime_get(), (unsigned long long)peer_now());
    run_for(300000);
    assert_coasting(23);
    zassert_equal(atomic_get(&relay_sends), 0);
    zassert_equal(atomic_get(&peer_sends), 0);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    uint64_t old_phase = engine.phase_epoch;
    k_mutex_unlock(&sync_mutex);
    printk("ANCHOR change actual=%lld counterpart=%llu\n",
           (long long)k_uptime_get(), (unsigned long long)peer_now());
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zassert_true(led_scheduled_set_brightness(89));
#else
    k_mutex_lock(&peer_mutex, K_FOREVER);
    struct led_sync_values next = remote_peer.desired;
    next.brightness = 89;
    led_sync_desire(&remote_peer, next, false);
    led_sync_reserve(&remote_peer, peer_now());
    peer_schedule_locked(peer_now());
    k_mutex_unlock(&peer_mutex);
#endif
    uint64_t deadline = 0, phase = 0;
    int64_t until = k_uptime_get() + LED_SYNC_START_MS + 1000;
    while (!deadline && k_uptime_get() < until) {
        k_mutex_lock(&sync_mutex, K_FOREVER);
        if (engine.pending.valid) {
            deadline = engine.pending.local_deadline;
            phase = engine.pending.wire.phase_epoch;
            zassert_equal(phase, engine.pending.wire.apply_at);
            zassert_true(led_sync_clock_valid_at(&engine.pending.clock,
                                                engine.pending.wire.apply_at, 0));
            zassert_equal(engine.visible.brightness, 23);
            zassert_equal(engine.phase_epoch, old_phase, "receive rebased the running animation");
            zassert_equal(engine.active_clock.offset_ms, 0, "receive replaced the active mapping");
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            uint32_t old_extra = led_sync_extra(&engine, &engine.pending.clock, deadline);
            printk("REFIT actual=%lld counterpart=%llu offset=%lld stale_floor_error=%u future=%llu\n",
                   (long long)k_uptime_get(), (unsigned long long)peer_now(),
                   (long long)engine.pending.clock.offset_ms, old_extra,
                   (unsigned long long)deadline);
            zassert_true(old_extra > 50, "control did not exercise a backwards fresh mapping");
#endif
        }
        k_mutex_unlock(&sync_mutex);
        if (!deadline) k_sleep(K_MSEC(5));
    }
    zassert_true(deadline > (uint64_t)k_uptime_get() + LED_SYNC_RECEIVE_LEAD_MS,
                 "fresh future reservation missing");
    zassert_equal(led_level, 23);
    uint64_t before_edge = deadline - (uint64_t)k_uptime_get() - 10;
    run_for(before_edge);
    zassert_equal(led_level, 23, "coasting output changed before the reserved edge");
    struct pwm_trace draw = wait_level(89, 200);
    zassert_true(draw.time >= deadline && draw.time - deadline <= LED_SYNC_LATE_MS);
    run_for(100);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    zassert_true(engine.have_active && engine.active_rendered);
    zassert_equal(engine.visible.brightness, 89);
    zassert_equal(engine.phase_epoch, phase);
    zassert_equal(engine.stage, LED_SYNC_SYNCED);
    k_mutex_unlock(&sync_mutex);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    zassert_true(remote_peer.have_active && remote_peer.active_rendered);
    zassert_equal(remote_peer.visible.brightness, 89);
    zassert_equal(remote_peer.phase_epoch, phase);
    zassert_equal(remote_peer.stage, LED_SYNC_SYNCED);
    k_mutex_unlock(&peer_mutex);
}
ZTEST(led_scheduled_kernel, test_failed_transaction_goes_silent_until_real_trigger) {
    /* Drop every outgoing frame at the relay boundary. Both roles must finish
     * their finite foreground burst without relying on an idle timer. */
    run_for(9000);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zassert_true(led_scheduled_set_brightness(91));
#else
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND));
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME));
#endif
    run_for(LED_SYNC_START_MS + (LED_SYNC_CLOCK_ATTEMPTS + 1) * LED_SYNC_RETRY_MS);
    zassert_equal(atomic_get(&relay_sends), LED_SYNC_CLOCK_ATTEMPTS,
                  "failed transaction exceeded its finite clock/hint budget");
    atomic_val_t tx_work_count = atomic_get(&tx_callbacks);
    run_for(60000);
    zassert_equal(atomic_get(&relay_sends), LED_SYNC_CLOCK_ATTEMPTS,
                  "idle retry renewed an exhausted transaction");
    zassert_equal(atomic_get(&tx_callbacks), tx_work_count,
                  "exhausted transaction retained an idle retry callback");
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zassert_false(led_scheduled_set_brightness(91)); /* Exact no-op must stay silent. */
#else
    zassert_false(led_scheduled_set_brightness(23));
#endif
    run_for(1000);
    zassert_equal(atomic_get(&relay_sends), LED_SYNC_CLOCK_ATTEMPTS);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zassert_true(led_scheduled_set_brightness(93));
#else
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_SUSPEND));
    zassert_ok(led_pattern_pm_action(NULL, PM_DEVICE_ACTION_RESUME));
#endif
    run_for(LED_SYNC_START_MS + (LED_SYNC_CLOCK_ATTEMPTS + 1) * LED_SYNC_RETRY_MS);
    zassert_equal(atomic_get(&relay_sends), 2 * LED_SYNC_CLOCK_ATTEMPTS,
                  "a new effective edit or real resume did not rearm its finite burst");
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZTEST(led_scheduled_kernel, test_disabled_idle_policy_activity_and_endpoint_are_radio_noops) {
    k_mutex_lock(&sync_mutex, K_FOREVER);
    desired.flags &= ~LED_SYNC_IDLE_OFF;
    engine.desired.flags = engine.visible.flags = engine.active.wire.flags = desired.flags;
    k_mutex_unlock(&sync_mutex);
    k_mutex_lock(&peer_mutex, K_FOREVER);
    remote_peer.desired.flags &= ~LED_SYNC_IDLE_OFF;
    remote_peer.visible.flags = remote_peer.active.wire.flags = remote_peer.desired.flags;
    k_mutex_unlock(&peer_mutex);
    enable_peer();
    led_scheduled_activity(false);
    k_sleep(K_MSEC(20));
    led_scheduled_activity(true);
    led_scheduled_activity(true);
    led_scheduled_endpoint(false);
    led_scheduled_endpoint(false);
    led_scheduled_activity(false);
    k_sleep(K_MSEC(20));
    zassert_equal(atomic_get(&relay_sends), 0, "disabled idle policy caused unnecessary activity TX");
    zassert_equal(atomic_get(&peer_sends), 0);
    zassert_equal(led_level, 23);
    struct led_sync_values latest;
    led_scheduled_get_desired(&latest);
    zassert_false(latest.flags & LED_SYNC_ACTIVE, "raw latest activity was not retained");
    struct led_pattern_state state = {LED_PATTERN_STEADY, 100, true, true};
    (void)led_scheduled_set_state(&state); /* Enabling idle-off observes the actual inactive state. */
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool off = engine.pending.valid && !(engine.pending.wire.flags & LED_SYNC_ACTIVE) &&
               (engine.pending.wire.flags & LED_SYNC_IDLE_OFF);
    uint64_t off_at = engine.pending.local_deadline;
    k_mutex_unlock(&sync_mutex);
    zassert_true(off, "enabling idle-off while inactive did not reserve OFF");
    zassert_equal(led_level, 23);
    struct pwm_trace dark = wait_level(0, LED_SYNC_LEAD_MS + 200);
    zassert_true(dark.time >= off_at && dark.time - off_at <= LED_SYNC_LATE_MS);
    run_for(100);
    led_scheduled_activity(true);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool on = engine.pending.valid && (engine.pending.wire.flags & LED_SYNC_ACTIVE);
    uint64_t on_at = engine.pending.local_deadline;
    k_mutex_unlock(&sync_mutex);
    zassert_true(on, "real wake under enabled idle-off did not reserve ON");
    zassert_equal(led_level, 0);
    struct pwm_trace lit = wait_level(23, LED_SYNC_LEAD_MS + 200);
    zassert_true(lit.time >= on_at && lit.time - on_at <= LED_SYNC_LATE_MS);
}
#endif

ZTEST(led_scheduled_kernel, test_invalid_request_fields_preserve_state_and_work) {
    static const enum led_pattern_field invalid_fields[] = {
        (enum led_pattern_field)-1, (enum led_pattern_field)3, (enum led_pattern_field)INT32_MAX,
    };
    struct led_sync_values before_values;
    led_scheduled_get_desired(&before_values);
    int tx_flags_before = k_work_delayable_busy_get(&tx_work);
    atomic_val_t tx_callbacks_before = atomic_get(&tx_callbacks);
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_fields); ++i) {
        zassert_false(led_scheduled_request(invalid_fields[i], 123),
                      "invalid selector was accepted as a speed request");
        struct led_sync_values after_values;
        led_scheduled_get_desired(&after_values);
        zassert_true(led_sync_values_equal(&before_values, &after_values));
        k_mutex_lock(&sync_mutex, K_FOREVER);
        bool unchanged = engine.sequence == 1 && engine.clock_sequence == 1 &&
                         !engine.pending.valid && !engine.transaction_active;
        k_mutex_unlock(&sync_mutex);
        zassert_true(unchanged, "invalid selector created a transaction");
        zassert_equal(k_work_delayable_remaining_get(&tx_work), 0);
        zassert_equal(k_work_delayable_busy_get(&tx_work), tx_flags_before,
                      "invalid selector changed the parked work descriptor");
        zassert_equal(atomic_get(&tx_callbacks), tx_callbacks_before);
    }
    zassert_equal(atomic_get(&relay_sends), 0);
}
ZTEST(led_scheduled_kernel, test_disconnect_retires_retry_and_preserves_redraw) {
    /* Start an actual finite resume exchange, then disconnect while its first
     * relay call is blocked and its next retry is already queued. */
    led_scheduled_pm(true);
    atomic_set(&block_tx, 1);
    led_scheduled_pm(false);
    zassert_ok(k_sem_take(&tx_entered, K_MSEC(LED_SYNC_START_MS + 200)));
    zassert_true(k_work_delayable_remaining_get(&tx_work) > 0);
    link_changed(false);
    k_mutex_lock(&sync_mutex, K_FOREVER);
    bool disconnected_state = !engine.linked && led_sync_next_tx(&engine, k_uptime_get()) == LED_SYNC_NEVER;
    k_mutex_unlock(&sync_mutex);
    zassert_true(disconnected_state);
    zassert_equal(k_work_delayable_remaining_get(&tx_work), 0, "disconnect left a detached retry timer");
    zassert_true(atomic_get(&tx_is_blocked));
    /* apply_work also carries imperative redraw requests. Canceling its old
     * deadline must never discard the link-status redraw. */
    (void)wait_level(100, 100);
    zassert_true(atomic_get(&tx_is_blocked));
    atomic_val_t callbacks_before_release = atomic_get(&tx_callbacks);
    k_sem_give(&tx_release);
    run_for(LED_SYNC_RETRY_MS + 100);
    zassert_equal(atomic_get(&tx_callbacks), callbacks_before_release,
                  "retired retry woke after disconnect");
}


ZTEST(led_scheduled_kernel, test_retired_tx_accepts_new_work_while_old_send_is_blocked) {
    led_scheduled_pm(true);
    atomic_set(&block_tx, 1);
    led_scheduled_pm(false);
    zassert_ok(k_sem_take(&tx_entered, K_MSEC(LED_SYNC_START_MS + 200)));
    link_changed(false); /* Retire the old retry while its relay call still runs. */
    zassert_equal(k_work_delayable_remaining_get(&tx_work), 0);
    link_changed(true);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* A real reconnect schedules its next PROBE. Let that timeout expire
     * before releasing the previous relay call. */
    run_for(LED_SYNC_START_MS + 20);
#else
    /* The other half's new PROBE requests an immediate REPLY, using the actual
     * wire decoder and protocol admission while the old REQUEST is blocked. */
    k_mutex_lock(&peer_mutex, K_FOREVER);
    struct led_sync_packet packet = led_sync_packet_new(&remote_peer, LED_SYNC_PROBE, 2);
    packet.data.clock.t1 = peer_now();
    k_mutex_unlock(&peer_mutex);
    inject(packet);
    run_for(20);
#endif
    zassert_true(atomic_get(&tx_is_blocked));
    k_mutex_lock(&sync_mutex, K_FOREVER);
    zassert_not_equal(led_sync_next_tx(&engine, k_uptime_get()), LED_SYNC_NEVER,
                      "new session did not request transmission");
    k_mutex_unlock(&sync_mutex);
    printk("REARM time=%lld blocked=1 flags=%d remaining=%lld\n",
           (long long)k_uptime_get(), k_work_delayable_busy_get(&tx_work),
           (long long)k_work_delayable_remaining_get(&tx_work));
    k_sem_give(&tx_release);
    k_sleep(K_MSEC(50));
    zassert_true(atomic_get(&relay_sends) >= 2,
                 "new request was lost while canceled old send was running");
    zassert_false(k_work_delayable_busy_get(&tx_work) & K_WORK_CANCELING);
}

ZTEST_SUITE(led_scheduled_kernel, NULL, setup, before, after, NULL);
