/* SPDX-License-Identifier: MIT
 * Version4 relay codec. Internal packed objects never supply the wire ABI.
 * The complete event, including its source byte, is exactly56 bytes.
 */
#pragma once
#include "led_sync_protocol.h"
#include <stddef.h>

#define LED_SYNC_WIRE_SIZE 56U
#define LED_SYNC_WIRE_VERSION 4U
_Static_assert(LED_SYNC_VERSION == LED_SYNC_WIRE_VERSION, "LED engine/wire version mismatch");

static inline uint64_t led_sync_wire_get(const uint8_t *p, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)p[i] << (8U * i);
    return value;
}
static inline void led_sync_wire_put(uint8_t *p, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        p[i] = (uint8_t)(value >> (8U * i));
}
static inline bool led_sync_wire_valid(const struct led_sync_packet *p) {
    if (p->version != LED_SYNC_WIRE_VERSION || !led_sync_packet_shape(p))
        return false;
    if (p->kind == LED_SYNC_STATE || p->kind == LED_SYNC_STATE_ACK) {
        const struct led_sync_state_wire *s = &p->data.applied.state;
        struct led_sync_values v = led_sync_wire_values(s);
        return led_sync_values_valid(&v) && s->apply_at <= INT64_MAX &&
               s->phase_epoch <= s->apply_at;
    }
    return p->data.clock.t1 <= INT64_MAX && p->data.clock.t2 <= INT64_MAX &&
           p->data.clock.t3 <= INT64_MAX && p->data.clock.t4 <= INT64_MAX;
}
/* Buffers/objects must not overlap. A writable result is cleared on failure.
 * size must equal56; no truncated or extended representation is accepted.
 * Session/source/sequence admission remains the production engine's job.
 */
static inline bool led_sync_wire_encode(uint8_t *out, size_t size,
                                        const struct led_sync_packet *p) {
    if (!out)
        return false;
    if (size != LED_SYNC_WIRE_SIZE) {
        memset(out, 0, size);
        return false;
    }
    memset(out, 0, size);
    if (!p || !led_sync_wire_valid(p))
        return false;
    out[0] = p->source;
    out[1] = p->version;
    out[2] = p->kind;
    out[3] = p->status;
    led_sync_wire_put(out + 4, p->central_boot, 4);
    led_sync_wire_put(out + 8, p->central_epoch, 4);
    led_sync_wire_put(out + 12, p->sequence, 4);
    led_sync_wire_put(out + 16, p->peer_boot, 4);
    led_sync_wire_put(out + 20, p->peer_epoch, 4);
    if (p->kind == LED_SYNC_STATE || p->kind == LED_SYNC_STATE_ACK) {
        const struct led_sync_state_wire *s = &p->data.applied.state;
        led_sync_wire_put(out + 24, s->clock_sequence, 4);
        led_sync_wire_put(out + 28, s->apply_at, 8);
        led_sync_wire_put(out + 36, s->phase_epoch, 8);
        out[44] = (uint8_t)(s->pattern | ((s->flags & 7U) << 5));
        out[45] = (uint8_t)(s->brightness | ((s->flags & 8U) << 4));
        led_sync_wire_put(out + 46, s->speed, 2);
        if (p->kind == LED_SYNC_STATE_ACK)
            led_sync_wire_put(out + 48, p->data.applied.actual_local, 8);
    } else {
        led_sync_wire_put(out + 24, p->data.clock.t1, 8);
        led_sync_wire_put(out + 32, p->data.clock.t2, 8);
        led_sync_wire_put(out + 40, p->data.clock.t3, 8);
        led_sync_wire_put(out + 48, p->data.clock.t4, 8);
    }
    return true;
}
static inline bool led_sync_wire_decode(struct led_sync_packet *out, const uint8_t *bytes,
                                        size_t size) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!bytes || size != LED_SYNC_WIRE_SIZE)
        return false;
    struct led_sync_packet p = {0};
    p.source = bytes[0];
    p.version = bytes[1];
    p.kind = bytes[2];
    p.status = bytes[3];
    p.central_boot = (uint32_t)led_sync_wire_get(bytes + 4, 4);
    p.central_epoch = (uint32_t)led_sync_wire_get(bytes + 8, 4);
    p.sequence = (uint32_t)led_sync_wire_get(bytes + 12, 4);
    p.peer_boot = (uint32_t)led_sync_wire_get(bytes + 16, 4);
    p.peer_epoch = (uint32_t)led_sync_wire_get(bytes + 20, 4);
    if (p.kind == LED_SYNC_STATE || p.kind == LED_SYNC_STATE_ACK) {
        struct led_sync_state_wire *s = &p.data.applied.state;
        s->clock_sequence = (uint32_t)led_sync_wire_get(bytes + 24, 4);
        s->apply_at = led_sync_wire_get(bytes + 28, 8);
        s->phase_epoch = led_sync_wire_get(bytes + 36, 8);
        s->pattern = bytes[44] & 31U;
        s->brightness = bytes[45] & 127U;
        s->flags = (uint8_t)((bytes[44] >> 5) | ((bytes[45] >> 4) & 8U));
        s->speed = (uint16_t)led_sync_wire_get(bytes + 46, 2);
        if (p.kind == LED_SYNC_STATE_ACK)
            p.data.applied.actual_local = led_sync_wire_get(bytes + 48, 8);
        else
            for (unsigned i = 48; i < LED_SYNC_WIRE_SIZE; i++)
                if (bytes[i])
                    return false;
    } else {
        p.data.clock.t1 = led_sync_wire_get(bytes + 24, 8);
        p.data.clock.t2 = led_sync_wire_get(bytes + 32, 8);
        p.data.clock.t3 = led_sync_wire_get(bytes + 40, 8);
        p.data.clock.t4 = led_sync_wire_get(bytes + 48, 8);
    }
    if (!led_sync_wire_valid(&p))
        return false;
    *out = p;
    return true;
}
