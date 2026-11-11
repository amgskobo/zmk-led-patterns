/* SPDX-License-Identifier: MIT
 * Real production engines and codec, controlled monotonic clocks/transport.
 * No physical oscillator, PWM or radio measurements are implied.
 */
#include "led_sync_wire.h"
#include <assert.h>
#include <stdio.h>

static struct led_sync_engine central, peer;
static const struct led_sync_values initial = {1, 23, 100, 14};
static uint64_t now;
static unsigned count, packets, kinds[8], applies[2];
static bool fast_peer, drop_all, drop_applied, drop_fit_ack, fail_central, fail_peer, quiet;
static struct led_sync_packet last_request;
static bool check_receipt_stage;
static struct queued {
    struct led_sync_packet packet;
    uint64_t due;
    bool peer;
} queue[32];
static uint64_t peer_now(uint64_t time) { return time + 10000U + (fast_peer ? time / 1000U : 0U); }
static void reset(bool drift) {
    now = 0;
    count = packets = 0;
    memset(kinds, 0, sizeof(kinds));
    memset(applies, 0, sizeof(applies));
    fast_peer = drift;
    check_receipt_stage = false;
    drop_all = drop_applied = fail_central = fail_peer = quiet = false;
    drop_fit_ack = false;
    led_sync_init(&central, true, 111, 2, initial);
    led_sync_init(&peer, false, 222, 2, initial);
    led_sync_link(&central, true, now, 333);
    led_sync_link(&peer, true, peer_now(now), 444);
}
static void step(void) {
    for (unsigned i = 0; i < count;) {
        if (queue[i].due > now) {
            i++;
            continue;
        }
        struct queued q = queue[i];
        memmove(&queue[i], &queue[i + 1], (--count - i) * sizeof(queue[0]));
        (void)led_sync_receive(q.peer ? &peer : &central, &q.packet, q.peer ? peer_now(now) : now);
        struct led_sync_engine *received = q.peer ? &peer : &central;
        if (check_receipt_stage && received->have_active &&
            (!received->active.valid ||
             received->active.sequence <= received->resume_after_sequence))
            assert(led_sync_coasting(received));
        printf("rx %llu %u %u %u %u\n", (unsigned long long)now, q.peer, q.packet.kind,
               q.packet.status, q.packet.sequence);
    }
    for (unsigned role = 0; role < 2; role++) {
        struct led_sync_engine *e = role ? &peer : &central;
        uint64_t local = role ? peer_now(now) : now;
        enum led_sync_stage stage = e->stage;
        if (led_sync_advance(e, local)) {
            bool ok = !(role ? fail_peer : fail_central);
            led_sync_rendered(e, ok, local);
            applies[role]++;
            printf("draw %llu %u %u %u %u %llu\n", (unsigned long long)now, role,
                   e->active.sequence, ok, e->visible.brightness,
                   (unsigned long long)e->phase_epoch);
        }
        if (stage != e->stage)
            printf("stage %llu %u %u %u\n", (unsigned long long)now, role, stage, e->stage);
        struct led_sync_packet packet, decoded;
        if (!led_sync_send(e, local, &packet))
            continue;
        printf("tx %llu %u %u %u %u\n", (unsigned long long)now, role, packet.kind, packet.status,
               packet.sequence);
        assert(!quiet);
        assert(packet.kind >= LED_SYNC_PROBE && packet.kind <= LED_SYNC_REQUEST);
        packets++;
        kinds[packet.kind]++;
        uint8_t bytes[LED_SYNC_WIRE_SIZE];
        assert(led_sync_wire_encode(bytes, sizeof(bytes), &packet));
        bytes[0] = 1; /* Actual relay replaces source on receipt. */
        assert(led_sync_wire_decode(&decoded, bytes, sizeof(bytes)));
        if (decoded.kind == LED_SYNC_REQUEST)
            last_request = decoded;
        if (drop_all || (drop_fit_ack && decoded.kind == LED_SYNC_FIT_ACK) ||
            (drop_applied && decoded.kind == LED_SYNC_STATE_ACK &&
             decoded.status == LED_SYNC_APPLIED))
            continue;
        assert(count < sizeof(queue) / sizeof(queue[0]));
        queue[count++] = (struct queued){decoded, now + 10, role == 0};
    }
    now += 10;
}
static void until(uint64_t end) {
    while (now < end)
        step();
}
static void started(bool drift) {
    reset(drift);
    until(7000);
    assert(central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
    assert(central.visible.brightness == 23 && peer.visible.brightness == 23);
}
static void desire(uint8_t brightness) {
    struct led_sync_values v = central.desired;
    v.brightness = brightness;
    led_sync_desire(&central, v, false);
    led_sync_reserve(&central, now);
}
static void test_hour_idle_and_fresh_reuse(void) {
    started(false);
    unsigned probes = kinds[LED_SYNC_PROBE];
    desire(41);
    until(8500);
    assert(central.visible.brightness == 41 && peer.visible.brightness == 41);
    assert(kinds[LED_SYNC_PROBE] == probes);
    unsigned frozen = packets;
    quiet = true;
    for (unsigned i = 0; i < 1000; i++)
        desire(41); /* No-op requests do not rearm. */
    uint64_t previous = peer.last_logical;
    until(3600000);
    assert(packets == frozen && central.have_active && peer.have_active);
    assert(central.stage == LED_SYNC_COASTING && peer.stage == LED_SYNC_COASTING);
    assert(peer.last_logical > previous + 3500000);
    assert(led_sync_next_tx(&central, now) == LED_SYNC_NEVER);
    assert(led_sync_next_tx(&peer, peer_now(now)) == LED_SYNC_NEVER);
    assert(led_sync_next_render(&central, now) == LED_SYNC_NEVER);
    assert(led_sync_next_render(&peer, peer_now(now)) == LED_SYNC_NEVER);
    puts("hour idle zero traffic, continuing phase, fresh fit reuse: PASS");
}
static void test_stale_backward_future_reset(void) {
    started(true);
    quiet = true;
    until(260000);
    quiet = false;
    struct led_sync_clock_fit old = peer.active.clock;
    desire(67);
    while (!peer.pending.valid && now < 261000)
        step();
    assert(peer.pending.valid && peer.pending.reset_phase);
    uint64_t edge = central.pending.wire.apply_at;
    assert(central.pending.wire.phase_epoch == edge);
    while (now + 10 < edge) {
        assert(peer.visible.brightness == 23 && central.visible.brightness == 23);
        step();
    }
    until(edge + 100);
    uint64_t old_projected;
    assert(led_sync_clock_to_central(&old, peer_now(now - 10), &old_projected));
    assert(peer.last_logical + 200 < old_projected);
    assert(peer.visible.brightness == 67 && central.visible.brightness == 67);
    assert(peer.phase_epoch == edge && central.phase_epoch == edge);
    assert(peer.phase_floor == 0 && central.phase_floor == 0);
    assert(peer.stage == LED_SYNC_SYNCED && central.stage == LED_SYNC_SYNCED);
    puts("long coast backward fit resets only at common future edge: PASS");
}
static void test_finite_failure_silence_and_rearm(void) {
    reset(false);
    drop_all = true;
    until(8000);
    assert(kinds[LED_SYNC_PROBE] == 5 && central.exhausted);
    quiet = true;
    until(3600000);
    assert(packets == 5 && led_sync_next_tx(&central, now) == LED_SYNC_NEVER);
    quiet = false;
    drop_all = false;
    desire(74);
    until(now + 4000);
    assert(central.visible.brightness == 74 && peer.visible.brightness == 74);
    assert(central.stage == LED_SYNC_SYNCED);
    puts("five total probes then hour silence; new real setting rearms: PASS");
}
static void test_rollback_preserves_newer_desired(void) {
    started(true);
    quiet = true;
    until(260000);
    quiet = false;
    desire(59);
    while (!peer.pending.valid && now < 261000)
        step();
    assert(peer.pending.valid);
    uint64_t failed_edge = central.pending.wire.apply_at;
    desire(73); /* Later intent while B is immutable. */
    fail_peer = true;
    until(failed_edge + 10);
    assert(peer.have_active && peer.visible.brightness == 23);
    assert(central.desired.brightness == 73);
    assert(!central.pending.valid); /* Own completion must precede next plan. */
    fail_peer = false;
    until(failed_edge + 100);
    assert(central.pending.valid && peer.pending.valid);
    assert(central.pending.wire.phase_epoch == central.pending.wire.apply_at);
    assert(peer.visible.brightness == 23);
    until(failed_edge + 1500);
    assert(central.visible.brightness == 73 && peer.visible.brightness == 73);
    assert(peer.stage == LED_SYNC_SYNCED && central.stage == LED_SYNC_SYNCED);
    puts("failed new phase rolls back old view and preserves latest intent: PASS");
}
static void test_three_future_attempts_then_silence(void) {
    started(false);
    fail_central = true;
    unsigned old = applies[0];
    desire(66);
    until(14000);
    assert(applies[0] == old + 3 && central.exhausted);
    assert(central.have_active && central.visible.brightness == 23);
    quiet = true;
    until(120000);
    quiet = false;
    fail_central = false;
    desire(67);
    until(now + 4000);
    assert(central.visible.brightness == 67 && peer.visible.brightness == 67);
    puts("three future attempts preserve old own-success then stop: PASS");
}
static void test_unconfirmed_phase_wait(void) {
    started(false);
    drop_applied = true;
    struct led_sync_values v = central.desired;
    v.pattern = 2;
    v.brightness = 40;
    led_sync_desire(&central, v, true);
    led_sync_reserve(&central, now);
    uint64_t first = central.pending.wire.apply_at;
    desire(59);
    until(first + 10);
    assert(!central.pending.valid && central.confirmation_due == first + 200);
    until(first + 200);
    assert(!central.pending.valid);
    step();
    assert(central.pending.valid &&
           central.pending.wire.phase_epoch == central.pending.wire.apply_at);
    until(first + 1600);
    quiet = true;
    until(first + 120000);
    assert(central.have_active && central.visible.brightness == 59);
    assert(central.stage == LED_SYNC_COASTING_UNCONFIRMED && peer.stage == LED_SYNC_COASTING);
    struct led_sync_packet ack =
        led_sync_packet_new(&peer, LED_SYNC_STATE_ACK, central.active.sequence);
    ack.source = 1;
    ack.status = LED_SYNC_APPLIED;
    ack.data.applied.state = central.active.wire;
    ack.data.applied.actual_local = peer.rendered_at;
    led_sync_receive(&central, &ack, now);
    assert(central.stage == LED_SYNC_COASTING);
    assert(led_sync_next_tx(&central, now) == LED_SYNC_NEVER);
    puts("queued new phase waits200ms; historical ACK never restores fresh sync: PASS");
}
static void test_unilateral_resume_request(void) {
    started(false);
    unsigned before = kinds[LED_SYNC_PROBE];
    uint32_t central_epoch = central.epoch;
    peer.suspended = true;
    led_sync_link(&peer, true, peer_now(now), 777);
    peer.suspended = false;
    led_sync_link(&peer, true, peer_now(now), 778);
    led_sync_request_clock(&peer, peer_now(now));
    until(now + 2500);
    assert(central.epoch == central_epoch && kinds[LED_SYNC_PROBE] == before + 1);
    assert(central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
    assert(kinds[LED_SYNC_REQUEST] >= 1 && kinds[LED_SYNC_REQUEST] <= 5);
    quiet = true;
    for (unsigned i = 0; i < 20; i++)
        led_sync_receive(&central, &last_request, now);
    until(now + 30000);
    assert(central.stage == LED_SYNC_COASTING);
    puts("unilateral peripheral resume hint is bounded and replay fenced: PASS");
}
static void test_latest_intent_after_last_failed_attempt(void) {
    reset(false);
    drop_all = true;
    until(7010); /* A's fifth PROBE has already used its old budget. */
    assert(kinds[LED_SYNC_PROBE] == 5 && central.clock_attempts == 5);
    desire(74);
    assert(central.clock_attempts == 5);
    until(11000);
    assert(kinds[LED_SYNC_PROBE] == 10 && central.exhausted);
    assert(central.transaction_desired.brightness == 74);
    quiet = true;
    desire(74);
    until(60000);
    assert(packets == 10);
    reset(false);
    drop_all = true;
    until(7010);
    desire(74);
    desire(23); /* A->B->A coalesces back to the same failed intent. */
    until(8000);
    assert(central.exhausted && packets == 5);
    quiet = true;
    until(60000);
    started(false);
    fail_central = true;
    unsigned completed = applies[0];
    desire(66);
    while (applies[0] < completed + 2 && now < 11000)
        step();
    assert(central.pending.valid && central.plan_attempts == 3);
    desire(74);
    assert(central.plan_attempts == 3);
    until(20000);
    assert(applies[0] == completed + 6 && central.exhausted);
    assert(central.transaction_desired.brightness == 74);
    quiet = true;
    until(60000);
    puts("last-attempt newer intent promotes once; unchanged and A-B-A stay silent: PASS");
}
static void test_confirmed_latest_and_duplicate_completion(void) {
    started(false);
    unsigned old_states = kinds[LED_SYNC_STATE];
    drop_applied = true;
    desire(40);
    uint64_t a_edge = central.pending.wire.apply_at;
    desire(59);
    until(a_edge + 10);
    assert(central.pending.valid && central.plan_attempts == 2);
    uint64_t b_edge = central.pending.wire.apply_at;
    drop_applied = false;
    until(b_edge + 100);
    assert(central.active_rendered && central.active.confirmed);
    assert(!central.transaction_active && !central.dirty && !central.pending.valid);
    assert(central.visible.brightness == 59 && kinds[LED_SYNC_STATE] == old_states + 2);
    quiet = true;
    until(now + 20000);

    started(false);
    drop_applied = true;
    struct led_sync_values v = central.desired;
    v.pattern = 2;
    v.brightness = 40;
    led_sync_desire(&central, v, true);
    led_sync_reserve(&central, now);
    a_edge = central.pending.wire.apply_at;
    desire(59);
    until(a_edge + 210);
    assert(central.pending.valid && central.plan_attempts == 2);
    b_edge = central.pending.wire.apply_at;
    desire(40); /* Latest returns A while immutable B still must finish. */
    struct led_sync_packet ack =
        led_sync_packet_new(&peer, LED_SYNC_STATE_ACK, central.active.sequence);
    ack.source = 1;
    ack.status = LED_SYNC_APPLIED;
    ack.data.applied.state = central.active.wire;
    ack.data.applied.actual_local = peer.rendered_at;
    led_sync_receive(&central, &ack, now);
    assert(central.completed_sequence == ack.sequence && central.plan_attempts == 1);
    /* Explicit independent accounting pressure proves duplicate historical
     * completion cannot refill another budget, even before B becomes active. */
    central.clock_attempts = 4;
    led_sync_receive(&central, &ack, now);
    assert(central.clock_attempts == 4 && central.plan_attempts == 1);
    until(b_edge + 220);
    assert(central.active.sequence != ack.sequence && central.pending.valid);
    unsigned plans = central.plan_attempts;
    led_sync_receive(&central, &ack, now);
    assert(central.plan_attempts == plans && !central.active.confirmed);
    puts("confirmed latest avoids promotion; current completion once, obsolete ACK inert: PASS");
}
static void test_immediate_session_stage_keeps_old_view(void) {
    started(false);
    struct led_sync_packet request = led_sync_packet_new(&peer, LED_SYNC_REQUEST, 1);
    request.source = 1;
    request.central_boot = request.central_epoch = 0;
    request.peer_epoch++;
    uint64_t old_phase = central.phase_epoch, old_floor = central.phase_floor;
    led_sync_receive(&central, &request, now);
    assert(central.have_active && central.active.valid); /* Local completion remains possible. */
    assert(central.visible.brightness == 23 && central.phase_epoch == old_phase &&
           central.phase_floor == old_floor);
    assert(central.stage == LED_SYNC_COASTING); /* No draw has run. */
    started(false);
    check_receipt_stage = true;
    led_sync_link(&central, true, now, 789);
    until(now + 6200);
    assert(central.have_active && peer.have_active);
    puts("session invalidation immediately coasts before drawing; old view retained: PASS");
}
static void test_unacknowledged_fit_cannot_fund_promoted_intent(void) {
    started(false);
    quiet = true;
    until(14000);
    quiet = false;
    unsigned states = kinds[LED_SYNC_STATE];
    drop_fit_ack = true;
    desire(40);
    until(16100); /* Five samples, but every FIT_ACK was lost. */
    assert(central.clock_attempts == 5);
    desire(59); /* This legitimate latest-next gets one new finite budget. */
    until(16600);
    assert(!central.pending.valid && kinds[LED_SYNC_STATE] == states);
    drop_fit_ack = false;
    until(now + 3000);
    assert(central.visible.brightness == 59 && peer.visible.brightness == 59);
    puts("unacknowledged candidate cannot fund promoted latest intent: PASS");
}
static void test_resume_after_last_unresolved_attempt(bool plans) {
    started(false);
    if (plans) {
        fail_central = true;
        desire(40);
        while (central.plan_attempts < LED_SYNC_PLAN_ATTEMPTS)
            step();
        assert(central.pending.valid && central.transaction_active);
    } else {
        quiet = true;
        until(14000); /* The old fit is stale before this setting transaction. */
        quiet = false;
        drop_all = true;
        desire(40);
        while (central.clock_attempts < LED_SYNC_CLOCK_ATTEMPTS)
            step();
        assert(central.clock_timeout > now && central.transaction_active);
    }
    unsigned before = kinds[LED_SYNC_PROBE];
    /* Resume is a new lifecycle event, not a repeated setting or old hint.
     * Its REQUEST arrives after the last old PROBE was lost during suspend. */
    peer.suspended = true;
    led_sync_link(&peer, true, peer_now(now), 901);
    peer.suspended = false;
    led_sync_link(&peer, true, peer_now(now), 902);
    led_sync_request_clock(&peer, peer_now(now));
    struct led_sync_packet request;
    assert(led_sync_send(&peer, peer_now(now), &request));
    assert(request.kind == LED_SYNC_REQUEST);
    request.source = 1;
    led_sync_receive(&central, &request, now);
    assert(central.hint_epoch == request.peer_epoch);
    drop_all = fail_central = false; /* Transport and output recover after resume. */
    until(now + 5000);
    assert(central.visible.brightness == 40 && peer.visible.brightness == 40);
    assert(central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
    assert(kinds[LED_SYNC_PROBE] > before &&
           kinds[LED_SYNC_PROBE] <= before + LED_SYNC_CLOCK_ATTEMPTS);
    printf("newer resume after final unresolved %s gets one bounded recovery: PASS\n",
           plans ? "plan" : "probe");
}
static struct led_sync_packet resume_peer_request(void) {
    peer.suspended = true;
    led_sync_link(&peer, true, peer_now(now), 903);
    peer.suspended = false;
    led_sync_link(&peer, true, peer_now(now), 904);
    led_sync_request_clock(&peer, peer_now(now));
    struct led_sync_packet request;
    assert(led_sync_send(&peer, peer_now(now), &request));
    assert(request.kind == LED_SYNC_REQUEST);
    request.source = 1;
    led_sync_receive(&central, &request, now);
    assert(central.hint_epoch == request.peer_epoch);
    return request;
}
static void test_old_pending_completion_keeps_resume_demand(void) {
    started(false);
    desire(40);
    uint64_t edge = central.pending.local_deadline;
    until(edge);
    /* Peer success arrives before the central finishes the same old plan. */
    assert(led_sync_advance(&peer, peer_now(now)));
    led_sync_rendered(&peer, true, peer_now(now));
    struct led_sync_packet applied;
    assert(led_sync_send(&peer, peer_now(now), &applied));
    assert(applied.status == LED_SYNC_APPLIED);
    applied.source = 1;
    led_sync_receive(&central, &applied, now);
    assert(central.pending.confirmed);
    struct led_sync_packet request = resume_peer_request();
    assert(led_sync_advance(&central, now));
    led_sync_rendered(&central, true, now);
    assert(central.stage == LED_SYNC_COASTING); /* New peer epoch invalidates old success. */
    assert(central.dirty && central.transaction_active); /* Old success cannot consume resume. */
    unsigned before = kinds[LED_SYNC_PROBE];
    until(now + 2500);
    assert(central.have_active && peer.have_active && central.stage == LED_SYNC_SYNCED &&
           peer.stage == LED_SYNC_SYNCED);
    assert(kinds[LED_SYNC_PROBE] == before + 1);
    unsigned packet_count = packets;
    quiet = true;
    for (unsigned i = 0; i < 20; i++)
        led_sync_receive(&central, &request, now);
    until(now + 60000);
    assert(packets == packet_count);
    puts("old pending success preserves unsent resume; duplicate hint stays quiet: PASS");
}
static void test_post_hint_probe_before_old_completion_keeps_intent(void) {
    started(false);
    desire(40);
    uint64_t edge = central.pending.local_deadline;
    until(edge);
    assert(led_sync_advance(&peer, peer_now(now)));
    led_sync_rendered(&peer, true, peer_now(now));
    struct led_sync_packet packet;
    assert(led_sync_send(&peer, peer_now(now), &packet));
    assert(packet.status == LED_SYNC_APPLIED);
    packet.source = 1;
    led_sync_receive(&central, &packet, now);
    assert(central.pending.confirmed);
    (void)resume_peer_request();
    assert(led_sync_advance(&central, now));
    uint32_t old_sequence = central.active.sequence;
    /* Real render completion is outside the metadata mutex. TX may extract
     * the new challenge while the old immutable PWM call is still running. */
    assert(led_sync_send(&central, now, &packet));
    assert(packet.kind == LED_SYNC_PROBE);
    packet.source = 1;
    led_sync_receive(&peer, &packet, peer_now(now));
    led_sync_rendered(&central, true, now);
    assert(central.dirty && central.transaction_active);
    until(now + 2500);
    assert(central.active.sequence > old_sequence && central.have_active && peer.have_active);
    assert(central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
    puts("post-hint challenge before old render completion preserves new-session intent: PASS");
}
static void test_failed_resume_demand_is_consumed_once(bool last_probe) {
    started(false);
    quiet = true;
    until(14000);
    quiet = false;
    drop_all = true;
    unsigned before = kinds[LED_SYNC_PROBE];
    desire(40);
    unsigned boundary = last_probe ? LED_SYNC_CLOCK_ATTEMPTS : LED_SYNC_CLOCK_ATTEMPTS - 1;
    while (central.clock_attempts < boundary)
        step();
    struct led_sync_packet request = resume_peer_request();
    until(now + 4000);
    unsigned expected = before + LED_SYNC_CLOCK_ATTEMPTS * (last_probe ? 2U : 1U);
    assert(kinds[LED_SYNC_PROBE] == expected && central.exhausted);
    assert(led_sync_next_tx(&central, now) == LED_SYNC_NEVER);
    unsigned packet_count = packets;
    quiet = true;
    for (unsigned i = 0; i < 20; i++)
        led_sync_receive(&central, &request, now);
    until(now + 60000);
    assert(packets == packet_count);
    /* A genuinely newer lifecycle event can start one new transaction. */
    quiet = drop_all = false;
    (void)resume_peer_request();
    until(now + 2500);
    assert(central.have_active && peer.have_active && central.visible.brightness == 40 &&
           peer.visible.brightness == 40 && central.stage == LED_SYNC_SYNCED &&
           peer.stage == LED_SYNC_SYNCED);
    printf("failed resume %s service is finite; duplicates quiet; newer event recovers: PASS\n",
           last_probe ? "promoted" : "remaining-budget");
}
static void test_resume_sequence_fence_resets_at_link_rotation(void) {
    started(false);
    (void)resume_peer_request();
    assert(central.resume_after_sequence == central.sequence);
    central.sequence = UINT32_MAX;
    (void)resume_peer_request();
    assert(central.resume_after_sequence == UINT32_MAX && central.stage == LED_SYNC_COASTING);
    struct led_sync_packet packet;
    assert(!led_sync_send(&central, now, &packet)); /* Rotate, without sequence+1 overflow. */
    assert(central.sequence == 0 && central.resume_after_sequence == 0 && !central.have_active);
    until(now + 6500);
    assert(central.have_active && peer.have_active && central.stage == LED_SYNC_SYNCED &&
           peer.stage == LED_SYNC_SYNCED);
    puts("resume sequence fence resets on link rotation, including UINT32_MAX: PASS");
}
static void test_historical_view_cannot_suppress_resumed_latest_intent(void) {
    started(false); /* The old confirmed presentation is23 on both halves. */
    quiet = true;
    until(14000);
    quiet = false;
    drop_all = true;
    unsigned before = kinds[LED_SYNC_PROBE];
    desire(40); /* This transaction's budget identity is40. */
    while (central.clock_attempts < LED_SYNC_CLOCK_ATTEMPTS - 1)
        step();
    (void)resume_peer_request();
    desire(23); /* A real newer setting, distinct from the40 transaction identity. */
    while (central.clock_attempts < LED_SYNC_CLOCK_ATTEMPTS)
        step(); /* The last post-hint challenge is lost, consuming the hint debt. */
    assert(central.transaction_desired.brightness == 40 && central.desired.brightness == 23);
    drop_all = false;
    until(now + 3000);
    assert(central.have_active && peer.have_active && central.visible.brightness == 23 &&
           peer.visible.brightness == 23 && central.stage == LED_SYNC_SYNCED &&
           peer.stage == LED_SYNC_SYNCED);
    assert(kinds[LED_SYNC_PROBE] == before + LED_SYNC_CLOCK_ATTEMPTS + 1);
    puts("historical pre-resume view cannot suppress one newer latest-value transaction: PASS");
}
static void test_resume_during_inflight_render_keeps_progress(bool succeeded) {
    started(false);
    desire(40);
    until(central.pending.local_deadline);
    assert(led_sync_advance(&central, now));
    uint32_t old_sequence = central.active.sequence;
    assert(!central.active_rendered && central.previous.valid);
    (void)resume_peer_request(); /* Listener runs while the real PWM call is outside the lock. */
    led_sync_rendered(&central, succeeded, now);
    if (succeeded) {
        assert(central.have_active && central.active_rendered && central.visible.brightness == 40);
        assert(central.stage == LED_SYNC_COASTING_UNCONFIRMED);
    } else {
        assert(central.have_active && central.active_rendered && central.visible.brightness == 23 &&
               central.replay_previous);
        assert(central.stage == LED_SYNC_COASTING);
    }
    assert(central.dirty && central.transaction_active);
    until(now + 2500);
    assert(central.have_active && peer.have_active && central.active.sequence > old_sequence &&
           central.visible.brightness == 40 && peer.visible.brightness == 40 &&
           central.stage == LED_SYNC_SYNCED && peer.stage == LED_SYNC_SYNCED);
    printf(
        "REQUEST during in-flight render %s keeps completion/rollback and future recovery: PASS\n",
        succeeded ? "success" : "failure");
}
static void test_new_session_retires_obsolete_render(bool has_previous, bool succeeded) {
    if (has_previous) {
        started(false);
        quiet = true;
        until(14000);
        quiet = false;
    } else {
        reset(false);
    }
    desire(40);
    while (!central.pending.valid)
        step();
    uint64_t old_edge = central.pending.wire.apply_at;
    until(old_edge);
    assert(led_sync_advance(&peer, peer_now(now)));
    assert(!peer.active_rendered && peer.previous.valid == has_previous);
    struct led_sync_clock_fit old_fit = peer.active.clock;
    /* Only the central restarts. The untouched peer's old PWM call remains
     * outside its mutex while the new session's real startup exchange runs. */
    central.suspended = true;
    led_sync_link(&central, true, now, 905);
    central.suspended = false;
    led_sync_link(&central, true, now, 906);
    while (!peer.clock_ready || peer.central_epoch != central.epoch || !peer.pending.valid)
        step();
    assert(!peer.active.valid && !peer.active_rendered);
    assert(!peer.ack_pending);
    uint64_t old_logical;
    assert(led_sync_clock_to_central(&old_fit, peer_now(now), &old_logical));
    assert(now - old_edge > LED_SYNC_LATE_MS && led_sync_clock_valid_at(&old_fit, old_logical, 0));
    struct led_sync_plan next_plan = peer.pending;
    struct led_sync_clock_fit new_fit = peer.fit;
    uint32_t epoch = peer.epoch, state_sequence = peer.sequence;
    uint8_t clock_attempts = peer.clock_attempts, plan_attempts = peer.plan_attempts;
    struct led_sync_values local_latest = peer.desired;
    local_latest.brightness = 71;
    led_sync_desire(&peer, local_latest, false);
    bool queued_ack = has_previous && succeeded;
    struct led_sync_packet accepted_ack = {0};
    if (queued_ack) {
        struct led_sync_packet duplicate =
            led_sync_packet_new(&central, LED_SYNC_STATE, peer.pending.sequence);
        duplicate.source = 1;
        duplicate.data.applied.state = peer.pending.wire;
        led_sync_receive(&peer, &duplicate, peer_now(now));
        assert(peer.ack_pending && peer.ack.status == LED_SYNC_ACCEPTED);
        accepted_ack = peer.ack;
    }
    led_sync_rendered(&peer, succeeded, peer_now(now));
    bool recovered = has_previous
                         ? peer.have_active && peer.active_rendered && !peer.active.valid &&
                               peer.visible.brightness == 23 && peer.stage == LED_SYNC_COASTING
                         : !peer.have_active && peer.stage == LED_SYNC_LINKED;
    recovered = recovered && peer.replay_previous && !peer.previous.valid;
    if (recovered) {
        /* The adapter consumes the redraw flag once. A duplicate completion
         * must keep an already completed invalid/coasting A (or empty status). */
        peer.replay_previous = false;
        uint32_t restored_sequence = peer.active.sequence;
        led_sync_rendered(&peer, !succeeded, peer_now(now) + 1);
        assert(!peer.replay_previous && peer.ack_pending == queued_ack &&
               peer.active.sequence == restored_sequence && peer.have_active == has_previous);
        if (queued_ack)
            assert(memcmp(&peer.ack, &accepted_ack, sizeof(accepted_ack)) == 0);
        if (has_previous)
            assert(peer.active_rendered && !peer.active.valid && peer.visible.brightness == 23 &&
                   peer.stage == LED_SYNC_COASTING);
    }
    fprintf(stderr,
            "obsolete render at%llu oldedge%llu previous%u succeeded%u recovered%u next%u\n",
            (unsigned long long)now, (unsigned long long)old_edge, has_previous, succeeded,
            recovered, next_plan.sequence);
    /* No old-session ACK is created; an already queued new-session ACK survives. */
    assert(peer.ack_pending == queued_ack);
    if (queued_ack)
        assert(memcmp(&peer.ack, &accepted_ack, sizeof(accepted_ack)) == 0);
    assert(peer.pending.valid && peer.pending.sequence == next_plan.sequence &&
           peer.pending.local_deadline == next_plan.local_deadline &&
           led_sync_state_equal(&peer.pending.wire, &next_plan.wire));
    assert(peer.fit.valid && peer.fit.reference_ms == new_fit.reference_ms &&
           peer.fit.offset_ms == new_fit.offset_ms && peer.epoch == epoch &&
           peer.sequence == state_sequence && peer.central_epoch == central.epoch &&
           peer.clock_attempts == clock_attempts && peer.plan_attempts == plan_attempts &&
           peer.desired.brightness == 71);
    /* A later new-session failure must retain the older own-successful A. */
    uint64_t next_edge = peer.pending.wire.apply_at;
    fail_peer = true;
    until(next_edge + 10);
    if (has_previous)
        assert(peer.have_active && peer.active_rendered && !peer.active.valid &&
               peer.visible.brightness == 23 && peer.stage == LED_SYNC_COASTING);
    else
        assert(!peer.have_active && peer.stage == LED_SYNC_LINKED);
    assert(recovered);
    fail_peer = false;
    until(now + 2500);
    assert(central.have_active && peer.have_active && central.visible.brightness == 40 &&
           peer.visible.brightness == 40 && central.stage == LED_SYNC_SYNCED &&
           peer.stage == LED_SYNC_SYNCED && peer.desired.brightness == 71);
    printf("new-session obsolete render %s/%s preserves pending/state and fallback: PASS\n",
           has_previous ? "prior" : "no-prior", succeeded ? "success" : "failure");
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (strcmp(argv[1], "resume-old-completion") == 0)
            test_old_pending_completion_keeps_resume_demand();
        else if (strcmp(argv[1], "peer-obsolete-prior-success") == 0)
            test_new_session_retires_obsolete_render(true, true);
        else if (strcmp(argv[1], "peer-obsolete-prior-failure") == 0)
            test_new_session_retires_obsolete_render(true, false);
        else if (strcmp(argv[1], "peer-obsolete-none-success") == 0)
            test_new_session_retires_obsolete_render(false, true);
        else if (strcmp(argv[1], "peer-obsolete-none-failure") == 0)
            test_new_session_retires_obsolete_render(false, false);
        else if (strcmp(argv[1], "resume-inflight-success") == 0 ||
                 strcmp(argv[1], "resume-inflight-failure") == 0)
            test_resume_during_inflight_render_keeps_progress(
                strcmp(argv[1], "resume-inflight-success") == 0);
        else if (strcmp(argv[1], "resume-latest-historical") == 0)
            test_historical_view_cannot_suppress_resumed_latest_intent();
        else if (strcmp(argv[1], "resume-render-interleave") == 0)
            test_post_hint_probe_before_old_completion_keeps_intent();
        else {
            assert(strcmp(argv[1], "resume-probe") == 0 || strcmp(argv[1], "resume-plan") == 0);
            test_resume_after_last_unresolved_attempt(strcmp(argv[1], "resume-plan") == 0);
        }
        return 0;
    }
    assert(argc == 1);
    test_resume_after_last_unresolved_attempt(false);
    test_resume_after_last_unresolved_attempt(true);
    test_old_pending_completion_keeps_resume_demand();
    test_post_hint_probe_before_old_completion_keeps_intent();
    test_failed_resume_demand_is_consumed_once(true);
    test_failed_resume_demand_is_consumed_once(false);
    test_resume_sequence_fence_resets_at_link_rotation();
    test_historical_view_cannot_suppress_resumed_latest_intent();
    test_resume_during_inflight_render_keeps_progress(true);
    test_resume_during_inflight_render_keeps_progress(false);
    test_new_session_retires_obsolete_render(true, true);
    test_new_session_retires_obsolete_render(true, false);
    test_new_session_retires_obsolete_render(false, true);
    test_new_session_retires_obsolete_render(false, false);
    test_unacknowledged_fit_cannot_fund_promoted_intent();
    test_immediate_session_stage_keeps_old_view();
    test_hour_idle_and_fresh_reuse();
    test_stale_backward_future_reset();
    test_finite_failure_silence_and_rearm();
    test_rollback_preserves_newer_desired();
    test_three_future_attempts_then_silence();
    test_unconfirmed_phase_wait();
    test_unilateral_resume_request();
    test_latest_intent_after_last_failed_attempt();
    test_confirmed_latest_and_duplicate_completion();
    puts("led sync on demand: PASS");
    return 0;
}
