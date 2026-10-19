/* SPDX-License-Identifier: MIT
 * Fixed-input traffic comparison through two unchanged production engines.
 * The baseline switch is admitted only with the original version2 source.
 */
#include "led_sync_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef LED_TRAFFIC_BASELINE64
#include "led_sync_wire.h"
#endif

struct queued {
    struct led_sync_packet packet;
    uint64_t due;
    bool peer;
};
struct reservation {
    uint32_t boot, epoch, sequence;
    struct led_sync_state_wire wire;
};
static struct led_sync_values input_values(unsigned scenario, uint64_t time) {
    struct led_sync_values v = {1, 80, 100, LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF};
    if (scenario != 0) {
        v.brightness = (uint8_t)((time / 10) % 101);
        v.speed = (uint16_t)(10 + (time / 10) % 391);
    }
    return v;
}
static void scenario(unsigned scenario, uint64_t duration) {
    struct led_sync_engine c, p;
    const struct led_sync_values initial = {1, 80, 100, LED_SYNC_ACTIVE | LED_SYNC_IDLE_OFF};
    struct queued queue[32];
    struct reservation records[128];
    unsigned record_count = 0;
    unsigned count = 0, kinds[8] = {0}, sent = 0, lost = 0, applied[2] = {0};
    bool recovered = false;
    led_sync_init(&c, true, 111, 2, initial);
    led_sync_init(&p, false, 222, 2, initial);
    led_sync_link(&c, true, 0, 333);
    led_sync_link(&p, true, 10000, 444);
    printf("scenario %u duration %llu\n", scenario, (unsigned long long)duration);
    for (uint64_t now = 0; now <= duration; now += 10) {
        if (scenario == 2 && (now == 18000 || now == 20000)) {
            bool up = now == 20000;
            led_sync_link(&c, up, now, 555);
            led_sync_link(&p, up, now + 10000, 666);
            printf("link %llu %u\n", (unsigned long long)now, up);
        }
        for (unsigned i = 0; i < count;) {
            if (queue[i].due > now) {
                i++;
                continue;
            }
            struct queued q = queue[i];
            memmove(&queue[i], &queue[i + 1], (--count - i) * sizeof(queue[0]));
            bool accepted =
                led_sync_receive(q.peer ? &p : &c, &q.packet, now + (q.peer ? 10000U : 0U));
            printf("rx %llu %u %u %u %u %u\n", (unsigned long long)now, q.peer, q.packet.kind,
                   q.packet.status, q.packet.sequence, accepted);
        }
        if (scenario == 1 || scenario == 2) {
            struct led_sync_values v = initial;
            v.brightness = (uint8_t)((now / 10) % 101);
            v.speed = (uint16_t)(10 + (now / 10) % 391);
            bool pending = c.pending.valid;
            uint64_t deadline = c.pending.local_deadline;
            led_sync_desire(&c, v, false);
            led_sync_reserve(&c, now);
            if (pending)
                assert(c.pending.local_deadline == deadline);
        }
        for (unsigned role = 0; role < 2; role++) {
            struct led_sync_engine *e = role ? &p : &c;
            uint64_t local = now + (role ? 10000U : 0U);
            struct led_sync_plan before = e->pending;
            struct led_sync_values old_visible = e->visible;
            uint64_t old_phase = e->phase_epoch;
            bool applied_now = led_sync_advance(e, local);
            if (!before.valid || local < before.local_deadline) {
                assert(!applied_now);
                assert(led_sync_values_equal(&old_visible, &e->visible));
                assert(e->phase_epoch == old_phase);
            } else {
                /* These fixed queues admit and complete exactly at the
                 * reserved edge; totals cannot hide a delayed burst. */
                assert(local - before.local_deadline <= LED_SYNC_LATE_MS);
                assert(applied_now);
            }
            if (applied_now) {
                assert(before.valid && e->active.sequence == before.sequence);
                assert(led_sync_state_equal(&e->active.wire, &before.wire));
                struct led_sync_values expected = led_sync_wire_values(&before.wire);
                assert(led_sync_values_equal(&expected, &e->visible));
                assert(e->phase_epoch == before.wire.phase_epoch);
                unsigned index = 0;
                for (; index < record_count; index++)
                    if (records[index].boot == (role ? e->central_boot : e->boot) &&
                        records[index].epoch == (role ? e->central_epoch : e->epoch) &&
                        records[index].sequence == before.sequence)
                        break;
                assert(index < record_count &&
                       led_sync_state_equal(&records[index].wire, &before.wire));
                if (role) {
                    assert(c.have_active && c.active.sequence == e->active.sequence);
                    assert(led_sync_state_equal(&c.active.wire, &e->active.wire));
                }
                led_sync_rendered(e, true, local);
                applied[role]++;
                printf("apply %llu %u %u %llu %llu %u %u %u %u\n", (unsigned long long)now, role,
                       e->active.sequence, (unsigned long long)e->active.wire.apply_at,
                       (unsigned long long)e->phase_epoch, e->visible.pattern,
                       e->visible.brightness, e->visible.speed, e->stage);
            }
            struct led_sync_packet packet, decoded;
            if (!led_sync_send(e, local, &packet))
                continue;
            if (packet.kind == LED_SYNC_STATE) {
                unsigned index = 0;
                for (; index < record_count; index++)
                    if (records[index].boot == packet.central_boot &&
                        records[index].epoch == packet.central_epoch &&
                        records[index].sequence == packet.sequence)
                        break;
                if (index == record_count) {
                    struct led_sync_values value = led_sync_wire_values(&packet.data.applied.state);
                    struct led_sync_values current = input_values(scenario, now);
                    struct led_sync_values previous =
                        input_values(scenario, now >= 10 ? now - 10 : 0);
                    assert(led_sync_values_equal(&value, &current) ||
                           led_sync_values_equal(&value, &previous));
                    assert(packet.data.applied.state.apply_at > now);
                    assert(record_count < sizeof(records) / sizeof(records[0]));
                    records[record_count++] =
                        (struct reservation){packet.central_boot, packet.central_epoch,
                                             packet.sequence, packet.data.applied.state};
                    printf("reserved %llu %u %u %u %llu %llu %u %u %u\n", (unsigned long long)now,
                           packet.central_boot, packet.central_epoch, packet.sequence,
                           (unsigned long long)packet.data.applied.state.apply_at,
                           (unsigned long long)packet.data.applied.state.phase_epoch, value.pattern,
                           value.brightness, value.speed);
                } else {
                    assert(led_sync_state_equal(&records[index].wire, &packet.data.applied.state));
                }
            }
            unsigned bytes;
#ifdef LED_TRAFFIC_BASELINE64
            _Static_assert(LED_SYNC_VERSION == 2, "baseline requires original version2");
            bytes = sizeof(packet);
            decoded = packet;
#else
#ifdef LED_TRAFFIC_BASELINE56
            _Static_assert(LED_SYNC_VERSION == 3, "baseline56 requires original periodic version3");
#else
            _Static_assert(LED_SYNC_VERSION == 4, "candidate requires on-demand version4");
#endif
            uint8_t wire[LED_SYNC_WIRE_SIZE];
            assert(led_sync_wire_encode(wire, sizeof(wire), &packet));
            assert(led_sync_wire_decode(&decoded, wire, sizeof(wire)));
            bytes = sizeof(wire);
#endif
            assert(memcmp(&packet, &decoded, sizeof(packet)) == 0);
            unsigned chunks = (bytes + 3U + 58U) / 59U;
            bool drop = scenario == 2 && now >= 10000 && now < 18000 && role == 1 &&
                        (packet.kind == LED_SYNC_FIT_ACK || packet.kind == LED_SYNC_STATE_ACK);
            printf("tx %llu %u %u %u %u %u\n", (unsigned long long)now, role, packet.kind,
                   packet.status, packet.sequence, drop);
            printf("wire %u %u\n", bytes, chunks);
            assert(packet.kind > 0 && packet.kind < sizeof(kinds) / sizeof(kinds[0]));
            sent++;
            kinds[packet.kind]++;
            if (drop) {
                lost++;
                continue;
            }
            decoded.source = 1;
            assert(count < sizeof(queue) / sizeof(queue[0]));
            queue[count++] = (struct queued){decoded, now + 10, role == 0};
        }
        if (scenario == 2 && now > 26000 && c.stage == LED_SYNC_SYNCED &&
            p.stage == LED_SYNC_SYNCED)
            recovered = true;
    }
    assert(applied[0] && applied[1] && sent < 600);
    if (scenario == 2)
        assert(lost && recovered);
    assert(c.have_active && p.have_active && c.active_rendered && p.active_rendered);
    assert(c.active.sequence == p.active.sequence && c.phase_epoch == p.phase_epoch);
    assert(led_sync_values_equal(&c.visible, &p.visible));
    printf("final %u %u %u %u %u %llu\n", scenario, c.active.sequence, c.visible.pattern,
           c.visible.brightness, c.visible.speed, (unsigned long long)c.phase_epoch);
    printf("summary %u packets %u lost %u applies %u %u kinds", scenario, sent, lost, applied[0],
           applied[1]);
    for (unsigned kind = 1; kind <= 7; kind++)
        printf(" %u", kinds[kind]);
    puts(" PASS");
}
int main(void) {
    scenario(0, 60000);
    scenario(1, 30000);
    scenario(2, 45000);
    puts("led sync traffic: PASS");
    return 0;
}
