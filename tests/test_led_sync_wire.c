/* SPDX-License-Identifier: MIT */
#include "led_sync_wire.h"
#include <assert.h>
#include <stdio.h>

static struct led_sync_packet valid(unsigned kind) {
    struct led_sync_packet p = {0};
    p.source = 255;
    p.version = LED_SYNC_VERSION;
    p.kind = (uint8_t)kind;
    p.central_boot = 0x12345678;
    p.central_epoch = UINT32_MAX;
    p.sequence = 0xabcdef01;
    p.peer_boot = 0x55667788;
    p.peer_epoch = 0x99887766;
    if (kind == LED_SYNC_REQUEST)
        return p;
    if (kind == LED_SYNC_STATE || kind == LED_SYNC_STATE_ACK) {
        p.data.applied.state = (struct led_sync_state_wire){
            0x78563412, UINT64_C(0x123456789abcdef), UINT64_C(0x123456789abcdee), 17, 100, 400, 15};
        if (kind == LED_SYNC_STATE_ACK)
            p.status = LED_SYNC_ACCEPTED;
    } else {
        p.data.clock.t1 = UINT64_C(0x123456789abcdef);
        if (kind >= LED_SYNC_REPLY)
            p.data.clock.t2 = p.data.clock.t3 = INT64_MAX;
        if (kind >= LED_SYNC_FIT)
            p.data.clock.t4 = INT64_MAX;
    }
    return p;
}
static void rejected(const uint8_t *wire, size_t size) {
    struct led_sync_packet out, zero = {0};
    memset(&out, 0xa5, sizeof(out));
    assert(!led_sync_wire_decode(&out, wire, size));
    assert(memcmp(&out, &zero, sizeof(out)) == 0);
}
static void test_full_literal_vectors(void) {
    static const uint8_t fit_bytes[LED_SYNC_WIRE_SIZE] = {
        0xff, 0x04, 0x03, 0x00, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff, 0x01, 0xef,
        0xcd, 0xab, 0x88, 0x77, 0x66, 0x55, 0x66, 0x77, 0x88, 0x99, 0x08, 0x07, 0x06, 0x05,
        0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x28, 0x27,
        0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
    };
    static const uint8_t applied_bytes[LED_SYNC_WIRE_SIZE] = {
        0xff, 0x04, 0x06, 0x02, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff, 0x01, 0xef,
        0xcd, 0xab, 0x88, 0x77, 0x66, 0x55, 0x66, 0x77, 0x88, 0x99, 0x12, 0x34, 0x56, 0x78,
        0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23,
        0x22, 0x21, 0xf1, 0xe4, 0x90, 0x01, 0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51,
    };
    struct led_sync_packet fit = valid(LED_SYNC_FIT), applied = valid(LED_SYNC_STATE_ACK), out;
    fit.data.clock.t1 = UINT64_C(0x0102030405060708);
    fit.data.clock.t2 = UINT64_C(0x1112131415161718);
    fit.data.clock.t3 = UINT64_C(0x2122232425262728);
    fit.data.clock.t4 = UINT64_C(0x3132333435363738);
    applied.status = LED_SYNC_APPLIED;
    applied.data.applied.state.apply_at = UINT64_C(0x4142434445464748);
    applied.data.applied.state.phase_epoch = UINT64_C(0x2122232425262728);
    applied.data.applied.actual_local = UINT64_C(0x5152535455565758);
    uint8_t encoded[LED_SYNC_WIRE_SIZE];
    assert(led_sync_wire_encode(encoded, sizeof(encoded), &fit));
    assert(memcmp(encoded, fit_bytes, sizeof(encoded)) == 0);
    assert(led_sync_wire_decode(&out, fit_bytes, sizeof(fit_bytes)));
    assert(memcmp(&out, &fit, sizeof(out)) == 0);
    assert(led_sync_wire_encode(encoded, sizeof(encoded), &applied));
    assert(memcmp(encoded, applied_bytes, sizeof(encoded)) == 0);
    assert(led_sync_wire_decode(&out, applied_bytes, sizeof(applied_bytes)));
    assert(memcmp(&out, &applied, sizeof(out)) == 0);
    puts("wire independent full FIT/APPLIED vectors: PASS");
}
static void test_vectors_and_flags(void) {
    for (unsigned kind = 1; kind <= LED_SYNC_REQUEST; kind++) {
        struct led_sync_packet p = valid(kind), out;
        uint8_t bytes[LED_SYNC_WIRE_SIZE], again[LED_SYNC_WIRE_SIZE];
        assert(led_sync_wire_encode(bytes, sizeof(bytes), &p));
        assert(bytes[0] == 255 && bytes[1] == 4 && bytes[2] == kind);
        assert(bytes[4] == 0x78 && bytes[7] == 0x12 && bytes[12] == 1 && bytes[15] == 0xab);
        assert(led_sync_wire_decode(&out, bytes, sizeof(bytes)));
        assert(memcmp(&out, &p, sizeof(p)) == 0);
        assert(led_sync_wire_encode(again, sizeof(again), &out));
        assert(memcmp(bytes, again, sizeof(bytes)) == 0);
        if (kind != LED_SYNC_STATE && kind != LED_SYNC_STATE_ACK)
            continue;
        assert(bytes[24] == 0x12 && bytes[27] == 0x78 && bytes[28] == 0xef && bytes[35] == 1);
        assert(bytes[44] == 0xf1 && bytes[45] == 0xe4 && bytes[46] == 0x90 && bytes[47] == 1);
        for (unsigned pattern = 0; pattern < 18; pattern++)
            for (unsigned brightness = 0; brightness <= 100; brightness++)
                for (unsigned flags = 0; flags < 16; flags++) {
                    p.data.applied.state.pattern = (uint8_t)pattern;
                    p.data.applied.state.brightness = (uint8_t)brightness;
                    p.data.applied.state.flags = (uint8_t)flags;
                    p.data.applied.state.speed = flags & 1U ? 10 : 400;
                    assert(led_sync_wire_encode(bytes, sizeof(bytes), &p));
                    assert(led_sync_wire_decode(&out, bytes, sizeof(bytes)));
                    assert(memcmp(&out, &p, sizeof(p)) == 0);
                }
        printf("wire kind %u flags/pattern/brightness: PASS\n", kind);
    }
}
static void test_rejection_and_domains(void) {
    uint8_t bytes[LED_SYNC_WIRE_SIZE + 1] = {0}, original[LED_SYNC_WIRE_SIZE];
    struct led_sync_packet out, p = valid(LED_SYNC_STATE);
    assert(led_sync_wire_encode(original, sizeof(original), &p));
    for (size_t size = 0; size < LED_SYNC_WIRE_SIZE; size++)
        rejected(original, size);
    rejected(bytes, sizeof(bytes));
    rejected(NULL, LED_SYNC_WIRE_SIZE);
    assert(!led_sync_wire_decode(NULL, original, sizeof(original)));
    assert(!led_sync_wire_encode(NULL, 0, &p));
    memset(bytes, 0xa5, sizeof(bytes));
    assert(!led_sync_wire_encode(bytes, sizeof(bytes), &p));
    for (unsigned i = 0; i < sizeof(bytes); i++)
        assert(bytes[i] == 0);
    assert(!led_sync_wire_encode(bytes, LED_SYNC_WIRE_SIZE, NULL));
    for (unsigned i = 0; i < LED_SYNC_WIRE_SIZE; i++)
        assert(bytes[i] == 0);
    for (unsigned kind = 1; kind <= LED_SYNC_REQUEST; kind++) {
        p = valid(kind);
        assert(led_sync_wire_encode(original, sizeof(original), &p));
        for (unsigned i = 0; i < LED_SYNC_WIRE_SIZE; i++) {
            bool unused =
                (kind == LED_SYNC_PROBE && i >= 32) || (kind == LED_SYNC_REPLY && i >= 48) ||
                (kind == LED_SYNC_STATE && i >= 48) || (kind == LED_SYNC_REQUEST && i >= 24);
            if (unused) {
                memcpy(bytes, original, sizeof(original));
                bytes[i] = 1;
                rejected(bytes, LED_SYNC_WIRE_SIZE);
            }
        }
        memcpy(bytes, original, sizeof(original));
        bytes[1] = 2;
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        bytes[1] = 3; /* Immediate predecessor has different coasting semantics. */
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        bytes[1] = 5;
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        memcpy(bytes, original, sizeof(original));
        bytes[2] = 0;
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        bytes[2] = 8;
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        memcpy(bytes, original, sizeof(original));
        bytes[3] = 255;
        rejected(bytes, LED_SYNC_WIRE_SIZE);
        if (kind == LED_SYNC_STATE || kind == LED_SYNC_STATE_ACK) {
            memcpy(bytes, original, sizeof(original));
            bytes[44] = 18;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            memcpy(bytes, original, sizeof(original));
            bytes[45] = 101;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            memcpy(bytes, original, sizeof(original));
            bytes[46] = 9;
            bytes[47] = 0;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            memcpy(bytes, original, sizeof(original));
            bytes[46] = 0x91;
            bytes[47] = 1;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            memcpy(bytes, original, sizeof(original));
            bytes[35] = 0x80;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            memcpy(bytes, original, sizeof(original));
            bytes[43] = 0x80;
            rejected(bytes, LED_SYNC_WIRE_SIZE);
            if (kind == LED_SYNC_STATE_ACK) {
                memcpy(bytes, original, sizeof(original));
                bytes[48] = 1;
                rejected(bytes, LED_SYNC_WIRE_SIZE);
                for (unsigned status = LED_SYNC_APPLIED; status <= LED_SYNC_REJECT_RENDER;
                     status++) {
                    p.status = (uint8_t)status;
                    p.data.applied.actual_local = INT64_MAX;
                    assert(led_sync_wire_encode(bytes, LED_SYNC_WIRE_SIZE, &p));
                    assert(led_sync_wire_decode(&out, bytes, LED_SYNC_WIRE_SIZE));
                    assert(out.data.applied.actual_local == INT64_MAX);
                    bytes[55] = 0x80;
                    rejected(bytes, LED_SYNC_WIRE_SIZE);
                }
            }
        } else {
            unsigned clocks = kind == LED_SYNC_REQUEST ? 0U
                              : kind == LED_SYNC_PROBE ? 1U
                              : kind == LED_SYNC_REPLY ? 3U
                                                       : 4U;
            for (unsigned i = 0; i < clocks; i++) {
                memcpy(bytes, original, sizeof(original));
                bytes[31 + 8 * i] |= 0x80;
                rejected(bytes, LED_SYNC_WIRE_SIZE);
            }
        }
        p = valid(kind);
        p.data.bytes[39] = 1;
        assert(!led_sync_wire_encode(bytes, LED_SYNC_WIRE_SIZE, &p));
    }
    puts("wire canonical shape/domains/failed-output: PASS");
}
int main(void) {
    _Static_assert(LED_SYNC_WIRE_SIZE + 3 == 65 - 3 - 3, "MTU65 one relay chunk");
    test_full_literal_vectors();
    test_vectors_and_flags();
    test_rejection_and_domains();
    puts("led sync wire: PASS");
    return 0;
}
