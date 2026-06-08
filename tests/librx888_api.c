/*
 * librx888_api.c — no-hardware ABI / validation tests for librx888.
 *
 * Designed to run in CI on machines with no RX888 plugged in.  Each
 * subtest is a single assertion; first failure aborts.  Add subtests
 * for any new public surface in librx888.h.
 *
 * Build via the top-level Makefile (`make check`) or by hand:
 *   cc -Iinclude tests/librx888_api.c -L. -lrx888 \
 *      -Wl,-rpath,'$$ORIGIN/..' -o tests/librx888_api
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb-1.0/libusb.h>

#include "librx888.h"

static int failures = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { fprintf(stderr, "ok   %s\n", msg); } \
    else      { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
} while (0)

static void test_null_safety(void) {
    /* Must not crash on NULL inputs. */
    rx888_close(NULL);
    rx888_stop(NULL);
    rx888_config_init_default(NULL);
    rx888_stats_t s;
    rx888_get_stats(NULL, &s);
    rx888_get_stats((rx888_t *)0xdeadbeef, NULL);
    EXPECT(rx888_is_running(NULL) == 0, "rx888_is_running(NULL) returns 0");
}

static void test_version_string(void) {
    const char *v = rx888_version();
    EXPECT(v != NULL && v[0] != '\0', "rx888_version returns non-empty string");
}

static void test_strerror(void) {
    const char *s = rx888_strerror(LIBUSB_ERROR_NO_DEVICE);
    EXPECT(s != NULL && s[0] != '\0',
           "rx888_strerror(NO_DEVICE) returns non-empty string");
}

static void test_open_validates_null(void) {
    int rc = rx888_open(NULL, NULL);
    EXPECT(rc == LIBUSB_ERROR_INVALID_PARAM, "rx888_open(NULL,NULL) rejects");

    rx888_t *r = (rx888_t *)0x1;
    rc = rx888_open(&r, NULL);
    EXPECT(rc == LIBUSB_ERROR_INVALID_PARAM, "rx888_open(&r,NULL) rejects");
    EXPECT(r == NULL, "rx888_open clears *out on error");
}

static void test_open_rejects_uninit_config(void) {
    /* Calloc'd config has zero queue_depth/req_packets/ctrl_timeout_ms.
     * rx888_open must reject so callers don't accidentally use the
     * library without first calling rx888_config_init_default(). */
    rx888_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    rx888_t *r = NULL;
    int rc = rx888_open(&r, &cfg);
    EXPECT(rc == LIBUSB_ERROR_INVALID_PARAM,
           "rx888_open rejects zero-initialised config");
    EXPECT(r == NULL, "rx888_open clears *out on validation failure");
}

static void test_config_init_default(void) {
    rx888_config_t cfg;
    memset(&cfg, 0xaa, sizeof cfg);  /* poison */
    rx888_config_init_default(&cfg);
    EXPECT(cfg.samplerate         == 32000000u, "default samplerate is 32 MS/s");
    EXPECT(cfg.queue_depth        == 32u,       "default queue_depth is 32");
    EXPECT(cfg.req_packets        == 1024u,     "default req_packets is 1024");
    EXPECT(cfg.ctrl_timeout_ms    == 5000u,     "default ctrl_timeout is 5s");
    EXPECT(cfg.stream_timeout_ms  == 0u,        "default stream_timeout is 0 (infinite)");
    EXPECT(cfg.watchdog_ms        == 3000u,     "default watchdog is 3s");
    EXPECT(cfg.gain_high          == 1,         "default gain mode is high");
    EXPECT(cfg.firmware_path      == NULL,      "default firmware_path is NULL");
    EXPECT(cfg.dither             == 0,         "default dither off");
    EXPECT(cfg.randomizer         == 0,         "default randomizer off");
    EXPECT(cfg.fixup_samples      == 0,         "default fixup off");
}

static void test_open_no_device(void) {
    /* Assumes no RX888 on the host (CI runners qualify).  If a usable
     * (application-mode) device is attached, `make check` sets
     * RX888_HW_PRESENT=1 and we skip — opening it here would fail the
     * assertion and disturb the device.  Skip only on the exact value "1",
     * matching the shell smoke tests. */
    const char *hw = getenv("RX888_HW_PRESENT");
    if (hw && strcmp(hw, "1") == 0) {
        fprintf(stderr, "skip rx888_open NO_DEVICE test (RX888_HW_PRESENT=1)\n");
        return;
    }
    rx888_config_t cfg;
    rx888_config_init_default(&cfg);
    rx888_t *r = NULL;
    int rc = rx888_open(&r, &cfg);
    EXPECT(rc == LIBUSB_ERROR_NO_DEVICE,
           "rx888_open returns NO_DEVICE with no hardware present");
    EXPECT(r == NULL, "rx888_open clears *out on NO_DEVICE");
}

int main(void) {
    test_null_safety();
    test_version_string();
    test_strerror();
    test_open_validates_null();
    test_open_rejects_uninit_config();
    test_config_init_default();
    test_open_no_device();

    fprintf(stderr, "\n%s (%d failures)\n",
            failures == 0 ? "ALL OK" : "FAILED",
            failures);
    return failures == 0 ? 0 : 1;
}
