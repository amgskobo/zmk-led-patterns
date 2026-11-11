/* SPDX-License-Identifier: MIT
 * Drive two instances of the production protocol, with explicit local clocks.
 * These are controlled queue/network inputs, not physical radio evidence.
 */
#include "led_sync_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct led_sync_engine central, peer;
static const struct led_sync_values initial = {1, 80, 100, LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF};
static struct led_sync_packet probe, reply, fit, fit_ack, state;
static unsigned packets;

static struct led_sync_packet sent(struct led_sync_engine *e, uint64_t now) {
    struct led_sync_packet p;
    assert(led_sync_send(e, now, &p));
    assert(p.source == 255 && p.version == 4 && led_sync_packet_shape(&p));
    packets++;
    p.source = 1;
    printf("tx %u %llu %u %u\n", e->central, (unsigned long long)now, p.kind, p.sequence);
    return p;
}
static void fresh(uint64_t start) {
    led_sync_init(&central, true, 111, 2, initial);
    led_sync_init(&peer, false, 222, 2, initial);
    led_sync_link(&central, true, start, 333);
    led_sync_link(&peer, true, start + 10000, 444);
    packets = 0;
}
static void clock_exchange(uint64_t start) {
    probe = sent(&central, start + 5000);
    led_sync_receive(&peer, &probe, start + 15010);
    assert(!peer.clock_ready && peer.central_boot == 0);
    reply = sent(&peer, start + 15015);
    led_sync_receive(&central, &reply, start + 5025);
    assert(!central.clock_ready);
    fit = sent(&central, start + 5025);
    led_sync_receive(&peer, &fit, start + 15035);
    assert(peer.clock_ready && !peer.have_active && !peer.pending.valid);
    fit_ack = sent(&peer, start + 15035);
    led_sync_receive(&central, &fit_ack, start + 5045);
    assert(central.clock_ready && central.pending.valid);
    assert(central.fit.offset_ms == 10000 && peer.fit.offset_ms == 10000);
    assert(central.fit.network_rtt_ms == 20);
    assert(central.pending.wire.apply_at == start + 6085);
}
static void accept_state_mode(uint64_t start, bool deliver_ack) {
    state = sent(&central, start + 5045);
    assert(state.kind == LED_SYNC_STATE);
    led_sync_receive(&peer, &state, start + 15055);
    assert(peer.pending.valid && !peer.have_active);
    assert(peer.pending.local_deadline == start + 16085);
    struct led_sync_packet ack = sent(&peer, start + 15055);
    assert(ack.status == LED_SYNC_ACCEPTED);
    if (deliver_ack)
        led_sync_receive(&central, &ack, start + 5065);
    assert(!central.have_active && !central.pending.confirmed);
}
static void accept_state(uint64_t start) { accept_state_mode(start, true); }
static void apply_both(uint64_t start, bool acknowledge) {
    assert(!led_sync_advance(&central, start + 6084));
    assert(!led_sync_advance(&peer, start + 16084));
    assert(!central.have_active && !peer.have_active);
    assert(led_sync_advance(&central, start + 6085));
    assert(led_sync_advance(&peer, start + 16085));
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED &&
           peer.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    led_sync_rendered(&central, true, start + 6085);
    assert(central.have_active && peer.have_active);
    assert(central.phase_epoch == peer.phase_epoch);
    assert(central.last_logical == start + 6085 && peer.last_logical == start + 6085);
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    led_sync_rendered(&peer, true, start + 16085);
    struct led_sync_packet ack = sent(&peer, start + 16085);
    assert(ack.status == LED_SYNC_APPLIED);
    if (acknowledge) {
        led_sync_receive(&central, &ack, start + 6095);
        assert(central.stage == LED_SYNC_SYNCED);
    }
}
static void test_scheduled_and_desired(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    /* Receipt itself changes neither visible state nor phase. */
    assert(peer.visible.brightness == 0 && central.visible.brightness == 0);
    uint64_t first_deadline = central.pending.local_deadline;
    struct led_sync_values next = initial;
    for (unsigned i = 0; i < 1000; i++) {
        next.brightness = (uint8_t)(i % 101);
        next.speed = (uint16_t)(10 + i % 391);
        led_sync_desire(&central, next, false);
        led_sync_reserve(&central, 5050 + i / 10);
        assert(central.pending.local_deadline == first_deadline);
        assert(central.desired.brightness == next.brightness);
    }
    assert(central.state_attempts == LED_SYNC_STATE_ATTEMPTS && packets == 6);
    apply_both(0, true);
    assert(central.visible.brightness == initial.brightness);
    assert(central.pending.valid && central.pending.wire.brightness == next.brightness);
    assert(central.pending.wire.phase_epoch == 6085); /* Brightness/speed preserve epoch. */
    uint64_t second = central.pending.local_deadline;
    next.pattern = 3;
    led_sync_desire(&central, next, true);
    assert(central.pending.local_deadline == second && central.reset_phase);
    printf("scheduled desired: PASS\n");
}
static void test_duplicate_reordered_and_shape(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    const uint64_t deadline = peer.pending.local_deadline;
    struct led_sync_packet p = probe;
    led_sync_receive(&peer, &p, 15100); /* Old sample cannot replace committed fit. */
    assert(!peer.reply_pending && peer.clock_sequence == fit.sequence);
    p.central_boot++;
    p.central_epoch = 2; /* Different boot must begin a fresh epoch1 challenge. */
    p.sequence += 100;
    led_sync_receive(&peer, &p, 15100);
    assert(!peer.reply_pending && peer.central_boot == 111);
    p = fit;
    p.sequence--;
    led_sync_receive(&peer, &p, 15100);
    assert(peer.pending.local_deadline == deadline);
    led_sync_receive(&peer, &fit, 15100); /* Exact FIT is idempotent, accepted deadline frozen. */
    assert(peer.pending.local_deadline == deadline && peer.fit_ack_pending);
    (void)sent(&peer, 15100);
    uint64_t due = central.clock_due;
    central.dirty = false;
    led_sync_receive(&central, &fit_ack, 5100);
    assert(!central.dirty && central.clock_due == due);
    p = state;
    p.data.applied.state.brightness--;
    led_sync_receive(&peer, &p, 15110); /* Same sequence with different contents. */
    assert(peer.pending.wire.brightness == state.data.applied.state.brightness);
    for (unsigned kind = LED_SYNC_PROBE; kind <= LED_SYNC_REQUEST; kind++) {
        p = led_sync_packet_new(&central, (uint8_t)kind, 7);
        p.source = 1;
        if (kind == LED_SYNC_STATE_ACK)
            p.status = LED_SYNC_ACCEPTED;
        assert(led_sync_packet_shape(&p));
        p.data.bytes[39] = 1;
        assert(!led_sync_packet_shape(&p));
    }
    p = state;
    p.version++;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.source = 255;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.peer_epoch++;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.data.applied.state.flags = 16;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.data.applied.state.speed = 9;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.data.applied.state.pattern = 18;
    led_sync_receive(&peer, &p, 15110);
    p = state;
    p.data.applied.state.phase_epoch = p.data.applied.state.apply_at + 1;
    led_sync_receive(&peer, &p, 15110);
    assert(peer.pending.local_deadline == deadline);
    printf("duplicate reorder shape: PASS\n");
}
static void test_late_retry_and_boundaries(void) {
    fresh(0);
    clock_exchange(0);
    uint64_t deadline = central.pending.local_deadline;
    state = sent(&central, deadline - 100);
    assert(central.state_attempts == 1);
    led_sync_receive(&peer, &state, deadline + 10000 - 100);
    assert(peer.pending.valid);
    struct led_sync_packet p;
    assert(!led_sync_send(&central, deadline - 99, &p));
    assert(led_sync_next_tx(&central, deadline - 99) == LED_SYNC_NEVER);
    assert(led_sync_advance(&central, deadline + 40));
    fresh(0);
    clock_exchange(0);
    deadline = central.pending.local_deadline;
    state = sent(&central, 5045);
    led_sync_receive(&peer, &state, deadline + 10000 - 99);
    assert(!peer.pending.valid && !peer.have_active);
    p = sent(&peer, deadline + 10000 - 99);
    assert(p.status == LED_SYNC_REJECT_LATE);
    led_sync_receive(&central, &p, deadline - 50);
    assert(central.dirty && central.pending.local_deadline == deadline);
    assert(!led_sync_advance(&central, deadline + 41));
    assert(!central.have_active && central.pending.valid &&
           central.pending.local_deadline > deadline + 41);
    assert(central.stage == LED_SYNC_LINKED);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    deadline = peer.pending.local_deadline;
    assert(!led_sync_advance(&peer, deadline + 41));
    assert(!peer.have_active && peer.stage == LED_SYNC_LINKED);
    p = sent(&peer, deadline + 41);
    assert(p.status == LED_SYNC_REJECT_LATE);
    printf("late retry boundaries: PASS\n");
}
static void test_expiry_quality_and_frozen_mapping(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    uint64_t deadline = peer.pending.local_deadline;
    peer.fit.offset_ms += 20; /* A refit cannot move an accepted edge. */
    peer.fit.lower_ms += 20;
    peer.fit.upper_ms += 20;
    assert(peer.pending.local_deadline == deadline);
    assert(led_sync_advance(&peer, deadline));
    assert(peer.active.clock.offset_ms == 10000 && peer.last_logical == 6085);
    led_sync_rendered(&peer, true, deadline);
    (void)sent(&peer, deadline);
    uint64_t expiry = led_sync_next_render(&peer, deadline);
    assert(expiry > deadline && expiry <= 23025);
    assert(!led_sync_advance(&peer, 23025));
    assert(peer.have_active && peer.stage == LED_SYNC_COASTING);
    assert(led_sync_next_render(&peer, 23025) == LED_SYNC_NEVER);
    assert(led_sync_next_tx(&peer, 23025) == LED_SYNC_NEVER);
    fresh(0);
    clock_exchange(0);
    central.pending.valid = false;
    central.dirty = true;
    assert(!led_sync_reserve(&central, 12000)); /* Future lead crosses8s TTL. */
    assert(central.clock_due <= 12000);
    central.fit.base_error_ms = 49;
    central.fit.reference_ms = 12000;
    central.clock_due = LED_SYNC_NEVER;
    assert(!led_sync_reserve(&central, 12000)); /* Lead aging exceeds50 even fresh. */
    printf("expiry quality frozen mapping: PASS\n");
}
static void test_reconnect_and_pm_fences(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    struct led_sync_packet old_state = state, old_fit = fit;
    uint32_t old_epoch = peer.epoch;
    led_sync_link(&peer, false, 15100, 777);
    led_sync_link(&peer, true, 15101, 778);
    assert(peer.epoch != old_epoch && !peer.pending.valid && !peer.have_active);
    led_sync_receive(&peer, &probe, 15102); /* Queued old probe can solicit challenge only. */
    assert(!peer.clock_ready && peer.central_boot == 0);
    led_sync_receive(&peer, &old_fit, 15103);
    led_sync_receive(&peer, &old_state, 15104);
    assert(!peer.clock_ready && !peer.pending.valid);
    struct led_sync_packet challenge = sent(&peer, 15105);
    assert(challenge.peer_epoch == peer.candidate_peer_epoch &&
           challenge.peer_epoch != old_fit.peer_epoch);
    led_sync_link(&central, true, 5200, 779);
    led_sync_receive(&central, &reply, 5201);
    assert(!central.clock_ready && !central.pending.valid);
    central.suspended = true;
    led_sync_link(&central, true, 5300, 780);
    struct led_sync_values v = initial;
    v.brightness = 20;
    led_sync_desire(&central, v, false);
    struct led_sync_packet p;
    assert(!led_sync_send(&central, 20000, &p));
    assert(central.desired.brightness == 20 && !central.pending.valid);
    central.suspended = false;
    led_sync_link(&central, true, 20000, 781);
    assert(central.clock_due == 25000 && central.desired.brightness == 20);
    printf("reconnect PM fences: PASS\n");
}
static void test_bounded_traffic_and_loss(void) {
    fresh(0);
    struct led_sync_packet p;
    for (unsigned i = 0; i < 5; i++) {
        p = sent(&central, 5000 + 500 * i);
        assert(p.kind == LED_SYNC_PROBE);
    }
    assert(!led_sync_send(&central, 7500, &p));
    assert(central.clock_due == LED_SYNC_NEVER && central.clock_attempts == 5 && central.exhausted);
    assert(!led_sync_send(&central, 37499, &p));
    assert(!led_sync_send(&central, 37500, &p));
    struct led_sync_values changed = initial;
    changed.brightness--;
    led_sync_desire(&central, changed, false);
    assert(!led_sync_reserve(&central, 37500));
    assert(led_sync_send(&central, 37500, &p));
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(!led_sync_send(&central, 5245, &p)); /* Received admission stops copies. */
    fresh(0);
    clock_exchange(0);
    accept_state_mode(0, false);
    for (unsigned i = 1; i < 3; i++) {
        p = sent(&central, 5045 + 200 * i);
        assert(p.kind == LED_SYNC_STATE && p.sequence == state.sequence);
        led_sync_receive(&peer, &p, 15055 + 200 * i);
        struct led_sync_packet accepted = sent(&peer, 15055 + 200 * i);
        assert(accepted.status == LED_SYNC_ACCEPTED);
    }
    assert(!led_sync_send(&central, 5645, &p));
    apply_both(0, false);
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    uint64_t due = 30100;
    for (uint64_t now = 6100; now < due; now += 40) {
        led_sync_advance(&central, now);
        assert(!led_sync_send(&central, now, &p));
    }
    assert(!led_sync_send(&central, due, &p));
    assert(central.have_active && central.stage == LED_SYNC_COASTING_UNCONFIRMED);
    assert(led_sync_next_tx(&central, due) == LED_SYNC_NEVER);
    printf("bounded traffic loss: PASS\n");
}
static void test_ack_identity_render_failure_and_long_uptime(void) {
    const uint64_t start = UINT64_C(5000000000);
    fresh(start);
    clock_exchange(start);
    accept_state(start);
    apply_both(start, false);
    struct led_sync_packet ack = led_sync_packet_new(&peer, LED_SYNC_STATE_ACK, state.sequence);
    ack.source = 1;
    ack.status = LED_SYNC_APPLIED;
    ack.data.applied.state = state.data.applied.state;
    ack.data.applied.actual_local = start + 16085;
    ack.data.applied.state.brightness--;
    led_sync_receive(&central, &ack, start + 6100);
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    ack.data.applied.state.brightness++;
    ack.peer_epoch++;
    led_sync_receive(&central, &ack, start + 6100);
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    ack.peer_epoch--;
    led_sync_receive(&central, &ack, start + 6100);
    assert(central.stage == LED_SYNC_SYNCED);
    led_sync_rendered(&peer, false, start + 16100);
    assert(!peer.have_active && peer.stage == LED_SYNC_LINKED);
    ack = sent(&peer, start + 16100);
    assert(ack.status == LED_SYNC_REJECT_RENDER);
    led_sync_receive(&central, &ack, start + 6110);
    assert(!central.dirty); /* Closed historical reservation cannot rearm recovery. */
    assert(central.phase_epoch > UINT32_MAX && central.last_logical > UINT32_MAX);
    struct led_sync_values changed = initial;
    changed.brightness--;
    led_sync_desire(&central, changed, false);
    central.sequence = UINT32_MAX;
    central.pending.valid = false;
    central.dirty = true;
    uint32_t epoch = central.epoch;
    assert(!led_sync_reserve(&central, start + 6120));
    assert(central.epoch != epoch && central.sequence == 0 && !central.have_active);
    central.epoch = UINT32_MAX;
    led_sync_link(&central, true, start + 6200, 999);
    assert(central.epoch == 1 && central.boot == 999);
    printf("ACK identity renderer64bit: PASS\n");
}
static void test_render_completion_and_reset_recovery(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&peer, 16085));
    led_sync_receive(&peer, &state, 16086);
    struct led_sync_packet ack = sent(&peer, 16086);
    assert(ack.status == LED_SYNC_ACCEPTED); /* Drawing has not completed yet. */
    led_sync_rendered(&peer, false, 16087);
    (void)sent(&peer, 16087);
    led_sync_receive(&peer, &state, 16088);
    ack = sent(&peer, 16088);
    assert(ack.status == LED_SYNC_REJECT_RENDER && !peer.have_active);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&peer, 16085));
    led_sync_rendered(&peer, true, 16126);
    ack = sent(&peer, 16126);
    assert(ack.status == LED_SYNC_REJECT_LATE && !peer.have_active);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&peer, 16120)); /* Begin35ms late, driver takes30ms. */
    led_sync_rendered(&peer, true, 16150);
    ack = sent(&peer, 16150);
    assert(ack.status == LED_SYNC_REJECT_LATE && !peer.have_active);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&central, 6085));
    led_sync_rendered(&central, true, 6126);
    assert(!central.have_active && central.pending.valid);
    assert(central.pending.wire.phase_epoch == central.pending.wire.apply_at);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    apply_both(0, true);
    struct led_sync_values v = initial;
    v.pattern = 3;
    led_sync_desire(&central, v, true);
    assert(led_sync_reserve(&central, 6100));
    uint64_t failed = central.pending.local_deadline;
    assert(central.pending.wire.phase_epoch == failed);
    led_sync_advance(&central, failed + 41);
    assert(central.pending.valid && central.pending.wire.pattern == 3);
    assert(central.pending.wire.phase_epoch == central.pending.wire.apply_at);
    puts("render completion reset recovery: PASS");
}
static void exchange_at(uint64_t c) {
    probe = sent(&central, c);
    led_sync_receive(&peer, &probe, c + 10010);
    reply = sent(&peer, c + 10015);
    led_sync_receive(&central, &reply, c + 25);
    fit = sent(&central, c + 25);
    led_sync_receive(&peer, &fit, c + 10035);
    fit_ack = sent(&peer, c + 10035);
    led_sync_receive(&central, &fit_ack, c + 45);
    assert(central.clock_ready && peer.clock_ready && central.pending.valid);
    state = sent(&central, c + 45);
    led_sync_receive(&peer, &state, c + 10055);
    assert(peer.pending.valid);
    struct led_sync_packet accepted = sent(&peer, c + 10055);
    led_sync_receive(&central, &accepted, c + 65);
    uint64_t deadline = central.pending.local_deadline;
    assert(peer.pending.local_deadline == deadline + 10000);
    assert(led_sync_advance(&central, deadline));
    assert(led_sync_advance(&peer, deadline + 10000));
    led_sync_rendered(&central, true, deadline);
    led_sync_rendered(&peer, true, deadline + 10000);
    struct led_sync_packet ack = sent(&peer, deadline + 10000);
    led_sync_receive(&central, &ack, deadline + 10);
    assert(central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
}
static void test_unilateral_sessions(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    apply_both(0, true);
    const uint32_t peer_epoch = peer.epoch;
    central.suspended = true;
    led_sync_link(&central, true, 7000, 888);
    central.suspended = false;
    led_sync_link(&central, true, 7001, 889);
    assert(peer.epoch == peer_epoch && peer.have_active);
    exchange_at(12001);
    assert(peer.epoch != peer_epoch && peer.central_epoch == central.epoch);
    struct led_sync_values changed = initial;
    changed.brightness--;
    led_sync_desire(&central, changed, false);
    central.sequence = UINT32_MAX;
    central.dirty = true;
    central.clock_due = LED_SYNC_NEVER;
    assert(!led_sync_reserve(&central, 13100));
    uint32_t epoch = central.epoch;
    exchange_at(18100);
    assert(peer.central_epoch == epoch && peer.sequence == central.active.sequence);
    /* Epoch wrap uses a new boot identity and a fresh peer challenge too. */
    central.epoch = UINT32_MAX;
    led_sync_link(&central, true, 19300, 999);
    exchange_at(24300);
    assert(peer.central_boot == 999 && peer.central_epoch == 1);
    puts("unilateral central PM sequence and epoch rotation: PASS");
}
static void test_completion_quality_boundary(void) {
    fresh(10000);
    clock_exchange(10000);
    accept_state(10000);
    peer.pending.clock.reference_ms = peer.pending.wire.apply_at - 7990;
    peer.pending.clock.base_error_ms = 13;
    assert(led_sync_advance(&peer, 26085));
    led_sync_rendered(&peer, true, 26105); /*20ms completion crosses hardTTL. */
    struct led_sync_packet ack = sent(&peer, 26105);
    assert(ack.status == LED_SYNC_REJECT_CLOCK && !peer.have_active);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    peer.pending.clock.base_error_ms = 49;
    peer.pending.clock.reference_ms = peer.pending.wire.apply_at - 990;
    assert(led_sync_advance(&peer, 16085));
    led_sync_rendered(&peer, true, 16105); /* Aging crosses error50 within40ms. */
    ack = sent(&peer, 16105);
    assert(ack.status == LED_SYNC_REJECT_CLOCK && !peer.have_active);
    puts("completion clock quality boundary: PASS");
}
static void test_completion_stage_identity(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&peer, 16085));
    assert(peer.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    led_sync_rendered(&peer, true, 16085);
    assert(peer.stage == LED_SYNC_SYNCED);
    struct led_sync_packet ack = sent(&peer, 16085);
    assert(led_sync_advance(&central, 6085));
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    led_sync_receive(&central, &ack, 6090);
    assert(central.active.confirmed && central.stage == LED_SYNC_RUNNING_UNCONFIRMED);
    led_sync_rendered(&central, true, 6095);
    assert(central.stage == LED_SYNC_SYNCED);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&peer, 16085));
    led_sync_rendered(&peer, true, 16085);
    ack = sent(&peer, 16085);
    assert(led_sync_advance(&central, 6085));
    led_sync_receive(&central, &ack, 6090);
    led_sync_rendered(&central, false, 6095);
    assert(!central.have_active && central.stage == LED_SYNC_LINKED);
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    assert(led_sync_advance(&central, 6085));
    led_sync_rendered(&central, true, 6085);
    assert(central.stage == LED_SYNC_RUNNING_UNCONFIRMED); /* No peer completion. */
    puts("stage requires exact peer and own completed render: PASS");
}
static void test_candidate_challenges_same_ms(void) {
    fresh(0);
    clock_exchange(0);
    accept_state(0);
    apply_both(0, true);
    struct led_sync_packet next = probe;
    next.central_epoch = central.epoch + 1;
    next.sequence = 1;
    next.data.clock.t1 = 7000;
    led_sync_receive(&peer, &next, 17010);
    struct led_sync_packet older = sent(&peer, 17010);
    next.central_epoch++;
    led_sync_receive(&peer, &next, 17010); /* Deliberately same integer-ms captures. */
    struct led_sync_packet newer = sent(&peer, 17010);
    assert(newer.peer_epoch > older.peer_epoch);
    struct led_sync_packet oldfit = older;
    oldfit.kind = LED_SYNC_FIT;
    oldfit.data.clock.t4 = 7020;
    led_sync_receive(&peer, &oldfit, 17030);
    assert(peer.central_epoch == central.epoch && peer.have_active);
    next.central_epoch--;
    next.sequence = 2;
    led_sync_receive(&peer, &next, 17010); /* Lower candidate epoch cannot replace newest. */
    assert(!peer.reply_pending && peer.candidate_epoch == newer.central_epoch);
    struct led_sync_packet newfit = newer;
    newfit.kind = LED_SYNC_FIT;
    newfit.data.clock.t4 = 7020;
    led_sync_receive(&peer, &newfit, 17030);
    assert(peer.central_epoch == newer.central_epoch && peer.epoch == newer.peer_epoch);
    assert(peer.clock_ready && peer.have_active && !peer.active.valid && !peer.pending.valid);
    assert(peer.stage == LED_SYNC_COASTING);
    puts("same-ms competing candidate challenge replay: PASS");
}
static void test_continuous_changes_refresh(void) {
    struct queued {
        struct led_sync_packet p;
        uint64_t due;
        bool to_peer;
    } queue[16];
    unsigned count = 0, total = 0, clocks = 0;
    uint64_t last_fit = 0, max_fit_gap = 0;
    bool running = false;
    fresh(0);
    for (uint64_t now = 0; now <= 30000; now += 10) {
        for (unsigned i = 0; i < count;) {
            if (queue[i].due > now) {
                i++;
                continue;
            }
            struct queued q = queue[i];
            memmove(&queue[i], &queue[i + 1], (count - i - 1) * sizeof(queue[0]));
            count--;
            led_sync_receive(q.to_peer ? &peer : &central, &q.p, now + (q.to_peer ? 10000 : 0));
            if (!q.to_peer && q.p.kind == LED_SYNC_FIT_ACK &&
                central.clock_timeout == LED_SYNC_NEVER) {
                if (last_fit && now - last_fit > max_fit_gap)
                    max_fit_gap = now - last_fit;
                last_fit = now;
                clocks++;
            }
        }
        struct led_sync_values v = initial;
        v.brightness = (uint8_t)((now / 10) % 101);
        v.speed = (uint16_t)(10 + (now / 10) % 391);
        bool had_pending = central.pending.valid;
        uint64_t frozen = central.pending.local_deadline;
        led_sync_desire(&central, v, false);
        led_sync_reserve(&central, now);
        if (had_pending)
            assert(central.pending.local_deadline == frozen);
        if (led_sync_advance(&central, now))
            led_sync_rendered(&central, true, now);
        if (led_sync_advance(&peer, now + 10000))
            led_sync_rendered(&peer, true, now + 10000);
        if (central.have_active && peer.have_active)
            running = true;
        if (running)
            assert(central.have_active && peer.have_active);
        for (unsigned role = 0; role < 2; role++) {
            struct led_sync_packet p;
            if (led_sync_send(role ? &peer : &central, now + (role ? 10000 : 0), &p)) {
                p.source = 1;
                assert(count < sizeof(queue) / sizeof(queue[0]));
                queue[count++] = (struct queued){p, now + 10, role == 0};
                total++;
            }
        }
    }
    assert(running && clocks >= 3 && clocks <= 5 && max_fit_gap <= 8500 && total < 250);
    printf("continuous30s changes: fits=%u maxgap=%llu packets=%u queued=%u PASS\n", clocks,
           (unsigned long long)max_fit_gap, total, count);
}
static void test_storage_budget(void) {
    /* Both supported firmware/native ABIs fit within this static engine budget.
     * Presentation keeps its frozen clock in active.clock, independently of fit. */
    assert(sizeof(struct led_sync_engine) <= 792);
    assert(sizeof(struct led_sync_packet) == 64);
    printf("led sync storage: engine=%zu fit=%zu bytes PASS\n",
           sizeof(struct led_sync_engine), sizeof(struct led_sync_clock_fit));
}
int main(void) {
    test_storage_budget();
    test_scheduled_and_desired();
    test_duplicate_reordered_and_shape();
    test_late_retry_and_boundaries();
    test_expiry_quality_and_frozen_mapping();
    test_reconnect_and_pm_fences();
    test_bounded_traffic_and_loss();
    test_ack_identity_render_failure_and_long_uptime();
    test_render_completion_and_reset_recovery();
    test_unilateral_sessions();
    test_completion_quality_boundary();
    test_completion_stage_identity();
    test_candidate_challenges_same_ms();
    test_continuous_changes_refresh();
    puts("led sync protocol: PASS");
    return 0;
}
