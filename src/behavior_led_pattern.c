/*
 * Copyright (c) 2025-2026 amgskobo
 * SPDX-License-Identifier: MIT
 *
 * Reusable external LED pattern controller.
 */

#define DT_DRV_COMPAT zmk_behavior_led_pattern

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk-led-patterns/events/state_changed.h>
#include <zmk-led-patterns/led_pattern.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include "led_scheduled_sync.h"
#endif
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

#ifndef CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL
#define CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_led_patterns_behavior_led_pattern, CONFIG_ZMK_LED_PATTERNS_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_led_pattern_state_changed);

#define LED_INDEX 0

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "declare exactly one zmk,behavior-led-pattern node");

/*
 * Curves report how long their current level can be held. Step sections wake
 * at their next edge; the long dark section of Slow breathe sleeps throughout.
 * Ramps are sampled every 40 ms (25 steps a second), so their fades remain
 * smooth without waking for the sections whose output is constant.
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

static const struct device *const backlight = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));

static void pattern_work_handler(struct k_work *work);

/* Defined at compile time rather than initialised from this behavior's init,
 * so that nothing can submit it before it is usable: an init that runs after
 * the first submit would overwrite the queue's own list node inside an item
 * already on the list. */
static K_WORK_DELAYABLE_DEFINE(pattern_work, pattern_work_handler);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT)
/* Standalone presentation state. Split desired/rendered state belongs to the
 * scheduled adapter and is never stored a second time here. */
static uint8_t active_pattern = LED_PATTERN_BREATHE;
static uint16_t speed_percent = LED_PATTERN_SPEED_NOMINAL;
/* Kept here rather than delegated to ZMK's backlight subsystem: `&bl` carries
 * a global split relay of its own, and driving both would put two senders on
 * the split link for a single key press. */
static uint8_t brightness_percent = 100;
static bool advertising_indicator = true;
/* Whether idle darkens the LED. The default is the cheap one. */
static bool idle_off = true;
static int64_t pattern_started_at;
static bool advertising_blink;
/* Set while the animation is being held dark: ZMK reports the keyboard idle
 * and idle_off says to follow it. */
static bool animation_suspended;
#endif
static bool controller_ready;
/* This half's own view of ZMK's activity state, so the two inputs to that
 * decision can change independently -- an idle timeout, and a setting edited
 * while the keyboard is already idle. */
static enum zmk_activity_state activity_state = ZMK_ACTIVITY_ACTIVE;
/* Set while the keyboard powers off (the power-management suspend at the end
 * of this file) and cleared if that power-off is abandoned. Nothing is drawn
 * or mirrored while it is set: the LED has been written dark, and a later
 * write would be latched into the pin for as long as the SoC stays off. */
static atomic_t powering_off;
static atomic_t applied_brightness;

/*
 * Standalone drawing and relay TX use ZMK's low-priority queue. Split drawing
 * and reserved applies share a dedicated queue at the same low priority.
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
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    k_work_reschedule_for_queue(led_scheduled_render_queue(), &pattern_work, delay);
#else
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &pattern_work, delay);
#endif
}

/* Never negative: table lookups index by elapsed/step, so a future start
 * stamp must not index before the front of a level table. */
static int64_t elapsed_since(int64_t started_at) {
    return MAX(k_uptime_get() - started_at, (int64_t)0);
}

static void refresh_pattern_output(void);
static void apply_activity(void);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool should_show_advertising_blink(void);
#endif

/* Split timing, relay and session state live in led_scheduled_sync.c. */
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
    size_t i = 0;

    /* Ends by the last entry at the latest: phase is below the period, which
     * is that entry's boundary. */
    while (phase >= segments[i].until_ms) {
        i++;
    }

    return (struct pattern_sample){.brightness = segments[i].brightness,
                                   .hold_ms = segments[i].until_ms - phase};
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
static struct pattern_sample split_status_sample(enum led_sync_stage stage, int64_t elapsed_ms) {
    static const struct pattern_segment waiting[] = {{500, 100}, {1000, 0}};
    if (stage == LED_SYNC_LINKED) {
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
        {100, 100},
        {200, 8},
        {300, 65},
        {1200, 5},
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
    /* Morse SOS: ... --- ... ; a unit is 100 ms and one frame is 3.4 s. The
     * seven trailing units are the gap before the message repeats. */
    static const struct pattern_segment segments[] = {
        {100, 100},  {200, 3},  {300, 100},  {400, 3},  {500, 100},  {800, 3},
        {1100, 100}, {1200, 3}, {1500, 100}, {1600, 3}, {1900, 100}, {2200, 3},
        {2300, 100}, {2400, 3}, {2500, 100}, {2600, 3}, {2700, 100}, {3400, 3},
    };

    return SEGMENT_SAMPLE(segments, elapsed_ms);
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
        return (struct pattern_sample){.brightness = 100 - (uint8_t)(((phase - 3000) * 100) / 3000),
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
        {70, 100},
        {180, 3},
        {250, 100},
        {1600, 3},
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

static bool write_led(uint8_t percent) {
    if (!device_is_ready(backlight))
        return false;
    if (led_set_brightness(backlight, LED_INDEX, percent) != 0)
        return false;
    atomic_set(&applied_brightness, percent);
    return true;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT)
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
#endif

static void pattern_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_get(&powering_off))
        return;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct led_scheduled_view view;
    led_scheduled_render_begin(&view); /* Commit due reservation before any old animation draw. */
    if (!view.normal) {
        if ((view.values.flags & (LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE)) == LED_SYNC_IDLE_OFF) {
            write_led(0);
            return;
        }
        const struct pattern_sample status =
            split_status_sample(view.stage, elapsed_since(view.status_started_at));
        write_led(status.brightness);
        if (status.hold_ms)
            schedule_pattern_work(K_MSEC(status.hold_ms));
        return;
    }
    const bool suspended =
        (view.values.flags & (LED_SYNC_IDLE_OFF | LED_SYNC_ACTIVE)) == LED_SYNC_IDLE_OFF;
    const uint8_t brightness = view.values.brightness;
    if (suspended || brightness == 0) {
        bool ok = write_led(0);
        led_scheduled_render_end(view.applied_sequence, ok);
        return;
    }
    const uint64_t elapsed_ms = view.elapsed_ms;
    const bool advertising = (view.values.flags & LED_SYNC_ADVERTISING) != 0;
#else
    if (animation_suspended) {
        write_led(0);
        return;
    }
    if (brightness_percent == 0) {
        write_led(0);
        return;
    }
    const int64_t elapsed_ms = elapsed_since(pattern_started_at);
    const bool advertising = advertising_blink;
#endif
    struct pattern_sample sample;
    uint32_t delay_ms;
    if (advertising) {
        static const struct pattern_segment segments[] = {
            {ADVERTISING_BLINK_ON_MS, ADVERTISING_BLINK_BRIGHTNESS},
            {ADVERTISING_BLINK_PERIOD_MS, ADVERTISING_BLINK_OFF_BRIGHTNESS},
        };
        sample = SEGMENT_SAMPLE(segments, elapsed_ms);
        delay_ms = sample.hold_ms;
    } else {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        /* 2827440000 is the LCM of all18 curve periods. Multiplying its
         * 100x wall-clock repeat by any supported speed fits int64_t and
         * preserves the exact floor((elapsed*speed)/100) curve phase. */
        const int64_t scaled =
            (int64_t)((elapsed_ms % UINT64_C(282744000000)) * view.values.speed / 100);
        sample = pattern_sample_at(view.values.pattern, scaled);
        delay_ms = sample.hold_ms
                       ? MAX(1U, (uint32_t)((uint64_t)sample.hold_ms * 100 / view.values.speed))
                       : 0;
#else
        sample = pattern_sample_at(active_pattern, pattern_clock(elapsed_ms));
        delay_ms = sample.hold_ms == 0 ? 0 : hold_to_wall_ms(sample.hold_ms);
#endif
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    bool ok = write_led((sample.brightness * brightness) / 100);
    led_scheduled_render_end(view.applied_sequence, ok);
#else
    set_led_brightness(sample.brightness);
#endif
    if (delay_ms > 0)
        schedule_pattern_work(K_MSEC(delay_ms));
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
    if (!controller_ready || atomic_get(&powering_off)) {
        return;
    }

    schedule_pattern_work(K_NO_WAIT);
}

/*
 * The animation is the only thing here that would keep the core awake.
 *
 * ZMK already tracks whether anyone is using the keyboard, so the cheap policy
 * is to follow it: at CONFIG_ZMK_IDLE_TIMEOUT after the last key press the LED
 * goes dark -- a zero duty, so the PWM peripheral stops as well -- and nothing
 * is scheduled at all until the next press. Clearing led.idle_off buys the
 * other policy, and it is the one thing in this module that really does cost
 * battery rather than merely looking as though it might.
 *
 * Both inputs are re-read here rather than latched, because either can move on
 * its own: the timeout fires, or the setting is edited while the keyboard is
 * already idle and the LED has to come back on without waiting for a key.
 */
static void apply_activity(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    led_scheduled_activity(activity_state == ZMK_ACTIVITY_ACTIVE);
#else
    const bool suspend = idle_off && activity_state != ZMK_ACTIVITY_ACTIVE;
    if (suspend == animation_suspended)
        return;
    animation_suspended = suspend;
    if (!suspend)
        pattern_started_at = k_uptime_get();
    refresh_pattern_output();
#endif
}

static int led_pattern_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);

    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    activity_state = event->state;
    apply_activity();

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_pattern_activity, led_pattern_activity_listener);
ZMK_SUBSCRIPTION(led_pattern_activity, zmk_activity_state_changed);

/* ===== Public state ===== */

/* Which of the two indicators, if either, is standing in front of the pattern
 * right now. Only used to make the log line say so. */
static const char *masking_indicator(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    return " (scheduled split presentation)";
#else
    return advertising_blink ? " (masked by the advertising indicator)" : "";
#endif
}

void led_pattern_get_state(struct led_pattern_state *out) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct led_sync_values desired;
    led_scheduled_get_desired(&desired);
    *out = (struct led_pattern_state){.pattern = desired.pattern,
                                      .speed = desired.speed,
                                      .advertising_indicator =
                                          (desired.flags & LED_SYNC_INDICATOR) != 0,
                                      .idle_off = (desired.flags & LED_SYNC_IDLE_OFF) != 0};
#else
    out->pattern = active_pattern;
    out->speed = speed_percent;
    out->advertising_indicator = advertising_indicator;
    out->idle_off = idle_off;
#endif
}

uint8_t led_pattern_get_brightness(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct led_sync_values desired;
    led_scheduled_get_desired(&desired);
    return desired.brightness;
#else
    return brightness_percent;
#endif
}
uint8_t led_pattern_applied_brightness(void) { return (uint8_t)atomic_get(&applied_brightness); }

static void notify_state_changed(void) {
    if (controller_ready) {
        struct led_pattern_state state;
        led_pattern_get_state(&state);
        raise_zmk_led_pattern_state_changed(
            ((struct zmk_led_pattern_state_changed){.pattern = state.pattern,
                                                    .brightness = led_pattern_get_brightness(),
                                                    .speed = state.speed}));
    }
}

void led_pattern_set_brightness(uint8_t brightness) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (led_scheduled_set_brightness(brightness))
        notify_state_changed();
#else
    const uint8_t next = MIN(100, brightness);
    if (next == brightness_percent) {
        return;
    }
    brightness_percent = next;
    refresh_pattern_output();
    notify_state_changed();
#endif
}

static void set_presentation(const struct led_pattern_state *state, bool replace_brightness,
                             uint8_t brightness) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    const bool changed = replace_brightness
                             ? led_scheduled_set_presentation(state, brightness)
                             : led_scheduled_set_state(state);
    if (changed)
        notify_state_changed();
    LOG_INF("led: desired state pattern %u speed %u brightness %u%%%s",
            led_pattern_field_value(LED_PATTERN_FIELD_PATTERN),
            led_pattern_field_value(LED_PATTERN_FIELD_SPEED), led_pattern_get_brightness(),
            masking_indicator());
#else
    const uint8_t pattern =
        state->pattern < LED_PATTERN_COUNT ? state->pattern : LED_PATTERN_STEADY;
    const bool pattern_changed = pattern != active_pattern;
    const uint16_t speed = CLAMP(state->speed, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    const uint8_t next_brightness = replace_brightness ? MIN(100, brightness) : brightness_percent;
    const bool shown_changed = pattern_changed || speed != speed_percent ||
                               next_brightness != brightness_percent;

    brightness_percent = next_brightness;
    active_pattern = pattern;
    speed_percent = speed;
    advertising_indicator = state->advertising_indicator;
    idle_off = state->idle_off;
    /* Before the log line and the redraw: turning idle_off off while the
     * keyboard is already idle has to light the LED now, not at the next key
     * press. */
    apply_activity();

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
    if (shown_changed) {
        notify_state_changed();
    }
#endif
}

void led_pattern_set_state(const struct led_pattern_state *state) {
    set_presentation(state, false, 0);
}

void led_pattern_set_presentation(const struct led_pattern_state *state, uint8_t brightness) {
    set_presentation(state, true, brightness);
}

/* A BLE peripheral renders scheduled presentations without a host endpoint
 * or active host profile. Host endpoint indication belongs to the central
 * (or to a non-split build); behavior intents use central locality. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static bool should_show_advertising_blink(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    const struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();
    struct led_pattern_state state;
    led_pattern_get_state(&state);
    return state.advertising_indicator && endpoint.transport == ZMK_TRANSPORT_NONE &&
           !zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

static void refresh_connection_state(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    led_scheduled_endpoint(should_show_advertising_blink());
#else
    advertising_blink = should_show_advertising_blink();
    pattern_started_at = k_uptime_get();
    refresh_pattern_output();
#endif
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

/* ===== Requests from the behaviors ===== */

bool led_pattern_ready(void) { return device_is_ready(backlight); }

int32_t led_pattern_field_value(enum led_pattern_field field) {
    struct led_pattern_state state;
    led_pattern_get_state(&state);
    switch (field) {
    case LED_PATTERN_FIELD_PATTERN:
        return state.pattern;
    case LED_PATTERN_FIELD_BRIGHTNESS:
        return led_pattern_get_brightness();
    case LED_PATTERN_FIELD_SPEED:
        return state.speed;
    default:
        return 0;
    }
}

/* Straight onto the LED: for a build with no settings to own the value, and
 * for the rare write the settings refuse. */
static void apply_request(enum led_pattern_field field, int32_t value) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (led_scheduled_request(field, value))
        notify_state_changed();
#else
    struct led_pattern_state state;

    if (field == LED_PATTERN_FIELD_BRIGHTNESS) {
        led_pattern_set_brightness((uint8_t)CLAMP(value, 0, 100));
        return;
    }

    led_pattern_get_state(&state);
    if (field == LED_PATTERN_FIELD_PATTERN) {
        state.pattern = (uint8_t)CLAMP(value, 0, LED_PATTERN_COUNT - 1);
    } else {
        state.speed = (uint16_t)CLAMP(value, LED_PATTERN_SPEED_MIN, LED_PATTERN_SPEED_MAX);
    }
    led_pattern_set_state(&state);
#endif
}

void led_pattern_request(enum led_pattern_field field, int32_t value) {
    if (field != LED_PATTERN_FIELD_BRIGHTNESS && field != LED_PATTERN_FIELD_PATTERN &&
        field != LED_PATTERN_FIELD_SPEED)
        return;
#if IS_ENABLED(CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS)
    /* The setting is the owner: its change event is what reaches the LED. */
    if (led_pattern_settings_store(field, value) == 0) {
        return;
    }
#endif
    apply_request(field, value);
}

/* ===== &led_pattern ===== */

/*
 * A pattern number, or a step through them. Brightness and speed are behaviors
 * of their own, so every binding names the one setting it changes. A step
 * starts from what the LED is showing, which a settings write has already
 * updated by the time the next press arrives.
 */
static int on_pattern_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (!led_pattern_ready()) {
        return -ENODEV;
    }

    const int32_t current = led_pattern_field_value(LED_PATTERN_FIELD_PATTERN);
    int32_t pattern;

    if (binding->param1 < LED_PATTERN_COUNT) {
        pattern = (int32_t)binding->param1;
    } else if (binding->param1 == LED_PATTERN_PREVIOUS) {
        pattern = (current + LED_PATTERN_COUNT - 1) % LED_PATTERN_COUNT;
    } else if (binding->param1 == LED_PATTERN_NEXT) {
        pattern = (current + 1) % LED_PATTERN_COUNT;
    } else {
        LOG_WRN("led: &led_pattern has no command %u", binding->param1);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    led_pattern_request(LED_PATTERN_FIELD_PATTERN, pattern);
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
    activity_state = zmk_activity_get_state();
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    led_scheduled_start(refresh_pattern_output);
#else
    animation_suspended = idle_off && activity_state != ZMK_ACTIVITY_ACTIVE;
#endif
    controller_ready = true;
    apply_activity();
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    refresh_connection_state();
#endif
    refresh_pattern_output();
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_led_pattern_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_APPLICATION_INIT_PRIORITY, &behavior_led_pattern_driver_api);

#if IS_ENABLED(CONFIG_PM_DEVICE)

#if defined(CONFIG_PWM_INIT_PRIORITY)
BUILD_ASSERT(CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY < CONFIG_PWM_INIT_PRIORITY,
             "the first power-off device must come before the PWM driver in init order");
#endif
BUILD_ASSERT(CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY > CONFIG_LED_INIT_PRIORITY,
             "the second power-off device must come after the LED device in init order");

/*
 * The LED through a power-off.
 *
 * ZMK powers off along two paths and neither raises an event: &soft_off
 * suspends every device in init order, idle sleep suspends them in the
 * reverse order. A pin keeps its level for as long as the SoC is off, and the
 * LED drivers' own suspend only lets go of the pin: an LED lit at that moment
 * stayed lit through soft-off, on both halves of a split. So the LED is
 * written dark here, while its drivers still work, and nothing is drawn or
 * mirrored until a resume says the power-off was abandoned. Two devices sit
 * either side of those drivers in init order, so on either path one of them
 * runs first; the other finds the work stopped and the LED already dark.
 */
static int led_pattern_pm_action(const struct device *dev, enum pm_device_action action) {
    ARG_UNUSED(dev);

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND: {
        struct k_work_sync sync;

        if (atomic_set(&powering_off, 1) != 0) {
            return 0;
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        led_scheduled_pm_prepare(); /* Fence RX/TX before any potentially blocked driver call. */
#endif
        k_work_cancel_delayable_sync(&pattern_work, &sync);
        /* Darken before waiting for a potentially blocked transport worker. */
        write_led(0);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        led_scheduled_pm(true);
#endif
        /* Driven to zero rather than released: a pin let go while high
         * leaves a transistor's gate charged, and the LED behind it lit. */
        return 0;
    }
    case PM_DEVICE_ACTION_RESUME:
        if (atomic_clear(&powering_off) != 0) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            led_scheduled_pm(false);
#endif
            refresh_pattern_output();
        }
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int led_pattern_pm_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

PM_DEVICE_DEFINE(led_pattern_pm, led_pattern_pm_action);

DEVICE_DEFINE(led_pattern_pm, "led_pattern_pm", led_pattern_pm_init, PM_DEVICE_GET(led_pattern_pm),
              NULL, NULL, POST_KERNEL, CONFIG_ZMK_LED_PATTERNS_PM_INIT_PRIORITY, NULL);

PM_DEVICE_DEFINE(led_pattern_pm_late, led_pattern_pm_action);

DEVICE_DEFINE(led_pattern_pm_late, "led_pattern_pm_late", led_pattern_pm_init,
              PM_DEVICE_GET(led_pattern_pm_late), NULL, NULL, POST_KERNEL,
              CONFIG_ZMK_LED_PATTERNS_PM_LATE_INIT_PRIORITY, NULL);

#endif /* IS_ENABLED(CONFIG_PM_DEVICE) */
