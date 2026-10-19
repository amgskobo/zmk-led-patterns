/* Copyright (c) 2026 amgskobo
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LED_SYNC_CLOCK_MAX_RTT_MS 80U
#define LED_SYNC_CLOCK_MAX_ERROR_MS 50U
#define LED_SYNC_CLOCK_TTL_MS 8000U
#define LED_SYNC_CLOCK_DRIFT_PPM 1000U
#define LED_SYNC_CLOCK_MAX_EXCHANGE_MS 500U

/* Offset is peripheral time minus central time. All wire times are integer
 * milliseconds in 0..INT64_MAX; this helper never changes kernel uptime.
 * Session identities and reservation generations belong to the caller. */
struct led_sync_clock_fit {
    bool valid;
    int64_t offset_ms;
    int64_t lower_ms;
    int64_t upper_ms;
    uint64_t reference_ms;
    uint32_t network_rtt_ms;
    uint32_t base_error_ms;
};

static inline uint32_t led_sync_clock_drift_ms(uint32_t elapsed_ms) {
    return (uint32_t)(((uint64_t)elapsed_ms * LED_SYNC_CLOCK_DRIFT_PPM + 999999U) / 1000000U);
}

/* Validate public fit metadata before it participates in signed arithmetic.
 * The subtraction is unsigned so even malformed opposite signed extremes are
 * rejected without overflowing. A sample's interval is at most MAX_RTT wide. */
static inline bool led_sync_clock_fit_valid(const struct led_sync_clock_fit *fit) {
    return fit != NULL && fit->valid && fit->reference_ms <= INT64_MAX &&
           fit->network_rtt_ms <= LED_SYNC_CLOCK_MAX_RTT_MS &&
           fit->base_error_ms <= LED_SYNC_CLOCK_MAX_ERROR_MS && fit->lower_ms != INT64_MIN &&
           fit->lower_ms <= fit->upper_ms &&
           (uint64_t)fit->upper_ms - (uint64_t)fit->lower_ms == fit->network_rtt_ms &&
           fit->offset_ms == fit->lower_ms + (int64_t)(fit->network_rtt_ms / 2U) &&
           fit->base_error_ms >= (fit->network_rtt_ms + 1U) / 2U;
}

/* t1/t4 are central send/receive, t2/t3 peripheral receive/send. The interval
 * [t3-t4, t2-t1] includes path asymmetry and core queue residence. Quantization
 * must cover the actual timestamp unit (integer-ms captures need an ms budget,
 * even when the kernel tick is shorter). Exchange drift is budgeted separately.
 * Failure clears the complete candidate; use a temporary when retaining a fit. */
static inline bool led_sync_clock_sample(struct led_sync_clock_fit *out, uint64_t t1, uint64_t t2,
                                         uint64_t t3, uint64_t t4, uint32_t quantization_ms) {
    if (out == NULL) {
        return false;
    }
    *out = (struct led_sync_clock_fit){0};
    if (t1 > INT64_MAX || t2 > INT64_MAX || t3 > INT64_MAX || t4 > INT64_MAX || t4 < t1 ||
        t3 < t2) {
        return false;
    }
    const uint64_t exchange_ms = t4 - t1;
    const uint64_t residence_ms = t3 - t2;
    if (exchange_ms > LED_SYNC_CLOCK_MAX_EXCHANGE_MS || residence_ms > exchange_ms) {
        return false;
    }
    const uint32_t rtt_ms = (uint32_t)(exchange_ms - residence_ms);
    if (rtt_ms > LED_SYNC_CLOCK_MAX_RTT_MS) {
        return false;
    }
    const uint64_t error_ms = (rtt_ms + 1U) / 2U + (uint64_t)quantization_ms +
                              led_sync_clock_drift_ms((uint32_t)exchange_ms);
    if (error_ms > LED_SYNC_CLOCK_MAX_ERROR_MS) {
        return false;
    }
    const int64_t lower_ms = (int64_t)t3 - (int64_t)t4;
    *out = (struct led_sync_clock_fit){
        .valid = true,
        .offset_ms = lower_ms + (int64_t)(rtt_ms / 2U),
        .lower_ms = lower_ms,
        .upper_ms = (int64_t)t2 - (int64_t)t1,
        .reference_ms = t4,
        .network_rtt_ms = rtt_ms,
        .base_error_ms = (uint32_t)error_ms,
    };
    return true;
}

/* Returns UINT32_MAX when the fit/time is invalid or expired. Quality must be
 * evaluated at the future apply time, including any monotonic-clamp error.
 * Conversion alone below does not assert this freshness/quality contract. */
static inline uint32_t led_sync_clock_error_at(const struct led_sync_clock_fit *fit,
                                               uint64_t central_ms, uint32_t extra_error_ms) {
    if (!led_sync_clock_fit_valid(fit) || central_ms > INT64_MAX ||
        central_ms < fit->reference_ms) {
        return UINT32_MAX;
    }
    const uint64_t age_ms = central_ms - fit->reference_ms;
    if (age_ms >= LED_SYNC_CLOCK_TTL_MS) {
        return UINT32_MAX;
    }
    const uint64_t error_ms =
        (uint64_t)fit->base_error_ms + extra_error_ms + led_sync_clock_drift_ms((uint32_t)age_ms);
    return error_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)error_ms;
}

static inline bool led_sync_clock_valid_at(const struct led_sync_clock_fit *fit,
                                           uint64_t central_ms, uint32_t extra_error_ms) {
    return led_sync_clock_error_at(fit, central_ms, extra_error_ms) <= LED_SYNC_CLOCK_MAX_ERROR_MS;
}

static inline bool led_sync_clock_translate(const struct led_sync_clock_fit *fit, uint64_t value_ms,
                                            bool reverse, uint64_t *out) {
    if (out == NULL || !led_sync_clock_fit_valid(fit) || value_ms > INT64_MAX) {
        return false;
    }
    const bool negative = fit->offset_ms < 0;
    const uint64_t magnitude =
        negative ? (uint64_t)(-(fit->offset_ms + 1)) + 1U : (uint64_t)fit->offset_ms;
    if (negative != reverse) {
        if (value_ms < magnitude) {
            return false;
        }
        *out = value_ms - magnitude;
    } else {
        if (value_ms > (uint64_t)INT64_MAX - magnitude) {
            return false;
        }
        *out = value_ms + magnitude;
    }
    return true;
}

static inline bool led_sync_clock_to_local(const struct led_sync_clock_fit *fit,
                                           uint64_t central_ms, uint64_t *out) {
    return led_sync_clock_translate(fit, central_ms, false, out);
}

static inline bool led_sync_clock_to_central(const struct led_sync_clock_fit *fit,
                                             uint64_t local_ms, uint64_t *out) {
    return led_sync_clock_translate(fit, local_ms, true, out);
}
