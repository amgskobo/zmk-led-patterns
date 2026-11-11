/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 * LED-only logical clock and scheduled presentation protocol. No kernel clock is changed.
 */
#pragma once

#include "led_sync_clock.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LED_SYNC_VERSION 4
#define LED_SYNC_START_MS 5000U
#define LED_SYNC_RETRY_MS 500U
#define LED_SYNC_LEAD_MS 1000U
#define LED_SYNC_RECEIVE_LEAD_MS 100U
#define LED_SYNC_LATE_MS 40U
#define LED_SYNC_STATE_RETRY_MS 200U
#define LED_SYNC_CLOCK_ATTEMPTS 5U
#define LED_SYNC_STATE_ATTEMPTS 3U
#define LED_SYNC_PLAN_ATTEMPTS 3U
#define LED_SYNC_NEVER UINT64_MAX

enum led_sync_kind {
    LED_SYNC_PROBE = 1,
    LED_SYNC_REPLY,
    LED_SYNC_FIT,
    LED_SYNC_FIT_ACK,
    LED_SYNC_STATE,
    LED_SYNC_STATE_ACK,
    LED_SYNC_REQUEST,
};
enum led_sync_status {
    LED_SYNC_OK = 0,
    LED_SYNC_ACCEPTED,
    LED_SYNC_APPLIED,
    LED_SYNC_REJECT_LATE,
    LED_SYNC_REJECT_CLOCK,
    LED_SYNC_REJECT_RENDER,
};
enum led_sync_stage {
    LED_SYNC_WAITING,
    LED_SYNC_LINKED,
    LED_SYNC_RUNNING_UNCONFIRMED,
    LED_SYNC_SYNCED,
    LED_SYNC_COASTING,
    LED_SYNC_COASTING_UNCONFIRMED,
};
enum led_sync_flags {
    LED_SYNC_ADVERTISING = 1,
    LED_SYNC_IDLE_OFF = 2,
    LED_SYNC_ACTIVE = 4,
    LED_SYNC_INDICATOR = 8,
};

struct led_sync_values {
    uint8_t pattern, brightness;
    uint16_t speed;
    uint8_t flags;
};

/* Internal engine representation; led_sync_wire.h defines the explicit relay
 * encoding. Internal unused bytes remain zero and are validated. */
struct led_sync_state_wire {
    uint32_t clock_sequence;
    uint64_t apply_at, phase_epoch;
    uint8_t pattern, brightness;
    uint16_t speed;
    uint8_t flags;
} __attribute__((packed));
struct led_sync_packet {
    uint8_t source, version, kind, status;
    uint32_t central_boot, central_epoch, sequence, peer_boot, peer_epoch;
    union {
        struct {
            uint64_t t1, t2, t3, t4;
        } clock;
        struct {
            struct led_sync_state_wire state;
            uint64_t actual_local;
        } applied;
        uint8_t bytes[40];
    } data;
} __attribute__((packed));
_Static_assert(sizeof(struct led_sync_packet) == 64, "LED internal packet shape changed");

struct led_sync_plan {
    struct led_sync_clock_fit clock;
    uint64_t local_deadline;
    uint32_t sequence;
    struct led_sync_state_wire wire;
    bool valid, confirmed, reset_phase;
};
/* Only presentation rolls back: desired edits, fitted clock/session and retry
 * accounting may have changed while PWM ran outside the metadata lock. */
struct led_sync_saved_presentation {
    bool valid;
    struct led_sync_plan active;
    uint64_t phase_floor, last_logical, rendered_at;
};
struct led_sync_engine {
    struct led_sync_clock_fit fit, active_clock;
    struct led_sync_plan pending, active;
    struct led_sync_saved_presentation previous;
    uint64_t phase_epoch, phase_floor, clock_due, clock_timeout, state_due;
    uint64_t request_due, confirmation_due;
    uint64_t t1, t2, t3, t4, last_logical;
    uint64_t rendered_at;
    struct led_sync_packet reply, fit_ack, ack;
    uint32_t boot, epoch, peer_boot, peer_epoch, central_boot, central_epoch;
    uint32_t sequence, clock_sequence, candidate_sequence;
    uint32_t candidate_boot, candidate_epoch, candidate_peer_epoch;
    uint32_t challenge_counter, highest_central_epoch;
    uint32_t completed_sequence, hint_boot, hint_epoch, resume_after_sequence;
    uint32_t request_boot, request_epoch, retired_peer_boot[4];
    uint32_t quantization_ms;
    struct led_sync_values desired, visible, transaction_desired;
    bool central, linked, suspended, dirty, reset_phase, clock_ready;
    uint8_t clock_attempts, state_attempts, plan_attempts, request_attempts;
    bool transaction_active, exhausted, replay_previous, resume_pending;
    uint8_t retired_peer_next;
    enum led_sync_stage stage;
    bool have_active, reply_pending, fit_pending, fit_ack_pending, ack_pending;
    bool active_rendered;
    uint8_t active_failure;
};

static inline bool led_sync_values_equal(const struct led_sync_values *a,
                                         const struct led_sync_values *b) {
    return a->pattern == b->pattern && a->brightness == b->brightness && a->speed == b->speed &&
           a->flags == b->flags;
}
static inline bool led_sync_values_valid(const struct led_sync_values *v) {
    return v->pattern < 18 && v->brightness <= 100 && v->speed >= 10 && v->speed <= 400 &&
           (v->flags & ~15U) == 0;
}
static inline struct led_sync_values led_sync_wire_values(const struct led_sync_state_wire *w) {
    return (struct led_sync_values){w->pattern, w->brightness, w->speed, w->flags};
}
static inline bool led_sync_state_equal(const struct led_sync_state_wire *a,
                                        const struct led_sync_state_wire *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}
static inline bool led_sync_packet_shape(const struct led_sync_packet *p) {
    unsigned used;
    switch (p->kind) {
    case LED_SYNC_PROBE:
        used = 8;
        break;
    case LED_SYNC_REPLY:
        used = 24;
        break;
    case LED_SYNC_FIT:
    case LED_SYNC_FIT_ACK:
        used = 32;
        break;
    case LED_SYNC_STATE:
        used = sizeof(struct led_sync_state_wire);
        break;
    case LED_SYNC_STATE_ACK:
        used = sizeof(p->data.applied);
        break;
    case LED_SYNC_REQUEST:
        used = 0;
        break;
    default:
        return false;
    }
    if (p->kind != LED_SYNC_STATE_ACK && p->status != LED_SYNC_OK)
        return false;
    if (p->kind == LED_SYNC_STATE_ACK &&
        (p->status < LED_SYNC_ACCEPTED || p->status > LED_SYNC_REJECT_RENDER))
        return false;
    if (p->kind == LED_SYNC_STATE_ACK && p->status == LED_SYNC_ACCEPTED &&
        p->data.applied.actual_local != 0)
        return false;
    if (p->kind == LED_SYNC_STATE_ACK && p->data.applied.actual_local > INT64_MAX)
        return false;
    for (unsigned i = used; i < sizeof(p->data.bytes); i++)
        if (p->data.bytes[i] != 0)
            return false;
    return true;
}
static inline struct led_sync_packet led_sync_packet_new(const struct led_sync_engine *e,
                                                         uint8_t kind, uint32_t sequence) {
    struct led_sync_packet p = {0};
    p.source = 0xff;
    p.version = LED_SYNC_VERSION;
    p.kind = kind;
    p.central_boot = e->central ? e->boot : e->central_boot;
    p.central_epoch = e->central ? e->epoch : e->central_epoch;
    p.peer_boot = e->central ? e->peer_boot : e->boot;
    p.peer_epoch = e->central ? e->peer_epoch : e->epoch;
    p.sequence = sequence;
    return p;
}
static inline void led_sync_init(struct led_sync_engine *e, bool central, uint32_t boot,
                                 uint32_t quantization_ms, struct led_sync_values desired) {
    memset(e, 0, sizeof(*e));
    e->central = central;
    e->boot = boot ? boot : 1;
    e->quantization_ms = quantization_ms;
    e->desired = desired;
    e->transaction_desired = desired;
    e->dirty = e->reset_phase = true;
    e->clock_due = e->clock_timeout = e->state_due = LED_SYNC_NEVER;
    e->request_due = LED_SYNC_NEVER;
    e->confirmation_due = LED_SYNC_NEVER;
}

/* A link boundary always invalidates both the clock and any old reservation.
 * Boot nonce rollover is probabilistic identity, never authentication. */
static inline void led_sync_link(struct led_sync_engine *e, bool up, uint64_t now,
                                 uint32_t rollover_nonce) {
    if (e->challenge_counter > e->epoch)
        e->epoch = e->challenge_counter;
    if (++e->epoch == 0) {
        e->boot = rollover_nonce ? rollover_nonce : (e->boot ^ 0xa5a55a5aU);
        if (e->boot == 0)
            e->boot = 1;
        e->epoch = 1;
    }
    e->linked = up;
    e->clock_ready = e->have_active = false;
    e->active_rendered = false;
    e->active_failure = 0;
    e->pending.valid = e->active.valid = false;
    e->fit.valid = e->active_clock.valid = false;
    e->peer_boot = e->peer_epoch = e->central_boot = e->central_epoch = 0;
    e->candidate_sequence = e->clock_sequence = e->sequence = 0;
    e->candidate_boot = e->candidate_epoch = e->candidate_peer_epoch = 0;
    e->challenge_counter = e->epoch;
    e->highest_central_epoch = 0;
    e->clock_attempts = e->state_attempts = 0;
    e->plan_attempts = e->request_attempts = 0;
    e->transaction_active = up && e->central;
    e->transaction_desired = e->desired;
    e->exhausted = e->replay_previous = e->previous.valid = false;
    e->resume_pending = false;
    e->completed_sequence = e->hint_boot = e->hint_epoch = e->resume_after_sequence = 0;
    e->request_boot = e->request_epoch = 0;
    memset(e->retired_peer_boot, 0, sizeof(e->retired_peer_boot));
    e->retired_peer_next = 0;
    e->request_due = LED_SYNC_NEVER;
    e->confirmation_due = LED_SYNC_NEVER;
    e->reply_pending = e->fit_pending = e->fit_ack_pending = e->ack_pending = false;
    e->phase_floor = e->last_logical = 0;
    e->dirty = true;
    e->stage = up ? LED_SYNC_LINKED : LED_SYNC_WAITING;
    e->clock_due = up && e->central ? now + LED_SYNC_START_MS : LED_SYNC_NEVER;
    e->clock_timeout = e->state_due = LED_SYNC_NEVER;
}
static inline void led_sync_begin_transaction(struct led_sync_engine *e) {
    e->transaction_active = true;
    e->resume_pending = false; /* This fresh finite budget includes the latest hint. */
    e->exhausted = false;
    e->clock_attempts = e->plan_attempts = 0;
    e->transaction_desired = e->desired;
    e->clock_due = e->clock_timeout = LED_SYNC_NEVER;
    e->confirmation_due = LED_SYNC_NEVER;
}
static inline bool led_sync_exhaust(struct led_sync_engine *e) {
    bool confirmed_latest = e->have_active && e->active_rendered && e->active.confirmed &&
                            e->active.sequence > e->resume_after_sequence &&
                            led_sync_values_equal(&e->visible, &e->desired);
    bool promote =
        !led_sync_values_equal(&e->transaction_desired, &e->desired) && !confirmed_latest;
    promote |= e->resume_pending;
    e->transaction_active = false;
    e->exhausted = true;
    e->clock_due = e->clock_timeout = e->state_due = LED_SYNC_NEVER;
    e->confirmation_due = LED_SYNC_NEVER;
    e->fit_pending = false;
    if (promote) {
        /* This is one newer accepted intent, not a refill of the old budget.
         * Its snapshot prevents unchanged B, or A->B->A, retrying forever. */
        led_sync_begin_transaction(e);
        e->dirty = e->reset_phase = true;
    }
    return promote;
}
/* A peripheral-only aborted suspend need not create a central link event.
 * Hints solicit a challenge; they never establish a clock or a session. */
static inline void led_sync_request_clock(struct led_sync_engine *e, uint64_t now) {
    if (e->central || !e->linked || e->suspended)
        return;
    e->request_boot = e->boot;
    e->request_epoch = e->epoch;
    e->request_attempts = 0;
    e->request_due = now;
}
static inline bool led_sync_retired_boot(const struct led_sync_engine *e, uint32_t boot) {
    for (unsigned i = 0; i < 4; i++)
        if (e->retired_peer_boot[i] == boot)
            return true;
    return false;
}
static inline void led_sync_retire_boot(struct led_sync_engine *e, uint32_t boot) {
    if (!boot || led_sync_retired_boot(e, boot))
        return;
    e->retired_peer_boot[e->retired_peer_next] = boot;
    e->retired_peer_next = (uint8_t)((e->retired_peer_next + 1U) % 4U);
}
static inline void led_sync_desire(struct led_sync_engine *e, struct led_sync_values v,
                                   bool reset_phase) {
    if (!led_sync_values_valid(&v))
        return;
    if (led_sync_values_equal(&e->desired, &v))
        return;
    e->desired = v;
    e->dirty = true;
    if (e->central && (!e->transaction_active || e->exhausted))
        led_sync_begin_transaction(e);
    e->reset_phase |= reset_phase;
}
static inline bool led_sync_identity(const struct led_sync_engine *e,
                                     const struct led_sync_packet *p) {
    return p->central_boot == (e->central ? e->boot : e->central_boot) &&
           p->central_epoch == (e->central ? e->epoch : e->central_epoch) &&
           p->peer_boot == (e->central ? e->peer_boot : e->boot) &&
           p->peer_epoch == (e->central ? e->peer_epoch : e->epoch);
}
static inline uint64_t led_sync_logical_raw(const struct led_sync_engine *e, uint64_t now) {
    uint64_t logical = now;
    if (!e->central && !led_sync_clock_to_central(&e->active_clock, now, &logical))
        return 0;
    return logical;
}
static inline uint32_t led_sync_extra(const struct led_sync_engine *e,
                                      const struct led_sync_clock_fit *fit, uint64_t deadline) {
    uint64_t old_time, new_time;
    if (e->central || !e->have_active)
        return 0;
    if (!led_sync_clock_to_central(&e->active_clock, deadline, &old_time) ||
        !led_sync_clock_to_central(fit, deadline, &new_time))
        return UINT32_MAX;
    if (old_time < e->last_logical)
        old_time = e->last_logical;
    return old_time <= new_time               ? 0
           : old_time - new_time > UINT32_MAX ? UINT32_MAX
                                              : (uint32_t)(old_time - new_time);
}
static inline bool led_sync_coasting(const struct led_sync_engine *e) {
    return e->stage == LED_SYNC_COASTING || e->stage == LED_SYNC_COASTING_UNCONFIRMED;
}
static inline void led_sync_presentation_stage(struct led_sync_engine *e, uint64_t now) {
    if (!e->have_active)
        return;
    uint64_t logical = led_sync_logical_raw(e, now);
    uint64_t floor = e->last_logical > e->phase_floor ? e->last_logical : e->phase_floor;
    uint64_t ahead = logical < floor ? floor - logical : 0;
    if (logical < floor)
        logical = floor;
    bool fresh = e->active.valid && e->active.sequence > e->resume_after_sequence &&
                 ahead <= UINT32_MAX &&
                 led_sync_clock_valid_at(&e->active_clock, logical, (uint32_t)ahead);
    bool confirmed = e->active_rendered && (!e->central || e->active.confirmed);
    e->stage = fresh ? (confirmed ? LED_SYNC_SYNCED : LED_SYNC_RUNNING_UNCONFIRMED)
                     : (confirmed ? LED_SYNC_COASTING : LED_SYNC_COASTING_UNCONFIRMED);
}
/* Capture only the last own-successful presentation. A later failure must not
 * restore desired edits, session identity, the candidate fit or retry budgets. */
static inline void led_sync_capture_previous(struct led_sync_engine *e) {
    e->previous.valid = e->have_active && e->active_rendered;
    if (e->previous.valid) {
        e->previous.active = e->active;
        e->previous.phase_floor = e->phase_floor;
        e->previous.last_logical = e->last_logical;
        e->previous.rendered_at = e->rendered_at;
    }
}
/* Install the already admitted plan after its caller has established the phase
 * floor. Output completion remains a separate transition. */
static inline void led_sync_install_active(struct led_sync_engine *e,
                                           const struct led_sync_plan *plan) {
    e->active = *plan;
    e->active_rendered = false;
    e->active_failure = 0;
    e->active_clock = plan->clock;
    e->visible = led_sync_wire_values(&plan->wire);
    e->phase_epoch = plan->wire.phase_epoch;
    e->have_active = true;
    e->stage = LED_SYNC_RUNNING_UNCONFIRMED;
}
/* The caller checks previous.valid and clears it after handling the attempted
 * plan. Preserve the original successful completion stamp for duplicate ACKs. */
static inline void led_sync_restore_previous(struct led_sync_engine *e, uint64_t now) {
    e->active = e->previous.active;
    e->visible = led_sync_wire_values(&e->active.wire);
    e->active_clock = e->active.clock;
    e->phase_epoch = e->active.wire.phase_epoch;
    e->phase_floor = e->previous.phase_floor;
    e->last_logical = e->previous.last_logical;
    e->rendered_at = e->previous.rendered_at;
    e->active_rendered = e->have_active = true;
    e->active_failure = 0;
    e->replay_previous = true;
    led_sync_presentation_stage(e, now);
}
static inline void led_sync_complete_transaction(struct led_sync_engine *e) {
    if (!e->central || !e->active_rendered || !e->active.confirmed ||
        e->active.sequence <= e->completed_sequence)
        return;
    e->completed_sequence = e->active.sequence;
    e->confirmation_due = LED_SYNC_NEVER;
    /* Historical confirmation cannot rearm an exhausted transaction. If the
     * latest legitimate edit already has a pending plan, it is attempt1 of
     * the next transaction; repeated ACKs cannot reset that accounting. */
    if (e->exhausted)
        return;
    if (!e->pending.valid && !e->resume_pending && e->active.sequence > e->resume_after_sequence &&
        led_sync_values_equal(&e->desired, &e->visible)) {
        e->dirty = false;
        e->reset_phase = false;
    }
    /* One genuine own+peer success completes its transaction even if another
     * immutable plan already exists. Only this new completed sequence can
     * establish the next budget; duplicates or noncurrent ACKs cannot. */
    e->clock_attempts = 0;
    e->plan_attempts = e->pending.valid ? 1 : 0;
    e->transaction_desired = e->desired;
    e->transaction_active = e->pending.valid || e->dirty;
}
static inline bool led_sync_reserve(struct led_sync_engine *e, uint64_t now) {
    if (!e->central || !e->linked || e->suspended || e->pending.valid || !e->dirty ||
        !e->transaction_active || e->exhausted || (e->have_active && !e->active_rendered))
        return false;
    if (e->plan_attempts >= LED_SYNC_PLAN_ATTEMPTS) {
        return led_sync_exhaust(e) && led_sync_reserve(e, now);
    }
    if (e->have_active && e->active.reset_phase && !e->active.confirmed && !e->reset_phase) {
        /* Healthy confirmation preserves phase for a queued slider edit. Only
         * foreground work creates this one deadline; lost ACK cannot block it
         * forever or create an independent background retry transaction. */
        if (e->confirmation_due == LED_SYNC_NEVER)
            e->confirmation_due = now + LED_SYNC_STATE_RETRY_MS;
        if (now < e->confirmation_due)
            return false;
        e->confirmation_due = LED_SYNC_NEVER;
        e->reset_phase = true;
    }
    if (e->clock_due != LED_SYNC_NEVER || e->clock_timeout != LED_SYNC_NEVER || e->fit_pending)
        return false;
    uint64_t apply = now + LED_SYNC_LEAD_MS + 2U * e->fit.network_rtt_ms;
    if (!e->clock_ready || apply > INT64_MAX || !led_sync_clock_valid_at(&e->fit, apply, 0)) {
        if (e->clock_attempts >= LED_SYNC_CLOCK_ATTEMPTS)
            return led_sync_exhaust(e) && led_sync_reserve(e, now);
        else {
            e->clock_due = now;
            /* A stale-to-fresh mapping is a deliberate new phase. It cannot
             * inherit an hours-old coasting floor from another generation. */
            e->reset_phase = true;
        }
        return false;
    }
    if (e->sequence == UINT32_MAX) {
        led_sync_link(e, true, now, e->boot ^ 0x6d2b79f5U);
        return false;
    }
    struct led_sync_plan p = {0};
    e->plan_attempts++;
    p.valid = true;
    p.reset_phase = e->reset_phase || !e->have_active;
    p.sequence = ++e->sequence;
    p.clock = e->fit;
    p.local_deadline = apply;
    p.wire = (struct led_sync_state_wire){
        .clock_sequence = e->clock_sequence,
        .apply_at = apply,
        .phase_epoch = e->reset_phase || !e->have_active ? apply : e->phase_epoch,
        .pattern = e->desired.pattern,
        .brightness = e->desired.brightness,
        .speed = e->desired.speed,
        .flags = e->desired.flags,
    };
    e->pending = p;
    e->confirmation_due = LED_SYNC_NEVER;
    e->dirty = e->reset_phase = false;
    e->state_attempts = 0;
    e->state_due = now;
    return true;
}

static inline void led_sync_ack(struct led_sync_engine *e, const struct led_sync_packet *p,
                                uint8_t status, uint64_t actual) {
    e->ack = led_sync_packet_new(e, LED_SYNC_STATE_ACK, p->sequence);
    e->ack.status = status;
    e->ack.data.applied.state = p->data.applied.state;
    e->ack.data.applied.actual_local = actual;
    e->ack_pending = true;
}
/* True only when a new STATE has passed all validation and is reserved. */
static inline bool led_sync_receive(struct led_sync_engine *e, const struct led_sync_packet *p,
                                    uint64_t now) {
    if (!e->linked || e->suspended || now > INT64_MAX || p->source != 1 ||
        p->version != LED_SYNC_VERSION || p->sequence == 0 || !led_sync_packet_shape(p))
        return false;
    if (e->central && p->kind == LED_SYNC_REQUEST && p->status == LED_SYNC_OK &&
        p->central_boot == 0 && p->central_epoch == 0 && p->sequence == 1 && p->peer_boot &&
        p->peer_epoch) {
        uint32_t seen = p->peer_boot == e->peer_boot ? e->peer_epoch : 0;
        if (p->peer_boot == e->hint_boot && e->hint_epoch > seen)
            seen = e->hint_epoch;
        if (p->peer_epoch <= seen ||
            (p->peer_boot != e->peer_boot && p->peer_boot != e->hint_boot &&
             (p->peer_epoch != 1 || led_sync_retired_boot(e, p->peer_boot))))
            return false;
        if (e->hint_boot && e->hint_boot != p->peer_boot && e->hint_boot != e->peer_boot)
            led_sync_retire_boot(e, e->hint_boot);
        e->hint_boot = p->peer_boot;
        e->hint_epoch = p->peer_epoch; /* Consume even if this burst later fails. */
        /* Preserve a new lifecycle demand if the old burst has no challenge
         * left. Duplicates never reach this point; this is not passive retry. */
        e->resume_pending = true;
        /* Old reservations keep their edge and historical confirmation, but
         * cannot certify the resumed session or consume its presentation demand. */
        e->resume_after_sequence = e->sequence;
        if (!e->transaction_active || e->exhausted)
            led_sync_begin_transaction(e);
        e->dirty = e->reset_phase = true;
        e->clock_ready = false;
        led_sync_presentation_stage(e, now);
        if (e->clock_timeout == LED_SYNC_NEVER && !e->fit_pending)
            e->clock_due = now;
        return false;
    }
    if (!e->central && p->kind == LED_SYNC_PROBE && p->status == LED_SYNC_OK && p->central_boot &&
        p->central_epoch && p->data.clock.t1 <= INT64_MAX) {
        /* A probe is only a challenge candidate. It cannot establish a session;
         * FIT must echo this peer's fresh boot+link epoch and exact sample. */
        bool same_session =
            p->central_boot == e->central_boot && p->central_epoch == e->central_epoch;
        if (e->central_boot && !same_session &&
            ((p->central_boot == e->central_boot && p->central_epoch <= e->central_epoch) ||
             (p->central_boot != e->central_boot && p->central_epoch != 1)))
            return false;
        if (p->central_boot == e->central_boot && p->central_epoch < e->highest_central_epoch)
            return false;
        if (p->central_boot == e->candidate_boot && p->central_epoch < e->candidate_epoch)
            return false;
        if (p->central_boot == e->candidate_boot && p->central_epoch == e->candidate_epoch &&
            p->sequence <= e->candidate_sequence)
            return false;
        if (same_session && p->sequence <= e->clock_sequence)
            return false;
        if (p->central_boot == e->central_boot && p->central_epoch > e->highest_central_epoch)
            e->highest_central_epoch = p->central_epoch;
        /* Consume a fresh challenge even for an abandoned candidate. Equal-ms
         * timestamp captures cannot make an earlier FIT match a later probe. */
        if (e->challenge_counter == UINT32_MAX)
            led_sync_link(e, true, now, e->boot ^ 0x9e3779b9U);
        e->candidate_peer_epoch = ++e->challenge_counter;
        e->candidate_boot = p->central_boot;
        e->candidate_epoch = p->central_epoch;
        e->candidate_sequence = p->sequence;
        e->t1 = p->data.clock.t1;
        e->t2 = now;
        e->reply = led_sync_packet_new(e, LED_SYNC_REPLY, p->sequence);
        e->reply.central_boot = p->central_boot;
        e->reply.central_epoch = p->central_epoch;
        e->reply.peer_epoch = e->candidate_peer_epoch;
        e->reply.data.clock.t1 = e->t1;
        e->reply.data.clock.t2 = e->t2;
        e->reply_pending = true;
        return false;
    }
    if (e->central && p->kind == LED_SYNC_REPLY && p->status == LED_SYNC_OK &&
        p->central_boot == e->boot && p->central_epoch == e->epoch &&
        p->sequence == e->candidate_sequence && p->data.clock.t1 == e->t1 && p->peer_boot &&
        p->peer_epoch && e->clock_timeout != LED_SYNC_NEVER) {
        struct led_sync_clock_fit fit;
        if (!led_sync_clock_sample(&fit, e->t1, p->data.clock.t2, p->data.clock.t3, now,
                                   e->quantization_ms))
            return false;
        if (e->peer_boot && e->peer_boot != p->peer_boot)
            led_sync_retire_boot(e, e->peer_boot);
        e->peer_boot = p->peer_boot;
        e->peer_epoch = p->peer_epoch;
        e->fit = fit;
        e->t2 = p->data.clock.t2;
        e->t3 = p->data.clock.t3;
        e->t4 = now;
        e->fit_pending = true;
        return false;
    }
    if (!e->central && p->kind == LED_SYNC_FIT && p->status == LED_SYNC_OK &&
        p->central_boot == e->candidate_boot && p->central_epoch == e->candidate_epoch &&
        p->peer_boot == e->boot && p->peer_epoch == e->candidate_peer_epoch &&
        p->sequence == e->candidate_sequence && p->data.clock.t1 == e->t1 &&
        p->data.clock.t2 == e->t2 && p->data.clock.t3 == e->t3) {
        struct led_sync_clock_fit fit;
        uint64_t central_now;
        if (!led_sync_clock_sample(&fit, e->t1, e->t2, e->t3, p->data.clock.t4,
                                   e->quantization_ms) ||
            !led_sync_clock_to_central(&fit, now, &central_now) ||
            !led_sync_clock_valid_at(&fit, central_now, 0))
            return false;
        if (p->central_boot != e->central_boot || p->central_epoch != e->central_epoch) {
            e->sequence = 0;
            e->pending.valid = e->active.valid = false;
            /* Preserve successful old presentation until the new future edge;
             * its invalidated plan cannot acknowledge a different session. */
            if (!e->have_active)
                e->stage = LED_SYNC_LINKED;
            else
                led_sync_presentation_stage(e, now);
        }
        e->epoch = e->candidate_peer_epoch;
        e->fit = fit;
        e->central_boot = p->central_boot;
        e->central_epoch = p->central_epoch;
        e->highest_central_epoch = p->central_epoch;
        e->clock_sequence = p->sequence;
        e->clock_ready = true;
        e->request_due = LED_SYNC_NEVER;
        e->fit_ack = led_sync_packet_new(e, LED_SYNC_FIT_ACK, p->sequence);
        e->fit_ack.data.clock = p->data.clock;
        e->fit_ack_pending = true;
        return false;
    }
    if (!led_sync_identity(e, p))
        return false;
    if (e->central && p->kind == LED_SYNC_FIT_ACK && p->status == LED_SYNC_OK &&
        e->clock_timeout != LED_SYNC_NEVER && p->sequence == e->candidate_sequence &&
        p->data.clock.t1 == e->t1 && p->data.clock.t2 == e->t2 && p->data.clock.t3 == e->t3 &&
        p->data.clock.t4 == e->t4 && led_sync_clock_valid_at(&e->fit, now, 0)) {
        e->clock_sequence = p->sequence;
        e->clock_ready = true;
        e->clock_timeout = LED_SYNC_NEVER;
        e->clock_due = LED_SYNC_NEVER;
        e->dirty = true; /* Refit becomes visible only at another common reservation. */
        led_sync_reserve(e, now);
        return false;
    }
    if (!e->central && p->kind == LED_SYNC_STATE && p->status == LED_SYNC_OK) {
        const struct led_sync_state_wire *w = &p->data.applied.state;
        struct led_sync_values v = led_sync_wire_values(w);
        if (!led_sync_values_valid(&v) || w->apply_at > INT64_MAX || w->phase_epoch > w->apply_at)
            return false;
        if (e->active.valid && p->sequence == e->active.sequence &&
            led_sync_state_equal(w, &e->active.wire)) {
            if (e->have_active && e->active_rendered)
                led_sync_ack(e, p, LED_SYNC_APPLIED, e->rendered_at);
            else
                led_sync_ack(e, p,
                             e->have_active      ? LED_SYNC_ACCEPTED
                             : e->active_failure ? e->active_failure
                                                 : LED_SYNC_REJECT_CLOCK,
                             0);
            return false;
        }
        if (e->pending.valid && p->sequence == e->pending.sequence &&
            led_sync_state_equal(w, &e->pending.wire)) {
            led_sync_ack(e, p, LED_SYNC_ACCEPTED, 0);
            return false;
        }
        if (p->sequence <= e->sequence || e->pending.valid)
            return false;
        uint64_t deadline;
        if (!e->clock_ready || w->clock_sequence != e->clock_sequence ||
            !led_sync_clock_to_local(&e->fit, w->apply_at, &deadline) ||
            !led_sync_clock_valid_at(
                &e->fit, w->apply_at,
                w->phase_epoch == w->apply_at ? 0 : led_sync_extra(e, &e->fit, deadline))) {
            led_sync_ack(e, p, LED_SYNC_REJECT_CLOCK, 0);
            return false;
        }
        if (deadline < now || deadline - now < LED_SYNC_RECEIVE_LEAD_MS) {
            led_sync_ack(e, p, LED_SYNC_REJECT_LATE, 0);
            return false;
        }
        e->pending = (struct led_sync_plan){.valid = true,
                                            .reset_phase = w->phase_epoch == w->apply_at,
                                            .sequence = p->sequence,
                                            .wire = *w,
                                            .clock = e->fit,
                                            .local_deadline = deadline};
        e->sequence = p->sequence;
        led_sync_ack(e, p, LED_SYNC_ACCEPTED, 0);
        return true;
    }
    if (e->central && p->kind == LED_SYNC_STATE_ACK && p->status >= LED_SYNC_ACCEPTED &&
        p->status <= LED_SYNC_REJECT_RENDER) {
        struct led_sync_plan *plan =
            e->pending.valid && e->pending.sequence == p->sequence ? &e->pending
            : e->active.valid && e->active.sequence == p->sequence ? &e->active
                                                                   : NULL;
        if (!plan || !led_sync_state_equal(&plan->wire, &p->data.applied.state))
            return false;
        if (p->status == LED_SYNC_APPLIED) {
            uint64_t actual;
            if (!led_sync_clock_to_central(&plan->clock, p->data.applied.actual_local, &actual) ||
                actual < plan->wire.apply_at || actual - plan->wire.apply_at > LED_SYNC_LATE_MS ||
                !led_sync_clock_valid_at(&plan->clock, actual, 0))
                return false;
            plan->confirmed = true;
            if (plan == &e->active && e->have_active && e->active_rendered) {
                led_sync_presentation_stage(e, now);
                led_sync_complete_transaction(e);
                led_sync_reserve(e, now);
            }
        } else if (p->status >= LED_SYNC_REJECT_LATE) {
            if (e->transaction_active && !e->exhausted)
                e->dirty = e->reset_phase = true;
            /* Central's already reserved edge is never moved by an ACK. */
        }
        if (plan == &e->pending) {
            /* Admission is known; further copies cannot improve this immutable
             * reservation. APPLIED confirmation remains a separate state. */
            e->state_due = LED_SYNC_NEVER;
            e->state_attempts = LED_SYNC_STATE_ATTEMPTS;
        }
    }
    return false;
}

/* Called by the serialized render queue BEFORE every draw. Thus an animation
 * item queued earlier cannot draw old state after a reservation becomes due. */
static inline bool led_sync_advance(struct led_sync_engine *e, uint64_t now) {
    if (!e->linked || e->suspended)
        return false;
    bool applied = false;
    if (e->pending.valid && now >= e->pending.local_deadline) {
        struct led_sync_plan p = e->pending;
        e->pending.valid = false;
        e->state_due = LED_SYNC_NEVER;
        uint64_t central_now = now;
        bool mapped = e->central || led_sync_clock_to_central(&p.clock, now, &central_now);
        uint32_t extra = p.reset_phase ? 0 : led_sync_extra(e, &p.clock, now);
        if (now - p.local_deadline > LED_SYNC_LATE_MS || !mapped ||
            !led_sync_clock_valid_at(&p.clock, central_now, extra)) {
            e->dirty |= e->central;
            e->reset_phase |= e->central && p.reset_phase;
            if (!e->have_active)
                e->stage = LED_SYNC_LINKED;
            if (!e->central) {
                struct led_sync_packet packet = led_sync_packet_new(e, LED_SYNC_STATE, p.sequence);
                packet.data.applied.state = p.wire;
                led_sync_ack(e, &packet, LED_SYNC_REJECT_LATE, now);
            }
        } else {
            led_sync_capture_previous(e);
            uint64_t previous = 0;
            if (!p.reset_phase) {
                previous = e->have_active ? led_sync_logical_raw(e, now) : 0;
                if (previous < e->last_logical)
                    previous = e->last_logical;
            } else {
                /* Only the due edge installs the new phase generation. A
                 * stale coasting mapping cannot clamp this reset forever. */
                e->last_logical = 0;
            }
            e->phase_floor = previous;
            led_sync_install_active(e, &p);
            applied = true;
        }
    }
    if (e->have_active) {
        uint64_t logical = led_sync_logical_raw(e, now);
        if (logical < e->phase_floor)
            logical = e->phase_floor;
        if (logical < e->last_logical)
            logical = e->last_logical;
        e->last_logical = logical;
        led_sync_presentation_stage(e, now);
    }
    if (e->central)
        led_sync_reserve(e, now);
    return applied;
}
static inline void led_sync_rendered(struct led_sync_engine *e, bool succeeded, uint64_t now) {
    if (!e->have_active)
        return; /* Link/PM teardown cannot resurrect an old view. */
    if (!e->active.valid) {
        if (e->active_rendered)
            return; /* A completed coasting view is not another failed attempt. */
        /* A new session retired this in-flight reservation. Restore only local
         * presentation history; never ACK the old attempt with new identities. */
        if (e->previous.valid) {
            e->previous.active.valid = false;
            led_sync_restore_previous(e, now);
        } else {
            e->have_active = false;
            e->stage = LED_SYNC_LINKED;
        }
        e->previous.valid = false;
        e->replay_previous = true; /* Replay A, or redraw status when no A exists. */
        return;
    }
    const bool late =
        now < e->active.local_deadline || now - e->active.local_deadline > LED_SYNC_LATE_MS;
    uint64_t logical = now;
    bool mapped = e->central || led_sync_clock_to_central(&e->active_clock, now, &logical);
    uint64_t floor = e->last_logical > e->phase_floor ? e->last_logical : e->phase_floor;
    uint64_t ahead = logical < floor ? floor - logical : 0;
    if (logical < floor)
        logical = floor;
    bool clock_valid = mapped && ahead <= UINT32_MAX &&
                       led_sync_clock_valid_at(&e->active_clock, logical, (uint32_t)ahead);
    uint8_t failure = !clock_valid ? LED_SYNC_REJECT_CLOCK
                      : late       ? LED_SYNC_REJECT_LATE
                      : !succeeded ? LED_SYNC_REJECT_RENDER
                                   : LED_SYNC_OK;
    const struct led_sync_plan attempted = e->active;
    if (failure) {
        e->active_failure = failure;
        e->dirty |= e->central && e->transaction_active;
        e->reset_phase |= e->central && attempted.reset_phase;
        e->have_active = false;
        e->stage = LED_SYNC_LINKED;
        if (e->previous.valid) {
            led_sync_restore_previous(e, now);
        }
    } else {
        e->active_rendered = true;
        e->rendered_at = now;
        led_sync_presentation_stage(e, now);
        led_sync_complete_transaction(e);
    }
    e->previous.valid = false;
    if (!e->central) {
        struct led_sync_packet p = led_sync_packet_new(e, LED_SYNC_STATE, attempted.sequence);
        p.data.applied.state = attempted.wire;
        led_sync_ack(e, &p, failure ? failure : LED_SYNC_APPLIED, now);
    }
    if (e->central)
        led_sync_reserve(e, now);
}

/* At most one packet per call. Timestamp fields are captured on the TX worker,
 * immediately before relay raise, never when the work was enqueued. */
static inline bool led_sync_send(struct led_sync_engine *e, uint64_t now,
                                 struct led_sync_packet *out) {
    if (!e->linked || e->suspended || now > INT64_MAX)
        return false;
    if (e->central && now >= e->confirmation_due)
        led_sync_reserve(e, now);
    if (e->reply_pending) {
        *out = e->reply;
        out->data.clock.t3 = e->t3 = now;
        e->reply_pending = false;
        return true;
    }
    if (e->fit_ack_pending) {
        *out = e->fit_ack;
        e->fit_ack_pending = false;
        return true;
    }
    if (e->ack_pending) {
        *out = e->ack;
        e->ack_pending = false;
        return true;
    }
    if (!e->central) {
        if (!e->clock_ready && now >= e->request_due &&
            e->request_attempts < LED_SYNC_CLOCK_ATTEMPTS) {
            *out = led_sync_packet_new(e, LED_SYNC_REQUEST, 1);
            out->central_boot = out->central_epoch = 0;
            out->peer_boot = e->request_boot;
            out->peer_epoch = e->request_epoch;
            e->request_attempts++;
            e->request_due = e->request_attempts < LED_SYNC_CLOCK_ATTEMPTS ? now + LED_SYNC_RETRY_MS
                                                                           : LED_SYNC_NEVER;
            return true;
        }
        return false;
    }
    if (e->fit_pending) {
        *out = led_sync_packet_new(e, LED_SYNC_FIT, e->candidate_sequence);
        out->data.clock.t1 = e->t1;
        out->data.clock.t2 = e->t2;
        out->data.clock.t3 = e->t3;
        out->data.clock.t4 = e->t4;
        e->fit_pending = false;
        return true;
    }
    if (e->pending.valid && e->pending.local_deadline < now + LED_SYNC_RECEIVE_LEAD_MS)
        e->state_due = LED_SYNC_NEVER;
    if (e->pending.valid && now >= e->state_due && e->state_attempts < LED_SYNC_STATE_ATTEMPTS &&
        e->pending.local_deadline >= now + LED_SYNC_RECEIVE_LEAD_MS) {
        *out = led_sync_packet_new(e, LED_SYNC_STATE, e->pending.sequence);
        out->data.applied.state = e->pending.wire;
        e->state_attempts++;
        e->state_due = e->state_attempts < LED_SYNC_STATE_ATTEMPTS ? now + LED_SYNC_STATE_RETRY_MS
                                                                   : LED_SYNC_NEVER;
        return true;
    }
    if (!e->pending.valid && (now >= e->clock_due || now >= e->clock_timeout)) {
        if (e->clock_attempts >= LED_SYNC_CLOCK_ATTEMPTS) {
            if (led_sync_exhaust(e))
                led_sync_reserve(e, now);
            return false;
        }
        if (e->sequence == UINT32_MAX) {
            led_sync_link(e, true, now, e->boot ^ 0x6d2b79f5U);
            return false;
        }
        /* An actual post-hint challenge services the demand within the
         * remaining finite budget; failure does not refill it again. */
        e->resume_pending = false;
        e->candidate_sequence = ++e->sequence;
        e->clock_attempts++;
        e->clock_ready = false;
        e->t1 = now;
        e->clock_timeout = now + LED_SYNC_RETRY_MS;
        e->clock_due = LED_SYNC_NEVER;
        *out = led_sync_packet_new(e, LED_SYNC_PROBE, e->candidate_sequence);
        out->data.clock.t1 = now;
        return true;
    }
    return false;
}
static inline uint64_t led_sync_next_tx(const struct led_sync_engine *e, uint64_t now) {
    if (!e->linked || e->suspended)
        return LED_SYNC_NEVER;
    if (e->reply_pending || e->fit_pending || e->fit_ack_pending || e->ack_pending)
        return now;
    if (!e->central)
        return e->request_due;
    if (e->pending.valid)
        return e->state_due;
    uint64_t next = e->clock_due < e->clock_timeout ? e->clock_due : e->clock_timeout;
    return e->confirmation_due < next ? e->confirmation_due : next;
}
static inline uint64_t led_sync_next_render(const struct led_sync_engine *e, uint64_t now) {
    if (!e->linked || e->suspended)
        return LED_SYNC_NEVER;
    uint64_t next = e->pending.valid ? e->pending.local_deadline : LED_SYNC_NEVER;
    /* A conservative1ms quality timer is avoided: schedule only the hard expiry
     * or the first integer-ms aging step which exceeds the declared bound. */
    if (e->have_active && !led_sync_coasting(e)) {
        uint64_t logical = led_sync_logical_raw(e, now);
        uint32_t extra = logical < e->last_logical ? (e->last_logical - logical > UINT32_MAX
                                                          ? UINT32_MAX
                                                          : (uint32_t)(e->last_logical - logical))
                                                   : 0;
        uint64_t expiry = e->active_clock.reference_ms + 8000;
        uint64_t base = (uint64_t)e->active_clock.base_error_ms + extra;
        uint64_t quality = e->active_clock.reference_ms + (base >= 50 ? 1 : (50 - base) * 1000 + 1);
        if (quality < expiry)
            expiry = quality;
        uint64_t local = expiry;
        if (e->central || led_sync_clock_to_local(&e->active_clock, expiry, &local))
            if (local < next)
                next = local;
    }
    return next;
}
