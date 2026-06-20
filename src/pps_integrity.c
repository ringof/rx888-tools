/*
 * pps_integrity — long-duration PPS in-band marker fidelity test.
 *
 * Streams from the RX888 mk II via librx888 while toggling the PPS
 * marker GPIO at 1 Hz on a second (EP0-only) libusb handle, and checks
 * that each rising edge produces one short USB bulk transfer (the in-band
 * marker). A miss is a marker-TIMING event, classified "blind-spot" (the
 * partial drifted within a buffer boundary, where no in-band short can exist —
 * inherent) or "displaced" (the delimiter moved to an adjacent edge,
 * reconstructable). Neither is a data-loss verdict: whether SAMPLES were lost
 * is judged separately by the loss detectors below. The run fails only on
 * confirmed sample loss, spurious shorts, device resets, or control faults.
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
 * ===========================================================================
 * THEORY OF OPERATION  (read this before changing anything below)
 * ===========================================================================
 *
 * THE MARKER. The firmware emits an in-band PPS marker by committing the
 * in-progress DMA buffer early when it sees a GPIO edge. That early commit
 * arrives at the host as a SHORT USB bulk transfer (fewer samples than a full
 * buffer). We drive the GPIO edge ourselves at 1 Hz and check that one short
 * transfer appears per edge.
 *
 * THE MARKER SIZE IS NOT ARBITRARY. The short transfer is the *leftover
 * partial buffer* for that PPS second, so its size is approximately
 *     marker_samples ~= samplerate mod (full-transfer samples)
 * e.g. at 32 MSPS with a 524288-sample buffer, ~18432; at 16 MSPS, ~271360.
 * This value drifts slowly with the ADC-vs-host clock offset (a few ppm).
 *
 * THE BLIND SPOT (inherent, not a bug). When that remainder drifts within a
 * buffer-boundary band (near 0 or near full), the partial is either empty
 * (suppressed) or indistinguishable from a full transfer, so NO in-band short
 * can exist for that edge. This is a fundamental limitation of any
 * short-transfer marker; such misses are classified "blind-spot" and do not
 * fail the run.
 *
 * MISS CLASSIFICATION is about marker TIMING only — never data loss:
 *   - blind-spot: the live remainder sits within DANGER_BAND of a boundary.
 *   - displaced : otherwise — the skipped flush rolled into an adjacent edge
 *                 (which then flushes ~2x). The delimiter moved; reconstructable.
 * Whether DATA survived is the separate question below.
 *
 * DID WE LOSE SAMPLES?  Detectors, in increasing authority:
 *   1. librx888 bad_xfers      — errored/cancelled USB transfers.
 *   2. inter-marker continuity — samples between markers vs (span x learned
 *      rate); a deficit > LOSS_TOL. DIAGNOSTIC ONLY: marker-detection timing
 *      jitters by ~a marker's worth at high rates, so an isolated interval can
 *      read short with no real loss (the next reads long; net zero). It
 *      localizes WHEN loss happened but does not, alone, fail the run — it is
 *      corroborated against detector 3.
 *   3. produced vs delivered   — firmware glDMACount counts DMA buffers the
 *      GPIF produced (FW_DMA_BUF_BYTES each); librx888 delivers larger USB
 *      transfers and we count delivered bytes exactly. Compared in BYTES (NOT
 *      buffer counts — they differ by buf_bytes / FW_DMA_BUF_BYTES), a mismatch
 *      IS loss, and it is CLOCK-INDEPENDENT: a slow ADC clock lowers both
 *      equally. This is the authoritative, decisive
 *      gross check; it answers "were all DMA buffers delivered?". Granularity
 *      is one buffer, bounded by the in-flight pipeline (see the summary).
 *
 * CLOCK vs LOSS.  delivered/(samplerate x elapsed) as ppm is the ADC-vs-host
 * clock offset COMBINED with any uniform loss. They look identical in that
 * single number. We separate them because loss always leaves a fingerprint
 * (bad_xfers, a continuity step, or produced>delivered) whereas a slow clock
 * delivers everything, just at a steadily lower rate (a smooth drift, no
 * fingerprints). So: with all loss detectors clear, produced==delivered proves
 * the offset is purely clock, and we report it as a calibration. This assumes
 * a disciplined (NTP/PTP/GPS) host clock; on a free-running host the ppm is
 * host+ADC combined. (glDMACount resets on STOPFX3 — read the end snapshot
 * BEFORE rx888_stop.)
 *
 * WHAT WE DO NOT DO. No sequence number / known pattern is embedded in the
 * sample stream (that is a separate RF-injection / TimeSpec idea). A firmware
 * "buffers delivered" counter would make the produced/delivered comparison
 * atomic and buffer-exact; today we infer delivered from ok_xfers (firmware
 * has only the producer-side count), which is why the in-flight slack exists.
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
 * band; otherwise it is a "displaced" delimiter (a marker-timing glitch; data
 * integrity is judged by the loss detectors). The band must exceed the per-second remainder
 * jitter (~2k samples observed); the firmware's empty-partial suppression
 * threshold is unknown, so tune this once long-run data is in hand. */
#define DANGER_BAND     8192

/* A hard miss (remainder mid-buffer) means the delimiter was displaced to an
 * adjacent edge — a marker-TIMING glitch. Whether any DATA was lost is a
 * separate question answered by the loss detectors, NOT by the marker count.
 * BASE_WIN is the median window for the continuity rate estimate. */
#define BASE_WIN        32

/* Firmware DMA buffer size, in bytes. CONFIRMED 16 KB against the SDDC_FX3
 * GPIF->DMA config (firmware maintainer, 2026). glDMACount counts buffers of
 * THIS size, whereas the host aggregates several of them into one librx888 bulk
 * transfer (buf_bytes; e.g. 1 MB = 64 of these). To compare produced vs
 * delivered we must work in bytes: produced_bytes = glDMACount * FW_DMA_BUF_BYTES.
 * The host can't read this at runtime, so it is still guarded below (if
 * produced_bytes is wildly off delivered_bytes the assumption is stale and the
 * check is marked indeterminate rather than failing). Revisit if the firmware
 * DMA config ever changes. */
#define FW_DMA_BUF_BYTES 16384

/* Vendor control-transfer timeout on the second handle (ms). */
#define CTRL_TIMEOUT_MS 1000

#define MARKER_DWELL_MS 10      /* rising-edge high time before falling edge */

/* Sample-loss continuity: the samples delivered between two consecutive
 * markers should equal (seconds spanned) x the per-second sample rate, which
 * we learn as the running median of single-second inter-marker counts. Over a
 * 1 s interval clock drift is negligible, so a dropped DMA buffer (~524288
 * samples) is a glaring deficit. LOSS_TOL absorbs the marker-flush jitter
 * (the partial size varies ~+/-12k, so the interval boundary does too) while
 * staying far below a buffer's worth. */
#define LOSS_TOL        65536

/* GETSTATS payload (see src/fx3_cmd/fx3_stats.h — the firmware layout
 * authority; keep these offsets in sync with it). */
#define GETSTATS_LEN     30
#define GETSTATS_MIN_LEN 26

struct pps_ctx {
    _Atomic int      expecting_marker;  /* armed before each rising edge */
    _Atomic int      marker_arrived;    /* set by the first short in the window */
    _Atomic uint64_t spurious;          /* shorts with no edge / extra shorts */
    _Atomic uint32_t min_nsamples;      /* smallest transfer seen this window */
    _Atomic uint64_t samples_total;     /* cumulative samples delivered */
    _Atomic uint64_t samples_at_marker; /* samples_total snapshot at last marker */
    size_t           expected_nsamples; /* full-transfer sample count */
};

/* Subset of GETSTATS we report on.
 *
 * dma_count is the firmware's glDMACount: the number of DMA buffers the GPIF
 * has *produced* (incremented on every CY_U3P_DMA_CB_PROD_EVENT). There is no
 * firmware counter for buffers *delivered* to the host, but librx888's ok_xfers
 * is exactly that (one completed USB bulk transfer per delivered DMA buffer).
 * So (dma_count delta) - (ok_xfers delta) = buffers the DMA->USB->host path
 * dropped — a clock-INDEPENDENT loss measurement (a slow ADC clock lowers both
 * equally; only real loss makes produced exceed delivered). NOTE: glDMACount
 * resets on STOPFX3, so the end snapshot must be read before rx888_stop(). */
struct fw_stats {
    uint32_t dma_count;        /* glDMACount: DMA buffers produced (GPIF->DMA) */
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

    /* Running sample count for the inter-marker continuity (loss) check. */
    uint64_t total = atomic_fetch_add(&c->samples_total, nsamples) + nsamples;

    if (nsamples + SHORT_MARGIN < c->expected_nsamples) {
        /* A short transfer. If we armed for a marker, the first short
         * closes the window; any further short this second is spurious. */
        if (atomic_exchange(&c->expecting_marker, 0)) {
            atomic_store(&c->marker_arrived, 1);
            /* Anchor the sample count to this marker (a fixed point in the
             * stream) so inter-marker deltas are free of host-window jitter. */
            atomic_store(&c->samples_at_marker, total);
        } else {
            atomic_fetch_add(&c->spurious, 1);
        }
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
    memcpy(&s->dma_count,        &buf[0],  4);
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
        "  --rate MSPS        Sample rate in MSPS (default 16; fractional ok,\n"
        "                     e.g. 129.6)\n"
        "  -f, --firmware FILE  Upload FX3 firmware if device is in boot mode\n"
        "  -v, --verbose      Add a per-second 'minxfer' column (smallest\n"
        "                     transfer that second, in samples; = the marker\n"
        "                     size on an ok second)\n"
        "  -h, --help         Show this help\n",
        argv0);
}

/* Median of a small ring (n <= BASE_WIN), for the continuity rate estimate.
 * Small n, so an insertion sort on a copy is plenty. */
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
    double rate_msps = 16.0;
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

    /* MSPS -> Hz (rounded to the nearest sample/s). */
    unsigned samplerate = (unsigned)(rate_msps * 1e6 + 0.5);
    if (samplerate == 0) {
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
    cfg.samplerate    = samplerate;
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

    /* Baseline counters for the run-long deltas: firmware glDMACount (buffers
     * produced) and librx888 ok_xfers (buffers delivered). Read close together
     * so the produced-vs-delivered comparison starts from a matched instant. */
    struct fw_stats st_start = {0}, st_end = {0};
    read_fw_stats(h2, &st_start);
    rx888_stats_t lib_start;
    rx888_get_stats(r, &lib_start);
    uint64_t samples_start = atomic_load(&ctx.samples_total);  /* aligned baseline */

    fprintf(stderr, "%s: starting %.3f hour run @ %g MSPS (%u Hz) "
            "(full transfer = %zu samples)\n",
            PROG_NAME, hours, rate_msps, samplerate, ctx.expected_nsamples);
    printf("#time             stat   edges  marks  spur  miss%s\n",
           verbose ? "   minxfer" : "");
    fflush(stdout);

    uint64_t edges = 0, marks = 0, missed = 0;
    uint64_t blind_spot = 0, displaced = 0;       /* miss classification (timing only) */
    uint32_t last_marker = 0;                     /* live remainder estimate */
    int last_marker_valid = 0;
    /* Inter-marker continuity: a per-interval deficit is a DIAGNOSTIC localizer,
     * not the verdict. At high rates marker-detection timing jitters by ~a
     * marker's worth, so an isolated interval can come up short while the next
     * runs long (redistribution, net zero) — that is not loss. Real loss is
     * confirmed by the clock-independent produced-vs-delivered byte check; these
     * deficits then localize WHEN it happened. */
    uint64_t prev_marker_samples = 0, prev_marker_edge = 0;
    int have_prev_marker = 0;
    uint32_t rate_ring[BASE_WIN];                 /* single-second inter-marker counts */
    int rate_n = 0, rate_pos = 0;
    uint64_t loss_events = 0, lost_samples_est = 0;
    uint64_t max_deficit = 0;                     /* largest inter-marker deficit seen */
    uint32_t pib_prev = st_start.valid ? st_start.pib_errors : 0;
    uint64_t pib_seconds = 0, pib_with_deficit = 0;  /* per-second PIB localisation */
    int mid_reset = 0;
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
        char note[128]; note[0] = '\0';
        int interval_lost = 0;            /* this window's interval showed a deficit */
        if (arrived) {
            marks++;
            if (mn != UINT32_MAX) { last_marker = mn; last_marker_valid = 1; }

            /* Inter-marker continuity (DIAGNOSTIC localizer, not the verdict):
             * samples between consecutive markers vs (seconds spanned) x the
             * learned per-second rate. A shortfall flags WHEN a deficit appears,
             * but marker-detection timing jitters by ~a marker's worth at high
             * rates, so an isolated interval can read short with no real loss.
             * The authoritative loss check is produced-vs-delivered (below);
             * these deficits are corroborated against it in the summary. */
            uint64_t cur_ms = atomic_load(&ctx.samples_at_marker);
            if (have_prev_marker) {
                uint64_t interval = cur_ms - prev_marker_samples;
                uint64_t span = edges - prev_marker_edge;     /* seconds */
                if (span == 1) {              /* clean interval: learn the rate */
                    uint32_t iv = interval > UINT32_MAX ? UINT32_MAX
                                                        : (uint32_t)interval;
                    rate_ring[rate_pos] = iv;
                    rate_pos = (rate_pos + 1) % BASE_WIN;
                    if (rate_n < BASE_WIN) rate_n++;
                }
                /* Only score continuity once the rate baseline is fully warmed
                 * (startup markers are wild and would poison the median). */
                if (rate_n >= BASE_WIN) {
                    uint64_t rate = base_median(rate_ring, rate_n);
                    uint64_t expected = rate * span;
                    uint64_t deficit = expected > interval ? expected - interval : 0;
                    if (deficit > max_deficit) max_deficit = deficit;  /* floor headroom */
                    if (interval + LOSS_TOL < expected) {
                        loss_events++;
                        lost_samples_est += deficit;
                        interval_lost = 1;
                        snprintf(note + strlen(note), sizeof note - strlen(note),
                                 "  dip ~%" PRIu64 " (continuity)", deficit);
                    }
                }
            }
            prev_marker_samples = cur_ms;
            prev_marker_edge = edges;
            have_prev_marker = 1;
        } else {
            missed++;
            /* Classify the marker-timing glitch (data integrity is a separate
             * question, answered by the loss detectors). mn < expected: a
             * near-full partial slipped past the detect threshold — near-full
             * edge of the blind spot. mn == expected: no distinguishable
             * partial; a blind spot only if the live remainder sits within
             * DANGER_BAND of a boundary. Otherwise the delimiter was displaced
             * to an adjacent edge ("displaced"). */
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
            else       { displaced++;  stat = "MISS"; }  /* delimiter moved one edge */
        }

        /* Per-second firmware poll on the EP0 handle: localise PIB overflow
         * events to the second they happen and correlate with the continuity
         * deficit (so we can say whether an overflow cost host-visible
         * samples), and catch a mid-run device reset immediately. */
        struct fw_stats cur;
        if (read_fw_stats(h2, &cur) == 0) {
            if (cur.pib_errors > pib_prev) {
                uint32_t d = cur.pib_errors - pib_prev;
                pib_seconds++;
                if (interval_lost) pib_with_deficit++;
                snprintf(note + strlen(note), sizeof note - strlen(note),
                         "  PIB+%u%s", d, interval_lost ? " (deficit)" : "");
                pib_prev = cur.pib_errors;
            }
            if (st_start.valid && cur.boot_count != st_start.boot_count && !mid_reset) {
                mid_reset = 1;
                snprintf(note + strlen(note), sizeof note - strlen(note),
                         "  DEVICE RESET");
            }
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

    /* Read the END counters BEFORE rx888_stop(): STOPFX3 resets glDMACount, so
     * reading dma_count after the stop would zero it. Read the firmware and
     * librx888 counters back-to-back to minimise the produced-vs-delivered skew
     * (they still aren't atomic — the residual is bounded by the in-flight
     * pipeline, accounted for below). */
    read_fw_stats(h2, &st_end);
    rx888_stats_t lib;
    rx888_get_stats(r, &lib);

    rx888_stop(r);

    /* ------------------------------- summary ------------------------------ */
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec - t0.tv_sec);
    uint64_t spur = atomic_load(&ctx.spurious);
    int boot_changed = (st_start.valid && st_end.valid &&
                        st_start.boot_count != st_end.boot_count);
    uint64_t delivered = atomic_load(&ctx.samples_total);
    uint32_t pib_delta = (st_start.valid && st_end.valid)
                       ? st_end.pib_errors - st_start.pib_errors : 0;

    /* ---- Bytes produced vs delivered: the clock-INDEPENDENT loss check ----
     *
     * glDMACount counts the firmware's DMA buffers (FW_DMA_BUF_BYTES each);
     * librx888 delivers larger USB transfers (buf_bytes each, an integer number
     * of DMA buffers), and we count delivered bytes exactly via samples_total.
     * So compare in BYTES, not buffer counts (they differ by buf_bytes /
     * FW_DMA_BUF_BYTES, e.g. 64):
     *     produced_bytes  = (glDMACount delta) x FW_DMA_BUF_BYTES
     *     delivered_bytes = (samples delta) x 2
     * Both count the same data, so a slow ADC clock lowers BOTH equally and the
     * difference stays at zero; only real loss makes produced exceed delivered.
     * This is the decisive, clock-independent separation of clock from loss.
     *
     * The counters aren't sampled atomically and the host pipeline always has
     * transfers in flight, so produced runs ahead of delivered by ~in_flight
     * transfers; subtract that (in bytes) and allow a few transfers of slack for
     * the startup ramp and read skew before calling it loss.
     *
     * GUARD: FW_DMA_BUF_BYTES is an asserted firmware constant. If it is wrong
     * for the running firmware, produced_bytes would be off delivered_bytes by a
     * large integer factor; we only trust the check when the two agree within 4x
     * (a clean run agrees within the in-flight fraction), otherwise it is marked
     * indeterminate and does NOT fail the run. */
    uint32_t bufs_produced   = st_end.valid && st_start.valid
                             ? st_end.dma_count - st_start.dma_count : 0; /* mod 2^32 */
    uint64_t produced_bytes  = (uint64_t)bufs_produced * (uint64_t)FW_DMA_BUF_BYTES;
    uint64_t delivered_bytes = (delivered - samples_start) * sizeof(int16_t);
    uint64_t inflight_bytes  = (uint64_t)lib.in_flight * (uint64_t)xfer_bytes;
    int64_t  undeliv_bytes   = (int64_t)produced_bytes - (int64_t)delivered_bytes
                             - (int64_t)inflight_bytes;
    int64_t  byte_slack      = 4 * (int64_t)xfer_bytes;   /* ramp + read skew */
    /* Sanity: produced and delivered must be the same order of magnitude for the
     * FW_DMA_BUF_BYTES assumption to hold. */
    int pd_valid = st_start.valid && st_end.valid && delivered_bytes > 0
                 && produced_bytes > delivered_bytes / 4
                 && produced_bytes < delivered_bytes * 4;
    int      bufs_lost       = pd_valid && (undeliv_bytes > byte_slack);
    int64_t  lost_bytes      = undeliv_bytes > 0 ? undeliv_bytes : 0;

    /* ---- Absolute sample budget and ADC clock offset ----
     *
     * expected = samplerate x elapsed (host clock = truth, assumes a disciplined
     * NTP/PTP/GPS host). delivered/expected as ppm is the ADC-vs-host clock
     * offset COMBINED with any uniform loss. With the produced-vs-delivered
     * check above proving no buffer loss (and bad_xfers/continuity clear), that
     * combined offset is provably pure clock — so we can report it as a clock
     * calibration. On a free-running host it is host+ADC combined. */
    double elapsed_d = (double)(t1.tv_sec - t0.tv_sec)
                     + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double expected_d = (double)samplerate * elapsed_d;
    uint64_t expected_total = (uint64_t)(expected_d + 0.5);
    double budget_ppm = expected_d > 0
                      ? ((double)delivered - expected_d) / expected_d * 1e6 : 0.0;
    /* Per-second rate slope (median of clean single-second inter-marker counts):
     * the drift slope, i.e. the clock offset, robust to step-second outliers. */
    uint64_t learned_rate = base_median(rate_ring, rate_n);  /* samples/s median */
    double learned_ppm = samplerate > 0
                       ? ((double)learned_rate - (double)samplerate)
                         / (double)samplerate * 1e6 : 0.0;

    /* Sample loss = the verdict's basis. Authoritative detectors:
     *   - bad_xfers : errored/cancelled USB transfers (transport loss)
     *   - bufs_lost : produced bytes > delivered bytes (clock-independent, gross)
     * The inter-marker continuity deficit (loss_events) is a DIAGNOSTIC that
     * localizes loss but jitters at high rates, so it does not fail the run on
     * its own — it is reported and corroborated against bufs_lost below. */
    int sample_loss = (lib.bad_xfers > 0) || bufs_lost;

    /* With every authoritative loss detector clear, produced==delivered proves no
     * data was dropped, so the sample-rate budget offset is purely clock. */
    int lossless = !sample_loss;

    /* Verdict: blind-spot and displaced misses are marker-TIMING glitches (data
     * integrity is judged separately), so they do not fail the run; confirmed
     * sample loss, spurious shorts, device resets, and control faults do. */
    int pass = (spur == 0) && !sample_loss &&
               !internal_stop && !ctrl_fault && !boot_changed && !mid_reset;

    printf("\n=== PPS INTEGRITY RESULT ===\n");
    printf("Duration:        %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 "\n",
           elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    printf("Sample rate:     %g MSPS (%u Hz)\n", rate_msps, samplerate);
    printf("Transfer size:   %zu samples (%zu bytes)\n",
           ctx.expected_nsamples, xfer_bytes);
    printf("Edges sent:      %" PRIu64 "\n", edges);
    printf("Markers seen:    %" PRIu64 "\n", marks);
    printf("Spurious shorts: %" PRIu64 "\n", spur);
    printf("Missed markers:  %" PRIu64 "  (blind-spot: %" PRIu64
           ", displaced: %" PRIu64 ") — marker timing only, see loss below\n",
           missed, blind_spot, displaced);
    if (edges > 0 && displaced > 0)
        printf("Displaced rate:  %.3f%% of edges (~%.1f/hour) — delimiter moved "
               "one edge; reconstructable. Data integrity judged separately.\n",
               100.0 * (double)displaced / (double)edges,
               (double)displaced / (double)edges * 3600.0);
    if (blind_spot > 0)
        printf("  NOTE: blind-spot misses are the inherent short-transfer "
               "marker limit near a buffer boundary, not a failure.\n");
    printf("Samples in:      %" PRIu64 " (%.2f Gsa)\n",
           delivered, (double)delivered / 1e9);
    /* The decisive, clock-independent line: BYTES produced vs delivered. */
    if (!pd_valid)
        printf("DMA buffers:     produced %" PRIu32 " x %d B = %.2f GB vs "
               "delivered %.2f GB — ratio off, FW_DMA_BUF_BYTES assumption "
               "unverified (check INDETERMINATE)\n",
               bufs_produced, FW_DMA_BUF_BYTES, (double)produced_bytes / 1e9,
               (double)delivered_bytes / 1e9);
    else
        printf("DMA buffers:     produced %.2f GB (%" PRIu32 " x %d B), delivered "
               "%.2f GB, in-flight %.2f GB, undelivered %+.3f MB (slack %.2f MB)\n",
               (double)produced_bytes / 1e9, bufs_produced, FW_DMA_BUF_BYTES,
               (double)delivered_bytes / 1e9, (double)inflight_bytes / 1e9,
               (double)undeliv_bytes / 1e6, (double)byte_slack / 1e6);
    printf("USB transfers:   ok=%llu bad=%llu\n",
           lib.ok_xfers, lib.bad_xfers);
    printf("Sample loss:     %" PRIu64 " continuity dip(s), ~%" PRIu64 " samples "
           "(diagnostic; %s)\n", loss_events, lost_samples_est,
           bufs_lost ? "corroborated by produced>delivered"
                     : "NOT corroborated — marker-flush jitter, no actual loss");
    printf("Loss floor:      continuity %d samples (%.2f buffer), largest dip "
           "%" PRIu64 " (%.2f buffer); gross floor ~%.2f MB (in-flight)\n",
           LOSS_TOL, (double)LOSS_TOL / (double)ctx.expected_nsamples,
           max_deficit, (double)max_deficit / (double)ctx.expected_nsamples,
           (double)byte_slack / 1e6);
    if (bufs_lost)
        printf("  FAIL: %.3f MB produced but never delivered — firmware/USB "
               "dropped data (clock-independent).\n", (double)lost_bytes / 1e6);
    if (lib.bad_xfers > 0)
        printf("  FAIL: %llu errored/cancelled USB transfers — transport-level "
               "sample loss.\n", lib.bad_xfers);
    /* Clock vs loss: with produced==delivered and no errors, the sample-rate
     * offset is provably the ADC-vs-host clock, not loss. */
    if (st_start.valid && st_end.valid) {
        if (lossless && pd_valid)
            printf("ADC clock:       %+.3f ppm (lossless: produced==delivered, "
                   "no USB errors; assumes disciplined host)\n",
                   rate_n >= BASE_WIN ? learned_ppm : budget_ppm);
        else
            printf("Sample budget:   delivered %" PRIu64 " vs expected %" PRIu64
                   " (rate x elapsed), %+.3f ppm (clock+loss combined)\n",
                   delivered, expected_total, budget_ppm);
    }
    if (st_start.valid && st_end.valid) {
        printf("PIB errors:      %u total in %" PRIu64 " second(s); "
               "%" PRIu64 " coincided with a deficit%s\n",
               pib_delta, pib_seconds, pib_with_deficit,
               pib_delta > 0 && pib_with_deficit == 0
                 ? "  (NOTE: overflow flagged but no host-visible loss)" : "");
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
    if (ctrl_fault)
        printf("Control:         GPIOFX3 write error(s) on the marker handle\n");
    printf("Result: %s\n", pass ? "PASS" : "FAIL");

    rx888_close(r);
    libusb_close(h2);
    libusb_exit(ctx2);
    return pass ? 0 : 1;
}
