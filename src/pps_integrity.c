/*
 * pps_integrity — long-duration PPS in-band marker fidelity test.
 *
 * Streams from the RX888 mk II via librx888 while toggling the PPS
 * marker GPIO at 1 Hz on a second (EP0-only) libusb handle, and checks
 * that each rising edge produces one short USB bulk transfer (the in-band
 * marker). Misses are classified: "blind-spot" misses (the marker partial
 * buffer drifted within a buffer boundary, where no in-band short can
 * exist) are inherent and benign; "anomalous" misses (marker lost with the
 * remainder mid-buffer) and spurious shorts are real fidelity failures.
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

/* The marker is the leftover partial buffer each PPS second: its size is
 * (samples_per_second mod full-transfer-size), which slowly drifts with the
 * ADC-vs-host clock offset. When that remainder drifts within DANGER_BAND of a
 * buffer boundary (0 or full), the partial is either empty (suppressed) or
 * indistinguishable from a full transfer, so no in-band short can exist — an
 * inherent blind spot of a short-transfer marker, not a fidelity failure. A
 * miss is "blind-spot" when the live remainder (last good marker) sits in that
 * band, "anomalous" otherwise. The band must exceed the per-second remainder
 * jitter (~2k samples observed); the firmware's empty-partial suppression
 * threshold is unknown, so tune this once long-run data is in hand. */
#define DANGER_BAND     8192

/* A hard miss (remainder mid-buffer) is a MERGE rather than a true loss when
 * the very next marker is oversized: the skipped flush rolled forward and the
 * following edge flushed the combined partial (~2x), so no samples were lost —
 * the delimiter was merely delayed, and is reconstructable in post. We flag it
 * when the next marker reaches MERGE_PCT% of the running median marker size.
 * Heuristic: normal jitter tops out ~160% of median, a merge lands ~190%, so
 * 170% separates them. Tune against campaign data. BASE_WIN is the median
 * window over recent normal markers (recovery spikes are excluded). */
#define MERGE_PCT       170
#define BASE_WIN        32

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

/* Median of the recent-normal-marker ring (n <= BASE_WIN), for the MERGE
 * baseline. Small n, so an insertion sort on a copy is plenty. */
static uint32_t base_median(const uint32_t *ring, int n)
{
    if (n <= 0) return 0;
    uint32_t t[BASE_WIN];
    memcpy(t, ring, (size_t)n * sizeof(uint32_t));
    for (int i = 1; i < n; i++) {
        uint32_t k = t[i]; int j = i - 1;
        while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
        t[j + 1] = k;
    }
    return t[n / 2];
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
    uint64_t blind_spot = 0, merge = 0, anomalous = 0;  /* miss classification */
    uint32_t last_marker = 0;                     /* live remainder estimate */
    int last_marker_valid = 0;
    uint32_t base_ring[BASE_WIN];                 /* recent normal markers */
    int base_n = 0, base_pos = 0;
    int pending_miss = 0;                         /* hard miss awaiting next marker */
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
        uint32_t mn = atomic_load(&ctx.min_nsamples);
        const char *stat = "ok";
        char note[64]; note[0] = '\0';
        if (arrived) {
            marks++;
            uint32_t marker = (mn == UINT32_MAX) ? 0 : mn;
            int is_recovery = 0;
            /* Resolve a deferred hard miss: an oversized marker now means the
             * skipped flush merged into this one (data intact, MERGE);
             * otherwise the earlier miss was a true loss (ANOM). */
            if (pending_miss) {
                uint32_t med = base_median(base_ring, base_n);
                if (med > 0 &&
                    (uint64_t)marker * 100u >= (uint64_t)med * MERGE_PCT) {
                    merge++; is_recovery = 1;
                    snprintf(note, sizeof note, "  <- prev MISS: MERGE (data intact)");
                } else {
                    anomalous++;
                    snprintf(note, sizeof note, "  <- prev MISS: ANOM (loss)");
                }
                pending_miss = 0;
            }
            /* Feed the median baseline with normal markers only (a recovery
             * spike would bias it high). */
            if (!is_recovery && marker > 0) {
                base_ring[base_pos] = marker;
                base_pos = (base_pos + 1) % BASE_WIN;
                if (base_n < BASE_WIN) base_n++;
            }
            if (mn != UINT32_MAX) { last_marker = mn; last_marker_valid = 1; }
        } else {
            missed++;
            /* A second miss before the previous one resolved: the prior was
             * not recovered by an oversized marker, so it was a true loss. */
            if (pending_miss) { anomalous++; pending_miss = 0; }
            /* Classify. mn < expected: a near-full partial slipped past the
             * detect threshold — near-full edge of the blind spot. mn ==
             * expected: no distinguishable partial; a blind spot only if the
             * live remainder sits within DANGER_BAND of a boundary. Otherwise
             * the marker vanished mid-buffer — defer the MERGE-vs-ANOM call to
             * the next marker. */
            int blind;
            if (mn < ctx.expected_nsamples) {
                blind = 1;                       /* near-full boundary */
            } else if (!last_marker_valid) {
                blind = 1;                       /* startup: no baseline yet */
            } else {
                uint32_t to_full = (uint32_t)ctx.expected_nsamples - last_marker;
                uint32_t dist = last_marker < to_full ? last_marker : to_full;
                blind = (dist <= DANGER_BAND);
            }
            if (blind) { blind_spot++; stat = "BLIND"; }
            else       { pending_miss = 1; stat = "MISS"; }  /* resolved next edge */
        }
        printf(" %-16s %-5s %6" PRIu64 " %6" PRIu64 " %5" PRIu64 " %5" PRIu64,
               ts, stat, edges, marks,
               (uint64_t)atomic_load(&ctx.spurious), missed);
        if (verbose) {
            if (mn == UINT32_MAX) printf("   %8s", "-");      /* no transfer */
            else                  printf("   %8" PRIu32, mn);
        }
        if (note[0]) printf("%s", note);
        printf("\n");
        fflush(stdout);
    }

    /* A hard miss on the final second never saw a recovery marker: a loss. */
    if (pending_miss) { anomalous++; pending_miss = 0; }

    rx888_stop(r);
    read_fw_stats(h2, &st_end);

    /* ------------------------------- summary ------------------------------ */
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec - t0.tv_sec);
    uint64_t spur = atomic_load(&ctx.spurious);
    int boot_changed = (st_start.valid && st_end.valid &&
                        st_start.boot_count != st_end.boot_count);

    /* Verdict is about marker fidelity: blind-spot misses are an inherent,
     * phase-dependent property of the short-transfer marker and do not fail
     * the run; anomalous misses (and spurious shorts) do. */
    int pass = (spur == 0) && (anomalous == 0) && !internal_stop &&
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
    printf("Missed markers:  %" PRIu64 "  (blind-spot: %" PRIu64
           ", merge: %" PRIu64 ", anomalous: %" PRIu64 ")\n",
           missed, blind_spot, merge, anomalous);
    if (edges > 0 && merge > 0)
        printf("Merge rate:      %.3f%% of edges (~%.1f/hour)\n",
               100.0 * (double)merge / (double)edges,
               (double)merge / (double)edges * 3600.0);
    if (edges > 0 && anomalous > 0)
        printf("Anomaly rate:    %.3f%% of edges (~%.1f/hour)\n",
               100.0 * (double)anomalous / (double)edges,
               (double)anomalous / (double)edges * 3600.0);
    if (blind_spot > 0)
        printf("  NOTE: blind-spot misses are the inherent short-transfer "
               "marker limit near a buffer boundary, not a failure.\n");
    if (merge > 0)
        printf("  WARN: merged markers — a delimiter was delayed into the "
               "next edge (data intact, reconstructable). Not a hard fail.\n");
    if (anomalous > 0)
        printf("  FAIL: anomalous misses — marker lost with the remainder "
               "mid-buffer, no oversized recovery (possible data loss).\n");
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
