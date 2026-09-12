/* Reusable external LED pattern controller. */

#define DT_DRV_COMPAT zmk_behavior_led_pattern

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/workqueue.h>
#include <zmk-led-patterns/led_pattern.h>
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zephyr/bluetooth/conn.h>
#include <zmk/split/central.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_INDEX 0

/*
 * The one clock left, and it runs only while a ramp is being drawn.
 *
 * Seventeen of the eighteen curves are step functions: they hold a level for a
 * known stretch and then change. Those say so, and the redraw is scheduled for
 * the moment they change -- a blink wakes twice a second, and the ten dark
 * seconds of Slow breathe are one sleep rather than four hundred. A ramp is
 * the one shape with no edges of its own, so it is the one shape that has to
 * be sampled, and 40 ms (25 steps a second) is fine enough that a three-second
 * fade reads as smooth.
 */
#define RAMP_STEP_MS 40

/*
 * The advertising indicator's dark level is a true zero, not a dim glow.
 *
 * pwm_nrfx_set_cycles() stops the whole PWM peripheral when every channel is
 * at 0% or 100% duty and drives the pin from GPIO instead. Any level in
 * between keeps the peripheral -- and the high-frequency clock it needs --
 * running. This indicator is shown exactly when the keyboard has no host,
 * which can be a long time, so its off half costs nothing.
 */
#define ADVERTISING_BLINK_PERIOD_MS 1500
#define ADVERTISING_BLINK_ON_MS 300
#define ADVERTISING_BLINK_BRIGHTNESS 70
#define ADVERTISING_BLINK_OFF_BRIGHTNESS 0

/* A settings apply writes a brightness and then a whole state, and dragging a
 * control in a client produces a stream of them. Coalescing into one message
 * is one radio event instead of many. */
#define MIRROR_DEBOUNCE_MS 20

static const struct device *const backlight = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));

static void pattern_work_handler(struct k_work *work);

/* Defined at compile time rather than initialised from this behavior's init,
 * so that nothing can submit it before it is usable: an init that runs after
 * the first submit would overwrite the queue's own list node inside an item
 * already on the list. */
static K_WORK_DELAYABLE_DEFINE(pattern_work, pattern_work_handler);
/* The resting pattern, once the halves have synced and nothing else has been
 * asked for. On a split the first thing seen is never this -- the connection
 * indicator below owns the LED until the two halves agree. */
static uint8_t active_pattern = LED_PATTERN_BREATHE;
static uint16_t speed_percent = LED_PATTERN_SPEED_NOMINAL;
/* Kept here rather than delegated to ZMK's backlight subsystem: `&bl` carries
 * a global split relay of its own, and driving both would put two senders on
 * the split link for a single key press. */
static uint8_t brightness_percent = 100;
static bool advertising_indicator = true;
static int64_t pattern_started_at;
static bool controller_ready;
static bool advertising_blink;
/* Set while ZMK reports the keyboard idle or asleep. The animation is the only
 * thing here that would otherwise keep waking the core after the last key
 * press, so it stops and the LED goes dark until activity returns. */
static bool animation_suspended;

/*
 * Everything this module schedules runs on ZMK's low-priority queue.
 *
 * Not the system work queue, which is where ZMK puts update_advertising_work
 * and raise_profile_changed_event_work -- the items that restart host
 * advertising after a disconnect. An animation that occupies that queue delays
 * exactly the work that makes the keyboard findable again. The split messages
 * have a second reason: Zephyr gives an ATT PDU allocation K_NO_WAIT when the
 * caller is the system work queue ("No blocking in the sysqueue",
 * bt_att_chan_create_pdu), so a relay write from there fails with -ENOMEM
 * whenever the pool is momentarily empty, while the same write from an
 * ordinary thread waits for a buffer.
 */
static inline void schedule_pattern_work(k_timeout_t delay) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &pattern_work, delay);
}

static void refresh_pattern_output(void);
static void schedule_mirror(k_timeout_t delay);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool should_show_advertising_blink(void);
#endif

/* ===== The two halves' connection, as three states and two messages ===== */

#if IS_ENABLED(CONFIG_ZMK_SPLIT)

/*
 * The LED says, at a glance, how far the pair has got.
 *
 *   WAITING  Blink,  as pattern 3   the other half is not there
 *   LINKED   Steady, as pattern 0   the BLE link is up, state not yet agreed
 *   SYNCED   released               the peripheral has confirmed what it is
 *                                   showing; the normal pattern takes over,
 *                                   whose default is 1 (Breathe)
 *
 * A pair that sticks on Steady is a pair whose link came up and whose state
 * exchange never completed, which is a different fault from one that never
 * linked -- and being able to tell those two apart from across the desk is the
 * whole reason this exists.
 *
 * The two shapes are drawn by split_status_sample() rather than by pattern id,
 * for a power reason spelled out there.
 */
enum split_stage {
    SPLIT_STAGE_WAITING = 0,
    SPLIT_STAGE_LINKED,
    SPLIT_STAGE_SYNCED,
};

/*
 * Timings for the state exchange.
 *
 * The link is reported as up before the central has discovered and subscribed
 * to the peripheral's relay characteristic, and a write sent before that is
 * dropped by the transport. Three seconds is comfortably past it, and it is
 * also long enough that Steady is legible as a stage of its own rather than a
 * flicker. The retries are bounded -- four sends, then the LED is left saying
 * "linked, never synced" -- so nothing here can become a loop.
 */
#define SPLIT_SYNC_START_DELAY_MS 3000
#define SPLIT_SYNC_ACK_TIMEOUT_MS 600
#define SPLIT_SYNC_MAX_ATTEMPTS 4

static enum split_stage split_stage = SPLIT_STAGE_WAITING;
static int64_t split_stage_started_at;

static bool split_status_showing(void) { return split_stage != SPLIT_STAGE_SYNCED; }

static void set_split_stage(enum split_stage stage) {
    if (stage == split_stage) {
        return;
    }

    LOG_INF("led: split stage %d -> %d", (int)split_stage, (int)stage);
    split_stage = stage;
    split_stage_started_at = k_uptime_get();
    /* The normal pattern restarts from the top of its cycle when the indicator
     * hands the LED back, so a release is visible as a fresh curve. */
    pattern_started_at = split_stage_started_at;
    refresh_pattern_output();
}

/*
 * Central to peripheral: the whole of what to show, as one small final-state
 * message. Not a remote behavior invocation -- the central does all the key
 * handling and the peripheral only redraws the exact values it receives.
 * `led` fits the four-byte relay name limit (three characters plus its NUL).
 */
struct zmk_led_pattern_mirror {
    uint8_t source;
    uint8_t pattern;
    uint8_t brightness;
    uint16_t speed;
    /* Milliseconds since the central began this pattern. Sending phase avoids
     * restarting a breathing/blinking curve when a peripheral reconnects. */
    uint32_t elapsed_ms;
    bool advertising_blink;
} __packed;

ZMK_EVENT_DECLARE(zmk_led_pattern_mirror);
ZMK_EVENT_IMPL(zmk_led_pattern_mirror);

/*
 * Peripheral to central: what it is actually showing now.
 *
 * This is what makes "synced" an observed fact rather than an assumption. The
 * central cannot otherwise know whether its write landed -- the transport
 * drops a relay write to a peripheral whose characteristics are not discovered
 * yet and says so only in a log line -- so without this the indicator could
 * only ever claim success. `lea` is three characters for the same reason
 * `led` is.
 */
struct zmk_led_pattern_ack {
    uint8_t source;
    uint8_t pattern;
} __packed;

ZMK_EVENT_DECLARE(zmk_led_pattern_ack);
ZMK_EVENT_IMPL(zmk_led_pattern_ack);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_led_pattern_mirror, led, source);
ZMK_RELAY_EVENT_HANDLE(zmk_led_pattern_ack, lea, source);
#else
ZMK_RELAY_EVENT_HANDLE(zmk_led_pattern_mirror, led, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_led_pattern_ack, lea, source);
#endif

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/*
 * Whether there is a peripheral to talk to, learned from the event rather than
 * asked for on a timer.
 *
 * bt_conn callbacks are a list, so a second registration here does not
 * displace ZMK's own. A link this half opened is one where it holds the
 * central role, which is exactly ZMK's own test for "not a host connection"
 * in ble.c.
 */
static bool peripheral_link_up;
static uint8_t sync_attempts;

static void mirror_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(mirror_work, mirror_work_handler);

static bool is_peripheral_conn(struct bt_conn *conn) {
    struct bt_conn_info info;

    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}

static void split_conn_connected(struct bt_conn *conn, uint8_t err) {
    if (err != 0 || !is_peripheral_conn(conn)) {
        return;
    }

    peripheral_link_up = true;
    sync_attempts = 0;
    set_split_stage(SPLIT_STAGE_LINKED);
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &mirror_work,
                                K_MSEC(SPLIT_SYNC_START_DELAY_MS));
}

static void split_conn_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);

    if (!is_peripheral_conn(conn)) {
        return;
    }

    peripheral_link_up = false;
    k_work_cancel_delayable(&mirror_work);
    set_split_stage(SPLIT_STAGE_WAITING);
}

BT_CONN_CB_DEFINE(led_pattern_split_conn_cb) = {
    .connected = split_conn_connected,
    .disconnected = split_conn_disconnected,
};

static void mirror_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!peripheral_link_up) {
        return;
    }

    struct zmk_led_pattern_mirror event = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
        .pattern = active_pattern,
        .brightness = brightness_percent,
        .speed = speed_percent,
        .elapsed_ms = (uint32_t)MAX(k_uptime_get() - pattern_started_at, (int64_t)0),
        .advertising_blink = advertising_blink,
    };

    raise_zmk_led_pattern_mirror(event);

    /* Only the connect-time exchange retries, and only until it is answered.
     * A mirror sent because the state changed is a single message: the
     * peripheral is known to be listening by then. */
    if (split_stage == SPLIT_STAGE_LINKED && ++sync_attempts < SPLIT_SYNC_MAX_ATTEMPTS) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &mirror_work,
                                    K_MSEC(SPLIT_SYNC_ACK_TIMEOUT_MS));
    }
}

static int led_pattern_ack_listener(const zmk_event_t *eh) {
    const struct zmk_led_pattern_ack *ack = as_zmk_led_pattern_ack(eh);

    /* A locally raised ack would be this half's own relay send, which only the
     * peripheral does. */
    if (ack == NULL || ack->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("led: peripheral %u acknowledged pattern %u", ack->source, ack->pattern);

    /* Only the connect-time exchange is cancelled, and only while it is still
     * the thing running. Every mirror is acknowledged, including the ones sent
     * because the state changed, and cancelling on one of those would be a
     * race against the next change's own pending send. */
    if (split_stage != SPLIT_STAGE_SYNCED) {
        k_work_cancel_delayable(&mirror_work);
        sync_attempts = 0;
        set_split_stage(SPLIT_STAGE_SYNCED);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_ack, led_pattern_ack_listener);
ZMK_SUBSCRIPTION(led_pattern_ack, zmk_led_pattern_ack);

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/*
 * Mirroring never happens on the caller's thread.
 *
 * A key press arrives on the behavior thread and a client's edit arrives on
 * the Studio RPC thread, and both of those end in a GATT write to the other
 * half. Handing the send to the low-priority queue keeps the split link off
 * threads that have a host waiting on them, and the shared delayable item
 * coalesces a burst into one message.
 */
static void schedule_mirror(k_timeout_t delay) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (!controller_ready || !peripheral_link_up) {
        return;
    }

    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &mirror_work, delay);
#else
    ARG_UNUSED(delay);
#endif
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* The ack leaves on the low-priority queue rather than from the relay receive
 * path it is answering, which runs on the Bluetooth receive thread. */
static void ack_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct zmk_led_pattern_ack event = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
        .pattern = active_pattern,
    };

    raise_zmk_led_pattern_ack(event);
}

static K_WORK_DEFINE(ack_work, ack_work_handler);

static int led_pattern_mirror_listener(const zmk_event_t *eh) {
    const struct zmk_led_pattern_mirror *event = as_zmk_led_pattern_mirror(eh);

    /* A locally raised mirror would be this half's own relay send, which only
     * the central does. A received one has source 0 (central) + 1. */
    if (event == NULL || event->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    active_pattern = event->pattern < LED_PATTERN_COUNT ? event->pattern : LED_PATTERN_STEADY;
    brightness_percent = MIN(100, event->brightness);
    speed_percent = CLAMP(event->speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    advertising_blink = event->advertising_blink;

    /* Before the phase is set, not after: releasing the indicator restarts the
     * pattern clock, and the whole point of carrying elapsed_ms is that this
     * half picks the curve up where the central is rather than at zero. */
    set_split_stage(SPLIT_STAGE_SYNCED);
    /* Match the central's phase, allowing only the one-way relay latency of
     * roughly 8 ms rather than restarting the curve at zero on reconnect. */
    pattern_started_at = k_uptime_get() - event->elapsed_ms;

    refresh_pattern_output();
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &ack_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_mirror, led_pattern_mirror_listener);
ZMK_SUBSCRIPTION(led_pattern_mirror, zmk_led_pattern_mirror);

static int led_pattern_peripheral_status_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(eh);

    if (status != NULL) {
        /* Back to the indicator on either edge: a reconnect has to re-agree
         * the state rather than keep showing what the last session left. */
        set_split_stage(status->connected ? SPLIT_STAGE_LINKED : SPLIT_STAGE_WAITING);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_peripheral_status, led_pattern_peripheral_status_listener);
ZMK_SUBSCRIPTION(led_pattern_peripheral_status, zmk_split_peripheral_status_changed);

#endif // split peripheral

/* ===== The eighteen curves, each able to say when it next changes ===== */

/*
 * One sample of a curve: the level now, and how long it stays there.
 *
 * `hold_ms` is what replaces the fixed animation tick. It is measured on the
 * pattern clock, so the speed setting scales it with the curve it came from,
 * and a hold of zero means the pattern is static and nothing needs to be
 * scheduled at all.
 */
struct pattern_sample {
    uint8_t brightness;
    uint32_t hold_ms;
};

/* A step function, as the boundaries at which it changes. The last entry's
 * `until_ms` is the period. */
struct pattern_segment {
    uint32_t until_ms;
    uint8_t brightness;
};

static struct pattern_sample segment_sample(const struct pattern_segment *segments, size_t count,
                                            int64_t elapsed_ms) {
    const uint32_t period = segments[count - 1].until_ms;
    const uint32_t phase = (uint32_t)(elapsed_ms % period);

    for (size_t i = 0; i < count; i++) {
        if (phase < segments[i].until_ms) {
            return (struct pattern_sample){.brightness = segments[i].brightness,
                                           .hold_ms = segments[i].until_ms - phase};
        }
    }

    /* Unreachable: phase is below the period, which is the last boundary. */
    return (struct pattern_sample){.brightness = segments[count - 1].brightness,
                                   .hold_ms = period};
}

/* A fixed-step level table: flicker and sparkle shapes, where every entry
 * lasts the same time. */
static struct pattern_sample table_sample(const uint8_t *levels, size_t count, uint32_t step_ms,
                                          int64_t elapsed_ms) {
    const uint32_t phase = (uint32_t)(elapsed_ms % ((int64_t)count * step_ms));

    return (struct pattern_sample){.brightness = levels[phase / step_ms],
                                   .hold_ms = step_ms - (phase % step_ms)};
}

/* The next RAMP_STEP_MS boundary, or the end of the ramp if that comes first.
 * Always at least 1, because phase is strictly below segment_end. */
static uint32_t ramp_hold(uint32_t phase, uint32_t segment_end) {
    return MIN(RAMP_STEP_MS - (phase % RAMP_STEP_MS), segment_end - phase);
}

#define SEGMENT_SAMPLE(segments, elapsed) segment_sample(segments, ARRAY_SIZE(segments), elapsed)
#define TABLE_SAMPLE(levels, step_ms, elapsed)                                                     \
    table_sample(levels, ARRAY_SIZE(levels), step_ms, elapsed)

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
/*
 * The connection indicator, drawn here rather than deferred to a pattern id.
 *
 * The shapes are the ones the pair is specified in -- Blink while the other
 * half is absent, Steady once the link is up -- with the blink's dim floor
 * taken to a true zero. pwm_nrfx_set_cycles() stops the whole PWM peripheral,
 * and so lets go of the high-frequency clock it needs, only when every channel
 * sits at exactly 0% or 100% duty. Pattern 3's 5% floor is a level in between,
 * which would keep that clock running for the whole of a state that exists
 * because something is wrong and may therefore last a very long time. Steady
 * is free for the same reason, and schedules nothing at all.
 */
static struct pattern_sample split_status_sample(int64_t elapsed_ms) {
    static const struct pattern_segment waiting[] = {{500, 100}, {1000, 0}};

    if (split_stage == SPLIT_STAGE_LINKED) {
        return (struct pattern_sample){.brightness = 100, .hold_ms = 0};
    }

    return SEGMENT_SAMPLE(waiting, elapsed_ms);
}
#endif

static struct pattern_sample triangular_sample(int64_t elapsed_ms) {
    const uint32_t phase = (uint32_t)(elapsed_ms % 2000);
    const uint32_t rising = phase < 1000 ? phase : 2000 - phase;

    return (struct pattern_sample){.brightness = 10 + (uint8_t)((rising * 90) / 1000),
                                   .hold_ms = ramp_hold(phase, phase < 1000 ? 1000 : 2000)};
}

static struct pattern_sample heartbeat_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {
        {100, 100}, {200, 8}, {300, 65}, {1200, 5},
    };

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample blink_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {{500, 100}, {1000, 5}};

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample fast_blink_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {{150, 100}, {300, 4}};

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample triple_flash_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {
        {100, 100}, {200, 4}, {300, 100}, {400, 4}, {500, 100}, {1500, 4},
    };

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample sos_sample(int64_t elapsed_ms) {
    /* Morse SOS: ... --- ... ; a unit is 100 ms and one frame is 2.8 s. The
     * seven trailing units are the gap before the message repeats. */
    static const uint8_t units[] = {
        100, 3, 100, 3, 100, 3, 3, 3, 100, 3, 100, 3, 100, 3,
        3,   3, 100, 3, 100, 3, 100, 3, 3,  3, 3,   3, 3,   3,
    };

    return TABLE_SAMPLE(units, 100, elapsed_ms);
}

static struct pattern_sample candle_sample(int64_t elapsed_ms) {
    /* A repeatable, non-random flicker sequence avoids adding an RNG dependency. */
    static const uint8_t levels[] = {55, 74, 63, 89, 69, 48, 82, 59, 95, 66, 76, 43};

    return TABLE_SAMPLE(levels, 90, elapsed_ms);
}

static struct pattern_sample sawtooth_sample(int64_t elapsed_ms) {
    const uint32_t phase = (uint32_t)(elapsed_ms % 1800);

    return (struct pattern_sample){.brightness = 5 + (uint8_t)((phase * 95) / 1800),
                                   .hold_ms = ramp_hold(phase, 1800)};
}

static struct pattern_sample beacon_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {{80, 100}, {200, 35}, {2200, 3}};

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample slow_breathe_sample(int64_t elapsed_ms) {
    const uint32_t phase = (uint32_t)(elapsed_ms % 16000);

    if (phase < 3000) {
        return (struct pattern_sample){.brightness = (uint8_t)((phase * 100) / 3000),
                                       .hold_ms = ramp_hold(phase, 3000)};
    }
    if (phase < 6000) {
        return (struct pattern_sample){
            .brightness = 100 - (uint8_t)(((phase - 3000) * 100) / 3000),
            .hold_ms = ramp_hold(phase, 6000)};
    }

    /* Leave the LED completely off for ten seconds before the next breath --
     * one sleep, which is the whole point of scheduling by deadline, and a
     * zero duty, which lets the PWM peripheral stop for all ten of them. */
    return (struct pattern_sample){.brightness = 0, .hold_ms = 16000 - phase};
}

static struct pattern_sample strobe_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {{70, 100}, {200, 3}};

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample double_beacon_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {
        {70, 100}, {180, 3}, {250, 100}, {1600, 3},
    };

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample long_flash_sample(int64_t elapsed_ms) {
    static const struct pattern_segment segments[] = {{800, 100}, {1200, 3}};

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample sparkle_sample(int64_t elapsed_ms) {
    static const uint8_t levels[] = {4, 12, 4, 84, 4, 38, 4, 100, 4, 18, 4, 62, 4, 4};

    return TABLE_SAMPLE(levels, 75, elapsed_ms);
}

static struct pattern_sample countdown_sample(int64_t elapsed_ms) {
    /* Four pulses, then a deliberately long dark pause before the repeat. */
    static const struct pattern_segment segments[] = {
        {80, 100}, {180, 3}, {260, 100}, {360, 3}, {440, 100}, {540, 3}, {620, 100}, {2500, 3},
    };

    return SEGMENT_SAMPLE(segments, elapsed_ms);
}

static struct pattern_sample ripple_sample(int64_t elapsed_ms) {
    static const uint8_t levels[] = {100, 65, 32, 12, 32, 65, 100, 65, 32, 12, 6, 3};

    return TABLE_SAMPLE(levels, 110, elapsed_ms);
}

static struct pattern_sample fade_blink_sample(int64_t elapsed_ms) {
    const uint32_t phase = (uint32_t)(elapsed_ms % 1200);

    if (phase < 150) {
        return (struct pattern_sample){.brightness = 100 - (uint8_t)((phase * 60) / 150),
                                       .hold_ms = ramp_hold(phase, 150)};
    }
    if (phase < 300) {
        return (struct pattern_sample){.brightness = 40 - (uint8_t)(((phase - 150) * 35) / 150),
                                       .hold_ms = ramp_hold(phase, 300)};
    }

    return (struct pattern_sample){.brightness = 3, .hold_ms = 1200 - phase};
}

static struct pattern_sample pattern_sample_at(uint8_t pattern, int64_t elapsed_ms) {
    switch (pattern) {
    case LED_PATTERN_BREATHE:
        return triangular_sample(elapsed_ms);
    case LED_PATTERN_HEARTBEAT:
        return heartbeat_sample(elapsed_ms);
    case LED_PATTERN_BLINK:
        return blink_sample(elapsed_ms);
    case LED_PATTERN_FAST_BLINK:
        return fast_blink_sample(elapsed_ms);
    case LED_PATTERN_TRIPLE_FLASH:
        return triple_flash_sample(elapsed_ms);
    case LED_PATTERN_SOS:
        return sos_sample(elapsed_ms);
    case LED_PATTERN_CANDLE:
        return candle_sample(elapsed_ms);
    case LED_PATTERN_SAWTOOTH:
        return sawtooth_sample(elapsed_ms);
    case LED_PATTERN_BEACON:
        return beacon_sample(elapsed_ms);
    case LED_PATTERN_SLOW_BREATHE:
        return slow_breathe_sample(elapsed_ms);
    case LED_PATTERN_STROBE:
        return strobe_sample(elapsed_ms);
    case LED_PATTERN_DOUBLE_BEACON:
        return double_beacon_sample(elapsed_ms);
    case LED_PATTERN_LONG_FLASH:
        return long_flash_sample(elapsed_ms);
    case LED_PATTERN_SPARKLE:
        return sparkle_sample(elapsed_ms);
    case LED_PATTERN_COUNTDOWN:
        return countdown_sample(elapsed_ms);
    case LED_PATTERN_RIPPLE:
        return ripple_sample(elapsed_ms);
    case LED_PATTERN_FADE_BLINK:
        return fade_blink_sample(elapsed_ms);
    case LED_PATTERN_STEADY:
    default:
        /* Nothing to schedule: one write, the work item goes quiet, and at
         * 100% duty the PWM peripheral stops too. */
        return (struct pattern_sample){.brightness = 100, .hold_ms = 0};
    }
}

/* ===== Output ===== */

static void write_led(uint8_t percent) {
    /* The work item is defined at compile time, so a redraw can in principle
     * be queued before the LED device has finished initialising; this behavior
     * and that device share an init priority, so their order is undefined. */
    if (!device_is_ready(backlight)) {
        return;
    }

    /* The selected board configuration has one PWM LED child (index 0). */
    (void)led_set_brightness(backlight, LED_INDEX, percent);
}

/*
 * The pattern says what shape and this behavior supplies its current ceiling.
 * It deliberately does not go through ZMK's backlight behavior, whose own
 * global split relay would send a second message for the same key press.
 */
static void set_led_brightness(uint8_t pattern_level) {
    write_led((pattern_level * brightness_percent) / 100);
}

/*
 * Speed is applied to the clock, not to each pattern.
 *
 * Every curve above is written against wall-clock milliseconds, so stretching
 * the time they are asked about is the one change that speeds all eighteen up
 * by the same factor and cannot get a single one of them wrong. The product
 * stays in int64_t, so even the fastest setting has far more uptime than the
 * hardware will ever see before it wraps.
 */
static int64_t pattern_clock(int64_t elapsed_ms) {
    return (elapsed_ms * speed_percent) / LED_PATTERN_SPEED_NOMINAL;
}

/* The inverse, for a hold: a curve that is being run at 400% reaches its next
 * edge in a quarter of the time it nominally takes. Never returns 0, so a
 * reschedule cannot turn into a busy loop at the top of the speed range. */
static uint32_t hold_to_wall_ms(uint32_t hold_ms) {
    const uint16_t speed = MAX(speed_percent, LED_PATTERN_SPEED_MIN);
    const uint64_t wall = ((uint64_t)hold_ms * LED_PATTERN_SPEED_NOMINAL) / speed;

    return (uint32_t)CLAMP(wall, 1, UINT32_MAX);
}

/* Never negative: the table lookups index by elapsed/step, so a start stamp in
 * the future -- which only a malformed relayed phase could produce -- would
 * otherwise index before the front of a level table. */
static int64_t elapsed_since(int64_t started_at) {
    return MAX(k_uptime_get() - started_at, (int64_t)0);
}

static void pattern_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (animation_suspended) {
        write_led(0);
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (split_status_showing()) {
        /*
         * The connection indicator is drawn at full brightness and nominal
         * speed, ignoring both settings. It is a diagnostic: it has to look
         * the same whatever the LED happens to be configured for, including
         * a brightness of zero.
         */
        const struct pattern_sample status =
            split_status_sample(elapsed_since(split_stage_started_at));

        write_led(status.brightness);
        if (status.hold_ms > 0) {
            schedule_pattern_work(K_MSEC(status.hold_ms));
        }
        return;
    }
#endif

    const int64_t elapsed_ms = elapsed_since(pattern_started_at);
    struct pattern_sample sample;
    uint32_t delay_ms;

    if (advertising_blink) {
        /* The status blink keeps its own fixed rate. It says something about
         * the connection, so it has to stay recognisable whatever speed the
         * patterns happen to be set to -- which is also why it is sampled on
         * the wall clock and its hold is not scaled. */
        static const struct pattern_segment segments[] = {
            {ADVERTISING_BLINK_ON_MS, ADVERTISING_BLINK_BRIGHTNESS},
            {ADVERTISING_BLINK_PERIOD_MS, ADVERTISING_BLINK_OFF_BRIGHTNESS},
        };

        sample = SEGMENT_SAMPLE(segments, elapsed_ms);
        delay_ms = sample.hold_ms;
    } else {
        sample = pattern_sample_at(active_pattern, pattern_clock(elapsed_ms));
        delay_ms = sample.hold_ms == 0 ? 0 : hold_to_wall_ms(sample.hold_ms);
    }

    set_led_brightness(sample.brightness);

    /* The whole of the animation loop, and it is not a loop: the next wake-up
     * is the moment this level stops being right, and a level that never stops
     * being right is not scheduled at all. */
    if (delay_ms > 0) {
        schedule_pattern_work(K_MSEC(delay_ms));
    }
}

/*
 * Every redraw goes through the work item, and none of them runs inline.
 *
 * The LED is touched from more contexts than it looks: a key press, a state
 * relayed from the split central, a settings apply, an endpoint change, a
 * connection coming or going, and the animation's own next sample. Running the
 * handler on the caller's stack meant two of those could be inside
 * led_set_brightness() at once, and it meant the first one could land before
 * the PWM LED device had finished initialising -- this behavior and the device
 * share an init priority, so their order is not defined. Rescheduling for
 * K_NO_WAIT keeps all of it on the one queue, in order, a few microseconds
 * later.
 */
static void refresh_pattern_output(void) {
    /* A stored value can arrive before the behavior has been initialised:
     * settings_load() runs on the main thread and the settings listener that
     * applies a value is free to fire first. Dropping the redraw is right --
     * init redraws anyway -- while touching an uninitialised k_work is not. */
    if (!controller_ready) {
        return;
    }

    schedule_pattern_work(K_NO_WAIT);
}

/*
 * The animation is the only thing here that would keep the core awake.
 *
 * ZMK already tracks whether anyone is using the keyboard, so the cheapest
 * possible policy is to follow it: at CONFIG_ZMK_IDLE_TIMEOUT after the last
 * key press the LED goes dark -- a zero duty, so the PWM peripheral stops as
 * well -- and nothing is scheduled at all until the next press.
 */
static int led_pattern_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);

    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const bool suspend = event->state != ZMK_ACTIVITY_ACTIVE;
    if (suspend == animation_suspended) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    animation_suspended = suspend;
    if (!suspend) {
        pattern_started_at = k_uptime_get();
    }
    refresh_pattern_output();

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_activity, led_pattern_activity_listener);
ZMK_SUBSCRIPTION(led_pattern_activity, zmk_activity_state_changed);

/* ===== Public state ===== */

/* Which of the two indicators, if either, is standing in front of the pattern
 * right now. Only used to make the log line say so. */
static const char *masking_indicator(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (split_status_showing()) {
        return " (masked by the split connection indicator)";
    }
#endif
    return advertising_blink ? " (masked by the advertising indicator)" : "";
}

void led_pattern_get_state(struct led_pattern_state *out) {
    out->pattern = active_pattern;
    out->speed = speed_percent;
    out->advertising_indicator = advertising_indicator;
}

uint8_t led_pattern_get_brightness(void) { return brightness_percent; }

void led_pattern_set_brightness(uint8_t brightness) {
    brightness_percent = MIN(100, brightness);
    refresh_pattern_output();
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
}

void led_pattern_set_state(const struct led_pattern_state *state) {
    const uint8_t pattern =
        state->pattern < LED_PATTERN_COUNT ? state->pattern : LED_PATTERN_STEADY;
    const bool pattern_changed = pattern != active_pattern;

    active_pattern = pattern;
    speed_percent = CLAMP(state->speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    advertising_indicator = state->advertising_indicator;

    if (pattern_changed) {
        /* Asking for a pattern is asking to see it, so it wins over the status
         * blink until the connection state next changes. Switching the
         * indicator off in the settings makes that permanent. */
        advertising_blink = false;
        pattern_started_at = k_uptime_get();
    } else if (!advertising_indicator) {
        advertising_blink = false;
    }

    /* The one line that says whether a pattern that was asked for is actually
     * the thing being drawn. Both the split connection indicator and the
     * advertising blink override the pattern, and that is the difference
     * between "the setting did nothing" and "the setting worked and you are
     * looking at an indicator". */
    LOG_INF("led: state pattern %u speed %u brightness %u%%%s", active_pattern, speed_percent,
            brightness_percent, masking_indicator());

    refresh_pattern_output();
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
}

/* A BLE peripheral has no host endpoint or active host profile. It still
 * executes the same GLOBAL behavior locally, but endpoint state belongs only
 * to the central (or to a non-split build). */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool should_show_advertising_blink(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    const struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();

    return advertising_indicator && endpoint.transport == ZMK_TRANSPORT_NONE &&
           !zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

static void refresh_connection_state(void) {
    advertising_blink = should_show_advertising_blink();
    pattern_started_at = k_uptime_get();
    refresh_pattern_output();
    schedule_mirror(K_MSEC(MIRROR_DEBOUNCE_MS));
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int ble_profile_changed_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    if (!controller_ready) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    refresh_connection_state();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_ble, ble_profile_changed_listener);
ZMK_SUBSCRIPTION(led_pattern_ble, zmk_ble_active_profile_changed);
#endif

static int led_pattern_endpoint_listener(const zmk_event_t *eh) {
    if (!controller_ready) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_endpoint_changed(eh)) {
        refresh_connection_state();
        return ZMK_EV_EVENT_BUBBLE;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_endpoint, led_pattern_endpoint_listener);
ZMK_SUBSCRIPTION(led_pattern_endpoint, zmk_endpoint_changed);
#endif

/* ===== Behavior ===== */

/*
 * One parameter, three bands, one resulting state.
 *
 * Every band is decoded into a whole state and applied through the one setter,
 * so a keymap binding, a relayed absolute command and a value edited in a
 * client cannot drift apart: there is a single place that clamps, a single
 * place that decides whether the animation restarts, and a single place that
 * reports the result.
 */
static bool decode_command(uint32_t param, struct led_pattern_state *state) {
    if (param < LED_PATTERN_COUNT) {
        state->pattern = (uint8_t)param;
        return true;
    }

    switch (param) {
    case LED_PATTERN_PREVIOUS:
        state->pattern = (state->pattern + LED_PATTERN_COUNT - 1) % LED_PATTERN_COUNT;
        return true;
    case LED_PATTERN_NEXT:
        state->pattern = (state->pattern + 1) % LED_PATTERN_COUNT;
        return true;
    case LED_PATTERN_BRIGHTNESS_UP:
        brightness_percent = MIN(100, brightness_percent + 10);
        return true;
    case LED_PATTERN_BRIGHTNESS_DOWN:
        brightness_percent = brightness_percent < 10 ? 0 : brightness_percent - 10;
        return true;
    default:
        break;
    }

    if (param >= LED_PATTERN_SPEED(LED_PATTERN_SPEED_MIN) &&
        param <= LED_PATTERN_SPEED(LED_PATTERN_SPEED_MAX)) {
        state->speed = (uint16_t)(param - LED_PATTERN_SPEED_BASE);
        return true;
    }

    return false;
}

static int on_pattern_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (!device_is_ready(backlight)) {
        return -ENODEV;
    }

    struct led_pattern_state state;

    led_pattern_get_state(&state);
    if (decode_command(binding->param1, &state)) {
        led_pattern_set_state(&state);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_pattern_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/*
 * Without this a Studio client refuses the behavior outright.
 *
 * zmk_behavior_check_params_match_metadata() treats a behavior that publishes
 * no metadata as one that takes no parameter at all, and rejects every binding
 * whose param1 is not zero -- which is every useful binding of this one. So
 * the list below is what makes &led_pattern selectable in a keymap editor,
 * and its display names are what a person picks from there.
 */
#define LED_PATTERN_NAMED_VALUE(name, param)                                                       \
    {.display_name = name, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = param}

static const struct behavior_parameter_value_metadata param1_values[] = {
    LED_PATTERN_NAMED_VALUE("Steady", LED_PATTERN_STEADY),
    LED_PATTERN_NAMED_VALUE("Breathe", LED_PATTERN_BREATHE),
    LED_PATTERN_NAMED_VALUE("Heartbeat", LED_PATTERN_HEARTBEAT),
    LED_PATTERN_NAMED_VALUE("Blink", LED_PATTERN_BLINK),
    LED_PATTERN_NAMED_VALUE("Fast blink", LED_PATTERN_FAST_BLINK),
    LED_PATTERN_NAMED_VALUE("Triple flash", LED_PATTERN_TRIPLE_FLASH),
    LED_PATTERN_NAMED_VALUE("SOS", LED_PATTERN_SOS),
    LED_PATTERN_NAMED_VALUE("Candle", LED_PATTERN_CANDLE),
    LED_PATTERN_NAMED_VALUE("Sawtooth", LED_PATTERN_SAWTOOTH),
    LED_PATTERN_NAMED_VALUE("Beacon", LED_PATTERN_BEACON),
    LED_PATTERN_NAMED_VALUE("Slow breathe", LED_PATTERN_SLOW_BREATHE),
    LED_PATTERN_NAMED_VALUE("Strobe", LED_PATTERN_STROBE),
    LED_PATTERN_NAMED_VALUE("Double beacon", LED_PATTERN_DOUBLE_BEACON),
    LED_PATTERN_NAMED_VALUE("Long flash", LED_PATTERN_LONG_FLASH),
    LED_PATTERN_NAMED_VALUE("Sparkle", LED_PATTERN_SPARKLE),
    LED_PATTERN_NAMED_VALUE("Countdown", LED_PATTERN_COUNTDOWN),
    LED_PATTERN_NAMED_VALUE("Ripple", LED_PATTERN_RIPPLE),
    LED_PATTERN_NAMED_VALUE("Fade blink", LED_PATTERN_FADE_BLINK),
    LED_PATTERN_NAMED_VALUE("Previous pattern", LED_PATTERN_PREVIOUS),
    LED_PATTERN_NAMED_VALUE("Next pattern", LED_PATTERN_NEXT),
    LED_PATTERN_NAMED_VALUE("Brightness up", LED_PATTERN_BRIGHTNESS_UP),
    LED_PATTERN_NAMED_VALUE("Brightness down", LED_PATTERN_BRIGHTNESS_DOWN),
    {
        .display_name = "Set speed (400 + percent)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = LED_PATTERN_SPEED(LED_PATTERN_SPEED_MIN),
                  .max = LED_PATTERN_SPEED(LED_PATTERN_SPEED_MAX)},
    },
};

static const struct behavior_parameter_metadata_set metadata_set = {
    .param1_values = param1_values,
    .param1_values_len = ARRAY_SIZE(param1_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_led_pattern_driver_api = {
    .binding_pressed = on_pattern_pressed,
    .binding_released = on_pattern_released,
    /*
     * The central owns the state. Standard ZMK split already forwards a
     * peripheral's key positions to it, so binding this behavior on either
     * half works, and the peripheral learns the result from the mirror message
     * rather than by running the behavior a second time locally -- which a
     * GLOBAL locality would do, and which would decide the next pattern twice
     * from two different starting points.
     */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int behavior_led_pattern_init(const struct device *dev) {
    ARG_UNUSED(dev);
    controller_ready = true;
    animation_suspended = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* Both halves come up on the waiting indicator, and neither leaves it
     * until the state exchange has actually been acknowledged. */
    split_stage_started_at = k_uptime_get();
#endif
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    refresh_connection_state();
#else
    refresh_pattern_output();
#endif
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_led_pattern_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_APPLICATION_INIT_PRIORITY, &behavior_led_pattern_driver_api);
