/*
 * pps_audit.h — short-transfer classification for PPS-aligned DMA commits.
 *
 * The rx888-firmware GPIF state machine forces a DMA commit on the rising
 * edge of a PPS signal. The host sees that commit as a USB bulk transfer
 * shorter than the configured buffer size. Counting bytes between
 * consecutive short transfers tells us how many PCLK edges (== ADC
 * samples) occurred in one PPS interval; a deficit vs the configured
 * sample rate localises an ADC->GPIF silent drop.
 *
 * This header is pure logic — no atomics, no I/O, no librx888 internals.
 * librx888 owns a pps_audit_t per device, mutated only by the writer
 * thread; values are mirrored to atomic fields for rx888_get_stats().
 * The unit test exercises the same function with synthetic inputs.
 *
 * Copyright 2026 Free Software Foundation, Inc.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PPS_AUDIT_H
#define PPS_AUDIT_H

#include <stdint.h>

typedef struct {
    uint64_t full_xfers;          /* actual_length == expected_bytes */
    uint64_t short_xfers;         /* actual_length <  expected_bytes (and > 0), or forced */
    uint64_t zero_xfers;          /* actual_length == 0 (should not happen on healthy USB) */
    uint64_t bytes_in_window;     /* bytes since last short (or since start) */
    uint64_t last_window_bytes;   /* bytes_in_window at the moment of the most recent short */
    uint32_t min_actual_len;      /* smallest non-zero actual_length observed (0 = none yet) */
    uint32_t max_actual_len;      /* largest actual_length observed */
} pps_audit_t;

static inline void pps_audit_init(pps_audit_t *c) {
    c->full_xfers        = 0;
    c->short_xfers       = 0;
    c->zero_xfers        = 0;
    c->bytes_in_window   = 0;
    c->last_window_bytes = 0;
    c->min_actual_len    = 0;
    c->max_actual_len    = 0;
}

/*
 * Classify one completed transfer.
 *
 *   expected_bytes  configured full-transfer length (req_packets * max_packet)
 *   actual_length   bytes actually delivered by libusb
 *   force_short     non-zero: classify as short even if actual_length >= expected
 *                   (used by the synthetic debug mode to exercise the detector
 *                   end-to-end against stock firmware that never short-commits)
 *
 * bytes_in_window always accumulates actual_length, regardless of
 * classification, so window byte counts reflect real data even when the
 * "short" signal is synthetic.
 */
static inline void pps_audit_step(pps_audit_t *c,
                                  uint32_t expected_bytes,
                                  uint32_t actual_length,
                                  int force_short) {
    if (actual_length == 0) {
        c->zero_xfers++;
        return;
    }

    if (c->min_actual_len == 0 || actual_length < c->min_actual_len)
        c->min_actual_len = actual_length;
    if (actual_length > c->max_actual_len)
        c->max_actual_len = actual_length;

    c->bytes_in_window += actual_length;

    int is_short = force_short || (actual_length < expected_bytes);
    if (is_short) {
        c->short_xfers++;
        c->last_window_bytes = c->bytes_in_window;
        c->bytes_in_window   = 0;
    } else {
        c->full_xfers++;
    }
}

#endif /* PPS_AUDIT_H */
