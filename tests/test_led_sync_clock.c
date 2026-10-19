/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "led_sync_clock.h"

/* Identified negative controls change only the fixture's observed helper
 * result/call time. They retain every production input and assertion. The
 * ordinary fixture has neither flag; each flagged binary must fail. */
static bool sample(struct led_sync_clock_fit *out, uint64_t t1, uint64_t t2, uint64_t t3,
                   uint64_t t4, uint32_t quantization_ms) {
    const bool result = led_sync_clock_sample(out, t1, t2, t3, t4, quantization_ms);
#if defined(LED_SYNC_CLOCK_TEST_ZERO_OFFSET)
    if (result) {
        out->offset_ms = 0; /* Missing clock-offset compensation. */
    }
#endif
    return result;
}

static uint64_t quality_time(const struct led_sync_clock_fit *fit, uint64_t central_ms) {
#if defined(LED_SYNC_CLOCK_TEST_REFERENCE_AGE)
    if (fit != NULL && fit->valid && central_ms >= fit->reference_ms &&
        central_ms - fit->reference_ms < LED_SYNC_CLOCK_TTL_MS) {
        return fit->reference_ms; /* Wrongly checks sample time, not future apply. */
    }
#else
    (void)fit;
#endif
    return central_ms;
}

static uint32_t error_at(const struct led_sync_clock_fit *fit, uint64_t central_ms,
                         uint32_t extra_error_ms) {
    return led_sync_clock_error_at(fit, quality_time(fit, central_ms), extra_error_ms);
}

static bool valid_at(const struct led_sync_clock_fit *fit, uint64_t central_ms,
                     uint32_t extra_error_ms) {
    return led_sync_clock_valid_at(fit, quality_time(fit, central_ms), extra_error_ms);
}

static void assert_cleared(const struct led_sync_clock_fit *fit) {
    assert(!fit->valid && fit->offset_ms == 0 && fit->lower_ms == 0 && fit->upper_ms == 0);
    assert(fit->reference_ms == 0 && fit->network_rtt_ms == 0 && fit->base_error_ms == 0);
}

static void test_asymmetry_and_residence(void) {
    struct led_sync_clock_fit fit;
    /* The true offset is4000ms, outbound8ms and return10ms. Both queues and
     * radio residence belong to those delays; peer processing2ms is removed. */
    assert(sample(&fit, 1000, 5008, 5010, 1020, 2));
    assert(fit.valid && fit.lower_ms == 3990 && fit.upper_ms == 4008);
    assert(fit.offset_ms == 3999 && fit.network_rtt_ms == 18);
    assert(fit.reference_ms == 1020 && fit.base_error_ms == 12);
    assert(error_at(&fit, 1020, 0) == 12 && valid_at(&fit, 1020, 0));
    uint64_t local;
    uint64_t central;
    assert(led_sync_clock_to_local(&fit, 1020, &local) && local == 5019);
    assert(led_sync_clock_to_central(&fit, local, &central) && central == 1020);

    /* Opposite boot order and unequal paths: never assume which uptime leads. */
    assert(sample(&fit, 10000, 108, 110, 10020, 2));
    assert(fit.lower_ms == -9910 && fit.upper_ms == -9892 && fit.offset_ms == -9901);
    assert(led_sync_clock_to_local(&fit, 10020, &local) && local == 119);
    assert(led_sync_clock_to_central(&fit, local, &central) && central == 10020);

    /*450ms of reply processing does not count as networkRTT; exchange500ms is
     * admitted exactly, while preserving a drift allowance for the exchange. */
    assert(sample(&fit, 100, 400, 850, 600, 2));
    assert(fit.network_rtt_ms == 50 && fit.offset_ms == 275 && fit.base_error_ms == 28);
    assert(sample(&fit, 100, 1000, 1001, 104, 2));
    assert(fit.lower_ms == 897 && fit.upper_ms == 900 && fit.offset_ms == 898);
    assert(fit.network_rtt_ms == 3 && fit.base_error_ms == 5); /* odd-width ceil */
}

static void test_quality_at_future_apply(void) {
    struct led_sync_clock_fit fit;
    assert(sample(&fit, 100, 100, 100, 180, 2));
    assert(fit.network_rtt_ms == 80 && fit.base_error_ms == 43);
    assert(error_at(&fit, 180, 0) == 43);
    assert(error_at(&fit, 181, 0) == 44); /* ceil, not truncated drift */
    assert(error_at(&fit, 1180, 0) == 44);
    assert(error_at(&fit, 1181, 0) == 45);
    assert(error_at(&fit, 7180, 0) == 50 && valid_at(&fit, 7180, 0));
    assert(error_at(&fit, 7181, 0) == 51 && !valid_at(&fit, 7181, 0));
    assert(error_at(&fit, 8179, 0) == 51 && !valid_at(&fit, 8179, 0));
    assert(error_at(&fit, 8180, 0) == UINT32_MAX && !valid_at(&fit, 8180, 0));
    assert(error_at(&fit, 8181, 0) == UINT32_MAX);
    assert(error_at(&fit, 179, 0) == UINT32_MAX && !valid_at(&fit, 179, 0));

    /* A fit usable now may fail at the reserved deadline without expiring. */
    const uint64_t now = fit.reference_ms + 6000;
    const uint64_t apply_at = now + 1000 + 2 * fit.network_rtt_ms;
    assert(valid_at(&fit, now, 0));
    assert(apply_at - fit.reference_ms < LED_SYNC_CLOCK_TTL_MS);
    assert(error_at(&fit, apply_at, 0) == 51 && !valid_at(&fit, apply_at, 0));
    assert(valid_at(&fit, fit.reference_ms, 7));
    assert(!valid_at(&fit, fit.reference_ms, 8));
    assert(error_at(&fit, fit.reference_ms, UINT32_MAX) == UINT32_MAX);
    assert(error_at(&fit, (uint64_t)INT64_MAX + 1U, 0) == UINT32_MAX);
    assert(!valid_at(&fit, UINT64_MAX, 0));
}

static void test_quantization_and_zero_exchange(void) {
    struct led_sync_clock_fit fit;
    assert(sample(&fit, 5, 7, 7, 5, 0));
    assert(fit.lower_ms == 2 && fit.upper_ms == 2 && fit.offset_ms == 2);
    assert(fit.base_error_ms == 0 && error_at(&fit, 5, 0) == 0);
    assert(error_at(&fit, 6, 0) == 1);
    assert(sample(&fit, 0, 0, 1, 1, 0));
    assert(fit.network_rtt_ms == 0 && fit.base_error_ms == 1);
    assert(sample(&fit, 5, 7, 7, 5, 50));
    assert(fit.base_error_ms == 50 && valid_at(&fit, 5, 0) && !valid_at(&fit, 6, 0));
    assert(sample(&fit, 0, 0, 1, 1, 49));
    assert(fit.base_error_ms == 50);

    /* The caller-supplied ms allowance is retained even at sub-ms tick rates.
     * Slow clocks may pass a larger allowance; it is never silently clipped. */
    assert(sample(&fit, 100, 100, 100, 120, 32));
    assert(fit.base_error_ms == 43 && error_at(&fit, 120, 0) == 43);
}

static void test_uptime_and_checked_conversions(void) {
    struct led_sync_clock_fit fit;
    uint64_t value;
    const uint64_t uptime = (UINT64_C(1) << 32) + 12345;
    assert(sample(&fit, uptime, uptime + 999, uptime + 1001, uptime + 20, 2));
    assert(fit.offset_ms == 990 && fit.reference_ms == uptime + 20);
    assert(led_sync_clock_to_local(&fit, uptime + 20, &value) && value == uptime + 1010);
    assert(led_sync_clock_to_central(&fit, value, &value) && value == uptime + 20);

    const uint64_t maximum = INT64_MAX;
    assert(sample(&fit, maximum - 100, maximum - 90, maximum - 80, maximum - 70, 2));
    assert(fit.offset_ms == 0 && fit.lower_ms == -10 && fit.upper_ms == 10);
    assert(led_sync_clock_to_local(&fit, maximum, &value) && value == maximum);
    assert(led_sync_clock_to_central(&fit, maximum, &value) && value == maximum);
    assert(valid_at(&fit, maximum, 0));

    /* Both extreme offsets are valid when each independent uptime is valid.
     * Failed conversions retain the caller's output instead of saturating it. */
    assert(sample(&fit, 0, maximum, maximum, 0, 0));
    assert(fit.offset_ms == INT64_MAX);
    assert(led_sync_clock_to_local(&fit, 0, &value) && value == maximum);
    assert(led_sync_clock_to_central(&fit, maximum, &value) && value == 0);
    value = 123;
    assert(!led_sync_clock_to_local(&fit, 1, &value) && value == 123);
    assert(!led_sync_clock_to_central(&fit, maximum - 1, &value) && value == 123);
    assert(sample(&fit, maximum, 0, 0, maximum, 0));
    assert(fit.offset_ms == -INT64_MAX);
    assert(led_sync_clock_to_local(&fit, maximum, &value) && value == 0);
    assert(led_sync_clock_to_central(&fit, 0, &value) && value == maximum);
    value = 456;
    assert(!led_sync_clock_to_local(&fit, maximum - 1, &value) && value == 456);
    assert(!led_sync_clock_to_central(&fit, 1, &value) && value == 456);
    assert(!led_sync_clock_to_local(&fit, maximum + 1U, &value) && value == 456);
    assert(!led_sync_clock_to_central(&fit, UINT64_MAX, &value) && value == 456);
    assert(!led_sync_clock_to_local(&fit, maximum, NULL));
    assert(!led_sync_clock_to_central(&fit, 0, NULL));

    /* Range conversion is separate from freshness admission. */
    assert(sample(&fit, 100, 100, 100, 100, 0));
    assert(!valid_at(&fit, 8100, 0));
    assert(led_sync_clock_to_local(&fit, 8100, &value) && value == 8100);
}

static void test_invalid_samples_and_independence(void) {
    struct bad_sample { uint64_t t1, t2, t3, t4; uint32_t quantization; };
    static const struct bad_sample cases[] = {
        {(uint64_t)INT64_MAX + 1U, 0, 0, 0, 2},
        {0, (uint64_t)INT64_MAX + 1U, 0, 0, 2},
        {0, 0, UINT64_MAX, 0, 2},
        {0, 0, 0, UINT64_MAX, 2},
        {11, 0, 0, 10, 2},
        {0, 11, 10, 20, 2},
        {0, 0, 500, 501, 2},
        {0, 0, 21, 20, 2},
        {0, 0, 0, 81, 2},
        {5, 7, 7, 5, 51},
        {0, 0, 1, 1, 50},
        {0, 0, 0, 0, UINT32_MAX},
    };
    struct led_sync_clock_fit retained;
    assert(sample(&retained, 1000, 5008, 5010, 1020, 2));
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct led_sync_clock_fit candidate = retained;
        assert(!sample(&candidate, cases[i].t1, cases[i].t2, cases[i].t3,
                       cases[i].t4, cases[i].quantization));
        assert_cleared(&candidate);
        assert(retained.valid && retained.offset_ms == 3999 && retained.reference_ms == 1020);
    }
    assert(!sample(NULL, 0, 0, 0, 0, 0));
}

static void assert_invalid_fit(const struct led_sync_clock_fit *fit) {
    uint64_t value = 789;
    assert(error_at(fit, 1020, 0) == UINT32_MAX);
    assert(!valid_at(fit, 1020, 0));
    assert(!led_sync_clock_to_local(fit, 1020, &value) && value == 789);
    assert(!led_sync_clock_to_central(fit, 5019, &value) && value == 789);
}

static void test_invalid_fit_metadata(void) {
    struct led_sync_clock_fit original;
    assert(sample(&original, 1000, 5008, 5010, 1020, 2));
    assert_invalid_fit(NULL);
    struct led_sync_clock_fit bad = original;
    bad.valid = false;
    assert_invalid_fit(&bad);
    bad = original; bad.reference_ms = (uint64_t)INT64_MAX + 1U;
    assert_invalid_fit(&bad);
    bad = original; bad.network_rtt_ms = 81;
    assert_invalid_fit(&bad);
    bad = original; bad.base_error_ms = 51;
    assert_invalid_fit(&bad);
    bad = original; bad.lower_ms = INT64_MIN;
    assert_invalid_fit(&bad);
    bad = original; bad.lower_ms = bad.upper_ms + 1;
    assert_invalid_fit(&bad);
    bad = original; bad.upper_ms++;
    assert_invalid_fit(&bad);
    bad = original; bad.offset_ms++;
    assert_invalid_fit(&bad);
    bad = original; bad.offset_ms = INT64_MIN;
    assert_invalid_fit(&bad);
    bad = original; bad.base_error_ms = 8;
    assert_invalid_fit(&bad);
    bad = original; bad.lower_ms = -INT64_MAX; bad.upper_ms = INT64_MAX;
    assert_invalid_fit(&bad);
}

static void test_known_clock_interval_grid(void) {
    /* Generate independent known clocks, rather than an expected midpoint
     * implementation. Every actual offset must lie inside the admitted bounds
     * despite asymmetric paths, processing residence and uptime above49 days. */
    const uint64_t start = UINT64_C(1) << 40;
    for (uint32_t i = 0; i < 10000; ++i) {
        const int64_t offset = (int64_t)(i % 101) * 1000 - 50000;
        const uint64_t forward = i % 40;
        const uint64_t backward = (i / 40) % 40;
        const uint64_t processing = (i / 1600) % 50;
        const uint64_t t1 = start + i * UINT64_C(1000);
        const uint64_t t2 = (uint64_t)((int64_t)t1 + offset) + forward;
        const uint64_t t3 = t2 + processing;
        const uint64_t t4 = t1 + forward + processing + backward;
        struct led_sync_clock_fit fit;
        assert(sample(&fit, t1, t2, t3, t4, 2));
        assert(fit.lower_ms <= offset && offset <= fit.upper_ms);
        const uint64_t offset_error = fit.offset_ms >= offset
                                          ? (uint64_t)(fit.offset_ms - offset)
                                          : (uint64_t)(offset - fit.offset_ms);
        assert(offset_error <= fit.base_error_ms);
        uint64_t local;
        uint64_t central;
        assert(led_sync_clock_to_local(&fit, t4, &local));
        assert(led_sync_clock_to_central(&fit, local, &central) && central == t4);
        assert(valid_at(&fit, t4, 0));
    }
}

int main(void) {
    test_asymmetry_and_residence();
    test_quality_at_future_apply();
    test_quantization_and_zero_exchange();
    test_uptime_and_checked_conversions();
    test_invalid_samples_and_independence();
    test_invalid_fit_metadata();
    test_known_clock_interval_grid();
    puts("led sync clock: PASS");
    return 0;
}
