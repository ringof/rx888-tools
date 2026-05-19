/*
 * pps_audit_test.c — exercise pps_audit_step() with synthetic transfer
 * sequences so the short-transfer / PPS-window counter logic is verified
 * before any firmware actually produces short commits.
 *
 * No hardware, no USB. Runs under `make check`.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "pps_audit.h"

static int failures = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { fprintf(stderr, "ok   %s\n", msg); } \
    else      { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
} while (0)

#define EXPECT_EQ(actual, expected, msg) do {                                 \
    unsigned long long _a = (unsigned long long)(actual);                     \
    unsigned long long _e = (unsigned long long)(expected);                   \
    if (_a == _e) { fprintf(stderr, "ok   %s (got %llu)\n", msg, _a); }       \
    else { fprintf(stderr, "FAIL %s (got %llu, expected %llu)\n",             \
                   msg, _a, _e); failures++; }                                \
} while (0)

/* Init zeroes everything. */
static void test_init(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    EXPECT_EQ(c.full_xfers,        0, "init: full_xfers");
    EXPECT_EQ(c.short_xfers,       0, "init: short_xfers");
    EXPECT_EQ(c.zero_xfers,        0, "init: zero_xfers");
    EXPECT_EQ(c.bytes_in_window,   0, "init: bytes_in_window");
    EXPECT_EQ(c.last_window_bytes, 0, "init: last_window_bytes");
    EXPECT_EQ(c.min_actual_len,    0, "init: min_actual_len");
    EXPECT_EQ(c.max_actual_len,    0, "init: max_actual_len");
}

/* All full transfers, no shorts: bytes_in_window keeps accumulating;
 * last_window_bytes stays at 0 because no window has closed. */
static void test_all_full(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp = 1024u * 1024u;
    for (int i = 0; i < 10; i++) pps_audit_step(&c, exp, exp, 0);
    EXPECT_EQ(c.full_xfers,        10,            "all-full: full count");
    EXPECT_EQ(c.short_xfers,       0,             "all-full: short count");
    EXPECT_EQ(c.bytes_in_window,   10ULL * exp,   "all-full: window accum");
    EXPECT_EQ(c.last_window_bytes, 0,             "all-full: no window closed");
    EXPECT_EQ(c.min_actual_len,    exp,           "all-full: min == exp");
    EXPECT_EQ(c.max_actual_len,    exp,           "all-full: max == exp");
}

/* A natural short transfer closes the window; the short's own bytes
 * belong to that closing window, not the next. */
static void test_natural_short(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp   = 1024u * 1024u;
    const uint32_t shorty = 333u;
    for (int i = 0; i < 5; i++) pps_audit_step(&c, exp, exp, 0);
    pps_audit_step(&c, exp, shorty, 0);
    EXPECT_EQ(c.full_xfers,        5,                          "short-close: full count");
    EXPECT_EQ(c.short_xfers,       1,                          "short-close: short count");
    EXPECT_EQ(c.last_window_bytes, 5ULL * exp + shorty,        "short-close: window bytes");
    EXPECT_EQ(c.bytes_in_window,   0,                          "short-close: window reset");
    EXPECT_EQ(c.min_actual_len,    shorty,                     "short-close: min");
    EXPECT_EQ(c.max_actual_len,    exp,                        "short-close: max");
}

/* Two PPS windows back to back: each should report fs-worth of bytes
 * in last_window_bytes at the moment of close. */
static void test_two_windows(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp = 1024u * 1024u;
    /* window 1: 9 full + 1 short(100) -> 9*exp + 100 */
    for (int i = 0; i < 9; i++) pps_audit_step(&c, exp, exp, 0);
    pps_audit_step(&c, exp, 100u, 0);
    EXPECT_EQ(c.last_window_bytes, 9ULL * exp + 100u, "win1: byte total");

    /* window 2: 7 full + 1 short(200) -> 7*exp + 200 */
    for (int i = 0; i < 7; i++) pps_audit_step(&c, exp, exp, 0);
    pps_audit_step(&c, exp, 200u, 0);
    EXPECT_EQ(c.last_window_bytes, 7ULL * exp + 200u, "win2: byte total");
    EXPECT_EQ(c.short_xfers,       2,                 "win2: short count");
    EXPECT_EQ(c.bytes_in_window,   0,                 "win2: window reset");
}

/* force_short=1 closes the window even when actual_length >= expected,
 * and bytes_in_window includes the (full-sized) actual transfer. This
 * is what the librx888 synthetic-PPS debug mode relies on. */
static void test_force_short(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp = 1024u * 1024u;
    for (int i = 0; i < 4; i++) pps_audit_step(&c, exp, exp, 0);
    pps_audit_step(&c, exp, exp, 1);  /* full length, force short */
    EXPECT_EQ(c.full_xfers,        4,            "force-short: full count");
    EXPECT_EQ(c.short_xfers,       1,            "force-short: short count");
    EXPECT_EQ(c.last_window_bytes, 5ULL * exp,   "force-short: window includes forced xfer");
    EXPECT_EQ(c.bytes_in_window,   0,            "force-short: window reset");
}

/* Zero-length transfers should not enter the window. */
static void test_zero_length(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp = 1024u * 1024u;
    pps_audit_step(&c, exp, exp, 0);
    pps_audit_step(&c, exp, 0u,  0);
    pps_audit_step(&c, exp, exp, 0);
    EXPECT_EQ(c.full_xfers,        2,           "zero: full count unaffected");
    EXPECT_EQ(c.zero_xfers,        1,           "zero: zero count");
    EXPECT_EQ(c.bytes_in_window,   2ULL * exp,  "zero: window excludes zero xfer");
}

/* Synthetic mode: every Nth transfer marked short, others full. After
 * K*N transfers we should see exactly K shorts and K*(N-1) fulls; the
 * last_window_bytes should be N*exp (a synthetic short on a full xfer
 * contributes its full length to the window). */
static void test_synthetic_pattern(void) {
    pps_audit_t c;
    pps_audit_init(&c);
    const uint32_t exp = 1024u * 1024u;
    const int N = 5;
    const int K = 4;
    for (int i = 1; i <= K * N; i++) {
        int force = (i % N) == 0;
        pps_audit_step(&c, exp, exp, force);
    }
    EXPECT_EQ(c.short_xfers,       K,                "synth: K shorts");
    EXPECT_EQ(c.full_xfers,        K * (N - 1),      "synth: K*(N-1) fulls");
    EXPECT_EQ(c.last_window_bytes, (uint64_t)N * exp, "synth: window byte count");
    EXPECT_EQ(c.bytes_in_window,   0,                "synth: window reset");
}

int main(void) {
    test_init();
    test_all_full();
    test_natural_short();
    test_two_windows();
    test_force_short();
    test_zero_length();
    test_synthetic_pattern();

    fprintf(stderr, "\n%s (%d failures)\n",
            failures == 0 ? "ALL OK" : "FAILED",
            failures);
    return failures == 0 ? 0 : 1;
}
