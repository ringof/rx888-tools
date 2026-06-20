/*
 * stream_soak — passive stream-integrity soak: the no-marker CONTROL for
 * pps_integrity.
 *
 * pps_integrity injects a 1 Hz PPS marker and validates it. This tool runs the
 * SAME data-integrity instrumentation but injects NOTHING — it just streams via
 * librx888 and measures the stream on its own terms. It is the control that
 * tells us whether anomalies (short transfers, sample loss) come from the PPS
 * marker mechanism or are inherent to the streaming path.
 *
 * What it measures (host clock assumed disciplined, NTP/PTP/GPS):
 *   - Effective sample rate from delivered-samples / elapsed-time (-> ppm vs
 *     nominal). This is the clock offset COMBINED with any uniform loss.
 *   - Buffers produced vs delivered, in BYTES: glDMACount x FW_DMA_BUF_BYTES
 *     vs samples x 2. Clock-INDEPENDENT loss check (a slow clock lowers both
 *     equally; only real loss makes produced exceed delivered). With this and
 *     bad_xfers clear, the rate offset is provably pure clock.
 *   - SHORT transfers: any transfer smaller than a full buffer. With no marker
 *     applied there is nothing to cause one, so every short is an anomaly
 *     (typically the FX3 committing a partial buffer under stress). This is the
 *     control for pps_integrity's "spurious shorts".
 *   - bad_xfers, firmware PIB overflow (polled per second), streaming faults,
 *     and a mid-run device reset (boot-count change).
 *
 * Two handles, same as pps_integrity: librx888 owns the bulk stream; a second
 * libusb context opens the device for EP0-only GETSTATS reads.
 *
 * See doc/pps_integrity.md for the loss-accounting rationale shared with the
 * marker test.
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

#define PROG_NAME "stream_soak"

/* A transfer this many samples below full still counts as "full" (benign
 * under-fill tolerance); anything shorter is a short transfer / anomaly. */
#define SHORT_MARGIN     512

#define CTRL_TIMEOUT_MS  1000

/* Firmware DMA buffer size (CONFIRMED 16 KB, SDDC_FX3 GPIF->DMA config). The
 * host aggregates several of these into one librx888 bulk transfer, so the
 * produced (glDMACount) vs delivered comparison must be done in BYTES. Keep in
 * sync with src/pps_integrity.c. */
#define FW_DMA_BUF_BYTES 16384

/* GETSTATS payload (canonical layout: src/fx3_cmd/fx3_stats.h — keep in sync). */
#define GETSTATS_LEN     30
#define GETSTATS_MIN_LEN 26

struct soak_ctx {
    _Atomic uint64_t samples_total;   /* cumulative samples delivered */
    _Atomic uint64_t short_xfers;     /* transfers < full-SHORT_MARGIN (anomalies) */
    _Atomic uint32_t min_nsamples;    /* smallest transfer this window (for -v) */
    size_t           expected_nsamples;
};

/* Subset of GETSTATS (see FW_DMA_BUF_BYTES note re: dma_count = glDMACount). */
struct fw_stats {
    uint32_t dma_count;        /* glDMACount: DMA buffers produced */
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
    struct soak_ctx *c = user;

    atomic_fetch_add(&c->samples_total, nsamples);

    uint32_t n32 = nsamples > UINT32_MAX ? UINT32_MAX : (uint32_t)nsamples;
    uint32_t cur = atomic_load(&c->min_nsamples);
    while (n32 < cur &&
           !atomic_compare_exchange_weak(&c->min_nsamples, &cur, n32))
        ;

    /* No marker is applied, so any short transfer is an anomaly. */
    if (nsamples + SHORT_MARGIN < c->expected_nsamples)
        atomic_fetch_add(&c->short_xfers, 1);
}

/* --------------------------- EP0 helper (handle 2) ---------------------- */

static int read_fw_stats(libusb_device_handle *h, struct fw_stats *s)
{
    memset(s, 0, sizeof(*s));
    uint8_t buf[GETSTATS_LEN];
    int r = libusb_control_transfer(
        h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        (uint8_t)GETSTATS, 0, 0, buf, sizeof(buf), CTRL_TIMEOUT_MS);
    if (r < GETSTATS_MIN_LEN) return (r < 0) ? r : LIBUSB_ERROR_IO;
    memcpy(&s->dma_count,        &buf[0],  4);
    memcpy(&s->pib_errors,       &buf[5],  4);
    memcpy(&s->streaming_faults, &buf[15], 4);
    memcpy(&s->boot_count,       &buf[20], 4);
    s->valid = 1;
    return 0;
}

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
        "Usage: %s [hours] [--rate MSPS] [--firmware FILE] [-q N] [-p N] [-v]\n"
        "  hours              Run duration in hours (default 4; fractional ok)\n"
        "  --rate MSPS        Sample rate in MSPS (default 16; fractional ok)\n"
        "  -f, --firmware FILE  Upload FX3 firmware if device is in boot mode\n"
        "  -q, --queuedepth N   Concurrent in-flight USB transfers (default 32)\n"
        "  -p, --reqsize N    USB transfer size in packets (default 1024)\n"
        "  -v, --verbose      Add a per-second 'minxfer' column (samples)\n"
        "  -h, --help         Show this help\n"
        "\n"
        "The no-marker control for pps_integrity: streams and measures rate,\n"
        "produced-vs-delivered loss, short transfers, PIB and faults, without\n"
        "injecting a PPS marker.\n",
        argv0);
}

/* --------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    double hours = 4.0;
    double rate_msps = 16.0;
    const char *firmware_path = NULL;
    int verbose = 0;
    unsigned queuedepth = 0, reqsize = 0;

    static struct option opts[] = {
        {"rate",       required_argument, 0, 'r'},
        {"firmware",   required_argument, 0, 'f'},
        {"queuedepth", required_argument, 0, 'q'},
        {"reqsize",    required_argument, 0, 'p'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:f:q:p:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'r': {
            char *end = NULL;
            rate_msps = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || rate_msps <= 0) {
                fprintf(stderr, "%s: bad --rate '%s'\n", PROG_NAME, optarg);
                return 2;
            }
            break;
        }
        case 'f': firmware_path = optarg; break;
        case 'q': queuedepth = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'p': reqsize     = (unsigned)strtoul(optarg, NULL, 10); break;
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

    unsigned samplerate = (unsigned)(rate_msps * 1e6 + 0.5);
    if (samplerate == 0) {
        fprintf(stderr, "%s: --rate must be > 0\n", PROG_NAME);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct soak_ctx ctx = {0};

    rx888_config_t cfg;
    rx888_config_init_default(&cfg);
    cfg.samplerate    = samplerate;
    cfg.firmware_path = firmware_path;
    if (queuedepth) cfg.queue_depth = queuedepth;
    if (reqsize)    cfg.req_packets = reqsize;

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

    struct fw_stats st_start = {0}, st_end = {0};
    read_fw_stats(h2, &st_start);
    rx888_stats_t lib_start;
    rx888_get_stats(r, &lib_start);
    uint64_t samples_start = atomic_load(&ctx.samples_total);

    fprintf(stderr, "%s: starting %.3f hour run @ %g MSPS (%u Hz), no marker "
            "(full transfer = %zu samples)\n",
            PROG_NAME, hours, rate_msps, samplerate, ctx.expected_nsamples);
    printf("#time             sec    Msa/s   total_Gsa  short   pib%s\n",
           verbose ? "   minxfer" : "");
    fflush(stdout);

    uint64_t prev_samples = samples_start;
    uint32_t pib_prev = st_start.valid ? st_start.pib_errors : 0;
    int mid_reset = 0, internal_stop = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t total_secs = (uint64_t)(hours * 3600.0 + 0.5);

    for (uint64_t sec = 0; !g_stop; sec++) {
        if (total_secs && sec >= total_secs) break;
        if (!rx888_is_running(r)) { internal_stop = 1; break; }

        atomic_store(&ctx.min_nsamples, UINT32_MAX);

        /* Sleep to the next 1 Hz boundary (absolute, no drift). */
        struct timespec wake = t0;
        wake.tv_sec += (time_t)(sec + 1);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL) == EINTR
               && !g_stop)
            ;
        if (g_stop) break;

        char ts[48];
        fmt_wallclock(ts, sizeof(ts));

        uint64_t now_samples = atomic_load(&ctx.samples_total);
        uint64_t this_sec = now_samples - prev_samples;   /* data-vs-time rate */
        prev_samples = now_samples;
        uint32_t mn = atomic_load(&ctx.min_nsamples);

        char note[64]; note[0] = '\0';
        struct fw_stats cur;
        uint32_t pib_run = pib_prev;
        if (read_fw_stats(h2, &cur) == 0) {
            pib_run = cur.pib_errors;
            if (cur.pib_errors > pib_prev) {
                snprintf(note, sizeof note, "  PIB+%u", cur.pib_errors - pib_prev);
                pib_prev = cur.pib_errors;
            }
            if (st_start.valid && cur.boot_count != st_start.boot_count && !mid_reset) {
                mid_reset = 1;
                snprintf(note + strlen(note), sizeof note - strlen(note),
                         "  DEVICE RESET");
            }
        }

        printf(" %-16s %5" PRIu64 " %7.2f %10.3f %6" PRIu64 " %5" PRIu32,
               ts, sec + 1, (double)this_sec / 1e6,
               (double)now_samples / 1e9,
               (uint64_t)atomic_load(&ctx.short_xfers),
               (st_start.valid ? pib_run - st_start.pib_errors : 0));
        if (verbose) {
            if (mn == UINT32_MAX) printf("   %8s", "-");
            else                  printf("   %8" PRIu32, mn);
        }
        if (note[0]) printf("%s", note);
        printf("\n");
        fflush(stdout);
    }

    /* End counters BEFORE rx888_stop() (STOPFX3 resets glDMACount). */
    read_fw_stats(h2, &st_end);
    rx888_stats_t lib;
    rx888_get_stats(r, &lib);
    rx888_stop(r);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec - t0.tv_sec);
    double elapsed_d = (double)(t1.tv_sec - t0.tv_sec)
                     + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t delivered = atomic_load(&ctx.samples_total);
    uint64_t shorts    = atomic_load(&ctx.short_xfers);
    int boot_changed = (st_start.valid && st_end.valid &&
                        st_start.boot_count != st_end.boot_count);
    uint32_t pib_delta = (st_start.valid && st_end.valid)
                       ? st_end.pib_errors - st_start.pib_errors : 0;

    /* Produced vs delivered, in BYTES (clock-independent loss). */
    uint32_t bufs_produced   = st_end.valid && st_start.valid
                             ? st_end.dma_count - st_start.dma_count : 0;
    uint64_t produced_bytes  = (uint64_t)bufs_produced * (uint64_t)FW_DMA_BUF_BYTES;
    uint64_t delivered_bytes = (delivered - samples_start) * sizeof(int16_t);
    /* Subtract the in-flight DELTA (end - start), not the end value: produced
     * and delivered are both measured from the start snapshot, so the steady
     * in-flight already cancels in their difference; only a change in pipeline
     * occupancy between the two snapshots needs removing. (Subtracting the end
     * value double-counted one queue depth, biasing undelivered by ~ -queue.) */
    int64_t  inflight_delta  = ((int64_t)lib.in_flight - (int64_t)lib_start.in_flight)
                             * (int64_t)xfer_bytes;
    int64_t  undeliv_bytes   = (int64_t)produced_bytes - (int64_t)delivered_bytes
                             - inflight_delta;
    int64_t  byte_slack      = 4 * (int64_t)xfer_bytes;
    int pd_valid = st_start.valid && st_end.valid && delivered_bytes > 0
                 && produced_bytes > delivered_bytes / 4
                 && produced_bytes < delivered_bytes * 4;
    int      bufs_lost  = pd_valid && (undeliv_bytes > byte_slack);
    int64_t  lost_bytes = undeliv_bytes > 0 ? undeliv_bytes : 0;
    double   loss_ppm   = produced_bytes > 0
                        ? (double)lost_bytes / (double)produced_bytes * 1e6 : 0.0;

    /* Effective rate from data vs time = clock offset + any uniform loss. */
    double eff_rate = elapsed_d > 0 ? (double)(delivered - samples_start) / elapsed_d : 0.0;
    double rate_ppm = samplerate > 0
                    ? (eff_rate - (double)samplerate) / (double)samplerate * 1e6 : 0.0;

    int sample_loss = (lib.bad_xfers > 0) || bufs_lost;
    int lossless = !sample_loss;
    int pass = !sample_loss && !internal_stop && !boot_changed && !mid_reset;

    printf("\n=== STREAM SOAK RESULT (no marker) ===\n");
    printf("Duration:        %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 "\n",
           elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    printf("Sample rate:     %g MSPS (%u Hz)\n", rate_msps, samplerate);
    printf("Transfer size:   %zu samples (%zu bytes)\n",
           ctx.expected_nsamples, xfer_bytes);
    printf("Samples in:      %" PRIu64 " (%.2f Gsa, %.1f MB/s)\n",
           delivered, (double)delivered / 1e9,
           elapsed_d > 0 ? delivered_bytes / elapsed_d / 1e6 : 0.0);
    printf("Effective rate:  %.1f Sa/s (%+.3f ppm vs nominal)\n", eff_rate, rate_ppm);
    if (st_start.valid && st_end.valid) {
        if (!pd_valid)
            printf("DMA buffers:     produced %.2f GB vs delivered %.2f GB — ratio "
                   "off, FW_DMA_BUF_BYTES unverified (check INDETERMINATE)\n",
                   (double)produced_bytes / 1e9, (double)delivered_bytes / 1e9);
        else
            printf("DMA buffers:     produced %.2f GB, delivered %.2f GB, "
                   "undelivered %+.3f MB (in-flight %u->%u, slack %.2f MB)\n",
                   (double)produced_bytes / 1e9, (double)delivered_bytes / 1e9,
                   (double)undeliv_bytes / 1e6,
                   lib_start.in_flight, lib.in_flight, (double)byte_slack / 1e6);
    }
    printf("USB transfers:   ok=%llu bad=%llu\n", lib.ok_xfers, lib.bad_xfers);
    printf("Short transfers: %" PRIu64 "  (anomalous — no marker applied)\n", shorts);
    if (bufs_lost)
        printf("  FAIL: %.3f MB produced but never delivered = %.1f ppm "
               "(%.4f%%) — firmware/USB dropped data (clock-independent).\n",
               (double)lost_bytes / 1e6, loss_ppm, loss_ppm / 1e4);
    if (lib.bad_xfers > 0)
        printf("  FAIL: %llu errored/cancelled USB transfers — transport loss.\n",
               lib.bad_xfers);
    if (shorts > 0)
        printf("  WARN: short transfers with no marker — FX3 committing partial "
               "buffers (stress/overflow symptom).\n");
    if (st_start.valid && st_end.valid) {
        if (lossless && pd_valid)
            printf("ADC clock:       %+.3f ppm (lossless: produced==delivered, "
                   "no USB errors; assumes disciplined host)\n", rate_ppm);
        else if (pd_valid)
            printf("Implied clock:   %+.3f ppm (rate %+.3f + loss %.1f ppm added "
                   "back) — fast clock masking the loss\n",
                   rate_ppm + loss_ppm, rate_ppm, loss_ppm);
        printf("PIB errors:      %u\n", pib_delta);
        printf("Stream faults:   %u\n",
               st_end.streaming_faults - st_start.streaming_faults);
        printf("Boot count:      %s\n",
               (boot_changed || mid_reset) ? "CHANGED — device reset during run!"
                                           : "unchanged");
    } else {
        printf("FW stats:        unavailable\n");
    }
    if (internal_stop)
        printf("Stream:          stopped early by library (device/watchdog/usb)\n");
    printf("Result: %s\n", pass ? "PASS" : "FAIL");

    rx888_close(r);
    libusb_close(h2);
    libusb_exit(ctx2);
    return pass ? 0 : 1;
}
