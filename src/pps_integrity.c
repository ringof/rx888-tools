/*
 * pps_integrity — long-duration PPS in-band marker fidelity test.
 *
 * Streams from the RX888 mk II via librx888 while toggling the PPS
 * marker GPIO at 1 Hz on a second (EP0-only) libusb handle, and
 * verifies that every rising edge produces exactly one short USB bulk
 * transfer (the in-band marker) with no spurious shorts and no misses.
 *
 * Two handles by design:
 *   - librx888 owns interface 0 and the async bulk EP1-IN stream.
 *   - a second libusb context opens the same device for EP0 vendor
 *     control transfers only (GPIOFX3 marker toggle + GETSTATS). EP0 is
 *     device-level, so no interface claim is needed and the two handles
 *     do not contend.
 *
 * Short-transfer detection runs in librx888's writer-thread callback;
 * the main thread coordinates via _Atomic state. See doc/pps_integrity.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "librx888.h"
#include "rx888.h"

#define PROG_NAME "pps_integrity"

/* The PPS marker is GPIO bit 9 of the GPIOFX3 control word. include/rx888.h
 * names that bit BIAS_VHF (OUTXIO9 = 0x200); the firmware overloads it as the
 * in-band marker trigger (ExtIO_sddc #125). Aliased here so the overload is
 * explicit rather than a magic number. */
#define PPS_MARKER_BIT  ((uint32_t)BIAS_VHF)

/* A transfer this many samples below the full size still counts as "full",
 * absorbing any benign under-fill. A real marker transfer is far shorter. */
#define SHORT_MARGIN    512

/* Vendor control-transfer timeout on the second handle (ms). */
#define CTRL_TIMEOUT_MS 1000

#define MARKER_DWELL_MS 10      /* rising-edge high time before falling edge */

/* GETSTATS payload (see src/fx3_cmd/fx3_stats.h — the firmware layout
 * authority; keep these offsets in sync with it). */
#define GETSTATS_LEN     30
#define GETSTATS_MIN_LEN 26

struct pps_ctx {
    _Atomic int      expecting_marker;  /* armed before each rising edge */
    _Atomic int      marker_arrived;    /* set by the first short in the window */
    _Atomic uint64_t spurious;          /* shorts with no edge / extra shorts */
    _Atomic uint32_t min_nsamples;      /* smallest transfer seen this window */
    size_t           expected_nsamples; /* full-transfer sample count */
};

/* Subset of GETSTATS we report on. */
struct fw_stats {
    uint32_t pib_errors;
    uint32_t streaming_faults;
    uint32_t boot_count;
    int      valid;
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------- callback -------------------------------- */

static void sample_cb(const int16_t *samples, size_t nsamples, void *user)
{
    (void)samples;
    struct pps_ctx *c = user;

    /* Track the smallest transfer this window so -v can show the marker
     * size (and, on a miss, whether a near-full short slipped past the
     * threshold vs. no short arriving at all). */
    uint32_t n32 = nsamples > UINT32_MAX ? UINT32_MAX : (uint32_t)nsamples;
    uint32_t cur = atomic_load(&c->min_nsamples);
    while (n32 < cur &&
           !atomic_compare_exchange_weak(&c->min_nsamples, &cur, n32))
        ;

    if (nsamples + SHORT_MARGIN < c->expected_nsamples) {
        /* A short transfer. If we armed for a marker, the first short
         * closes the window; any further short this second is spurious. */
        if (atomic_exchange(&c->expecting_marker, 0))
            atomic_store(&c->marker_arrived, 1);
        else
            atomic_fetch_add(&c->spurious, 1);
    }
}

/* --------------------------- EP0 helpers (handle 2) ---------------------- */

static int ctrl_write_u32(libusb_device_handle *h, uint8_t request, uint32_t val)
{
    uint8_t data[4] = {
        (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF), (uint8_t)((val >> 24) & 0xFF)
    };
    int r = libusb_control_transfer(
        h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, 0, 0, data, sizeof(data), CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    return (r == (int)sizeof(data)) ? 0 : LIBUSB_ERROR_IO;
}

static int read_fw_stats(libusb_device_handle *h, struct fw_stats *s)
{
    memset(s, 0, sizeof(*s));
    uint8_t buf[GETSTATS_LEN];
    int r = libusb_control_transfer(
        h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        (uint8_t)GETSTATS, 0, 0, buf, sizeof(buf), CTRL_TIMEOUT_MS);
    if (r < GETSTATS_MIN_LEN) return (r < 0) ? r : LIBUSB_ERROR_IO;
    memcpy(&s->pib_errors,       &buf[5],  4);
    memcpy(&s->streaming_faults, &buf[15], 4);
    memcpy(&s->boot_count,       &buf[20], 4);
    s->valid = 1;
    return 0;
}

/* Open a second handle on its own context for EP0-only control transfers. */
static libusb_device_handle *open_ctrl_handle(libusb_context **ctx_out)
{
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return NULL;
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (!h) { libusb_exit(ctx); return NULL; }
    *ctx_out = ctx;
    return h;
}

/* --------------------------------- time ---------------------------------- */

static void fmt_wallclock(char *out, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char hms[16];
    strftime(hms, sizeof(hms), "%H:%M:%S", &tm);
    snprintf(out, n, "%s.%06ld", hms, ts.tv_nsec / 1000);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [hours] [--rate MSPS] [--firmware FILE] [-v]\n"
        "  hours              Run duration in hours (default 4; fractional ok)\n"
        "  --rate MSPS        Sample rate in MSPS (default 16)\n"
        "  -f, --firmware FILE  Upload FX3 firmware if device is in boot mode\n"
        "  -v, --verbose      Add a per-second 'minxfer' column (smallest\n"
        "                     transfer that second, in samples; = the marker\n"
        "                     size on an ok second)\n"
        "  -h, --help         Show this help\n",
        argv0);
}

/* --------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    double hours = 4.0;
    unsigned rate_msps = 16;
    const char *firmware_path = NULL;
    int verbose = 0;

    static struct option opts[] = {
        {"rate",     required_argument, 0, 'r'},
        {"firmware", required_argument, 0, 'f'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:f:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'r': rate_msps = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'f': firmware_path = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        char *end = NULL;
        hours = strtod(argv[optind], &end);
        if (end == argv[optind] || hours < 0) {
            fprintf(stderr, "%s: bad hours '%s'\n", PROG_NAME, argv[optind]);
            return 2;
        }
        optind++;
    }
    if (optind < argc) { usage(argv[0]); return 2; }
    if (rate_msps == 0) {
        fprintf(stderr, "%s: --rate must be > 0\n", PROG_NAME);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct pps_ctx ctx = {0};

    rx888_config_t cfg;
    rx888_config_init_default(&cfg);
    cfg.samplerate    = rate_msps * 1000000u;
    cfg.firmware_path = firmware_path;  /* NULL: device must already be loaded */

    rx888_t *r = NULL;
    int rc = rx888_open(&r, &cfg);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_open: %s\n", PROG_NAME, rx888_strerror(rc));
        return 1;
    }
    rc = rx888_start(r, sample_cb, &ctx);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_start: %s\n", PROG_NAME, rx888_strerror(rc));
        rx888_close(r);
        return 1;
    }

    /* Full-transfer size is known only after start() sizes the ring. */
    size_t xfer_bytes = rx888_get_transfer_bytes(r);
    if (xfer_bytes == 0) {
        fprintf(stderr, "%s: transfer size unavailable\n", PROG_NAME);
        rx888_close(r);
        return 1;
    }
    ctx.expected_nsamples = xfer_bytes / sizeof(int16_t);

    libusb_context *ctx2 = NULL;
    libusb_device_handle *h2 = open_ctrl_handle(&ctx2);
    if (!h2) {
        fprintf(stderr, "%s: cannot open control handle\n", PROG_NAME);
        rx888_close(r);
        return 1;
    }

    /* Base GPIO mirrors what librx888 set at open (dither|randomizer), plus a
     * steady LED heartbeat. GPIOFX3 writes the full word, so the marker toggle
     * must OR these in or it would clear them. */
    uint32_t base_gpio = (uint32_t)LED_BLUE
                       | (cfg.dither     ? (uint32_t)DITH  : 0u)
                       | (cfg.randomizer ? (uint32_t)RANDO : 0u);

    struct fw_stats st_start = {0}, st_end = {0};
    read_fw_stats(h2, &st_start);

    fprintf(stderr, "%s: starting %.3f hour run @ %u MSPS "
            "(full transfer = %zu samples)\n",
            PROG_NAME, hours, rate_msps, ctx.expected_nsamples);
    printf("#time             stat   edges  marks  spur  miss%s\n",
           verbose ? "   minxfer" : "");
    fflush(stdout);

    uint64_t edges = 0, marks = 0, missed = 0;
    int internal_stop = 0, ctrl_fault = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t total_secs = (uint64_t)(hours * 3600.0 + 0.5);

    for (uint64_t sec = 0; !g_stop; sec++) {
        if (total_secs && sec >= total_secs) break;
        if (!rx888_is_running(r)) { internal_stop = 1; break; }

        char ts[48];
        fmt_wallclock(ts, sizeof(ts));

        /* Arm before the edge so a fast marker is never missed. */
        atomic_store(&ctx.marker_arrived, 0);
        atomic_store(&ctx.min_nsamples, UINT32_MAX);
        atomic_store(&ctx.expecting_marker, 1);
        if (ctrl_write_u32(h2, (uint8_t)GPIOFX3, base_gpio | PPS_MARKER_BIT) < 0)
            ctrl_fault = 1;
        edges++;

        struct timespec dwell = { .tv_sec = 0, .tv_nsec = MARKER_DWELL_MS * 1000000L };
        nanosleep(&dwell, NULL);
        if (ctrl_write_u32(h2, (uint8_t)GPIOFX3, base_gpio) < 0)
            ctrl_fault = 1;

        /* Sleep to the next 1 Hz boundary (absolute, so cadence doesn't drift). */
        struct timespec wake = t0;
        wake.tv_sec += (time_t)(sec + 1);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL) == EINTR
               && !g_stop)
            ;
        if (g_stop) break;

        /* Window closed: stop accepting this edge's marker as "on time". */
        atomic_store(&ctx.expecting_marker, 0);
        int arrived = atomic_load(&ctx.marker_arrived);
        const char *stat = "ok";
        if (arrived) {
            marks++;
        } else {
            missed++;
            stat = "MISS";
        }
        printf(" %-16s %-5s %6" PRIu64 " %6" PRIu64 " %5" PRIu64 " %5" PRIu64,
               ts, stat, edges, marks,
               (uint64_t)atomic_load(&ctx.spurious), missed);
        if (verbose) {
            uint32_t mn = atomic_load(&ctx.min_nsamples);
            if (mn == UINT32_MAX) printf("   %8s", "-");      /* no transfer */
            else                  printf("   %8" PRIu32, mn);
        }
        printf("\n");
        fflush(stdout);
    }

    rx888_stop(r);
    read_fw_stats(h2, &st_end);

    /* ------------------------------- summary ------------------------------ */
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec - t0.tv_sec);
    uint64_t spur = atomic_load(&ctx.spurious);
    int boot_changed = (st_start.valid && st_end.valid &&
                        st_start.boot_count != st_end.boot_count);

    /* ±2 startup/shutdown tolerance on edge↔marker pairing. */
    int markers_ok = (edges >= marks) && ((edges - marks) <= 2);
    int pass = (spur == 0) && markers_ok && !internal_stop &&
               !ctrl_fault && !boot_changed;

    printf("\n=== PPS INTEGRITY RESULT ===\n");
    printf("Duration:        %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 "\n",
           elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    printf("Sample rate:     %u MSPS\n", rate_msps);
    printf("Transfer size:   %zu samples (%zu bytes)\n",
           ctx.expected_nsamples, xfer_bytes);
    printf("Edges sent:      %" PRIu64 "\n", edges);
    printf("Markers seen:    %" PRIu64 "\n", marks);
    printf("Spurious shorts: %" PRIu64 "\n", spur);
    printf("Missed markers:  %" PRIu64 "\n", missed);
    if (st_start.valid && st_end.valid) {
        printf("PIB errors:      %u (NOTE, informational)\n",
               st_end.pib_errors - st_start.pib_errors);
        printf("Stream faults:   %u\n",
               st_end.streaming_faults - st_start.streaming_faults);
        printf("Boot count:      %s\n",
               boot_changed ? "CHANGED — device reset during run!" : "unchanged");
    } else {
        printf("FW stats:        unavailable\n");
    }
    if (internal_stop)
        printf("Stream:          stopped early by library (device/watchdog/usb)\n");
    if (ctrl_fault)
        printf("Control:         GPIOFX3 write error(s) on the marker handle\n");
    printf("Result: %s\n", pass ? "PASS" : "FAIL");

    rx888_close(r);
    libusb_close(h2);
    libusb_exit(ctx2);
    return pass ? 0 : 1;
}
