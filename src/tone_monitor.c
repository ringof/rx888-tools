/*
 * tone_monitor — coherent-tone corruption monitor (the data-QUALITY companion
 * to pps_integrity/stream_soak, which watch data DELIVERY).
 *
 * RIG PRECONDITION (what makes this work): a single GPSDO 27 MHz output is
 * split to BOTH the RX888 Si5351 reference AND the RF input, and the Si5351 is
 * configured with INTEGER multipliers only (PLL x24 -> 648 MHz VCO -> /5 ->
 * 129.6 MHz; no fractional MultiSynth / delta-sigma). The tone is therefore
 * exactly phase-coherent with the sample clock:
 *
 *     f_tone / fs = 27 / 129.6 = 5 / 24   (exact, GPSDO-drift-immune: drift
 *                                          scales numerator and denominator
 *                                          together)
 *
 * so the digitized tone is a DETERMINISTIC period-24 sequence — a built-in test
 * pattern the LTC2208 does not provide on its own.
 *
 * WHAT THIS TOOL DOES (and DOES NOT do): it demodulates the tone INLINE with a
 * single-bin Goertzel at bin 5/24 (1 multiply + 2 adds per sample, no
 * per-sample trig) and EMITS RAW, COLLECTABLE TELEMETRY ONLY. It renders NO
 * verdict. All tracking / slip-vs-garble discrimination / SINAD happens OFFLINE
 * on the saved output, so the analysis can be refined without re-running the
 * rig. This mirrors how the PPS work paid off: --statslog dumped raw rows and a
 * Python tool interpreted (and re-interpreted) them.
 *
 * Two artifacts:
 *   --statslog FILE  per-second CSV: amplitude stats, residual frequency, and
 *                    the worst single-block phase step + its sample index (slip
 *                    evidence survives even when the per-second mean is clean).
 *                    Firmware GETSTATS counters ride along, like the PPS tools.
 *   --iqlog FILE     decimated complex baseband as interleaved float32 (I,Q) at
 *                    rate fs/decim. This IS the demodulated tone (amplitude,
 *                    phase, frequency preserved), so offline you can redo slip /
 *                    garble / SINAD analysis any number of ways with no rerun.
 *
 * A real sample SLIP (drop/dup) shows in the baseband phase as a persistent
 * ~75 deg/sample step; a GARBLE shows as a transient amplitude/phase spike;
 * benign GPSDO/PLL/thermal wander is slow and smooth (tracked out offline, not
 * a discontinuity). The tool just records the phasor; the offline analyzer
 * separates these.
 *
 * SOURCES: by default librx888 owns the bulk stream and a second libusb context
 * does EP0-only GETSTATS reads (like pps_integrity/stream_soak). With
 * --source FILE (or "-" for stdin) the SAME Goertzel/output path is driven from
 * raw int16 samples in a file instead of the device — so a synthesized waveform
 * can validate the DSP with no hardware (see tests/tone_monitor_replay.sh).
 *
 * See doc/tone_monitor_plan.md for the design and doc/pps_integrity.md for the
 * shared instrumentation rationale.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
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

#define PROG_NAME "tone_monitor"

#define CTRL_TIMEOUT_MS  1000

/* Firmware DMA buffer size (CONFIRMED 16 KB). Kept for parity with the other
 * tools; only used to render glDMACount cross-reference. */
#define FW_DMA_BUF_BYTES 16384

/* GETSTATS payload (canonical layout: src/fx3_cmd/fx3_stats.h). Same decode as
 * stream_soak: producer [36..39], consumer-API [40..43], consumer-raw [44..47]
 * socket xferCounts (bytes, wrap ~16 s). */
#define GETSTATS_LEN        48
#define GETSTATS_MIN_LEN    26
#define GETSTATS_DRAIN_PROD 36
#define GETSTATS_DRAIN_CONS 40
#define GETSTATS_DRAIN_RAW  44
#define GETSTATS_DRAIN_END  48

/* Defaults for the rig: 129.6 MSPS, 27 MHz tone, decimate by 2400 (-> 54 kHz
 * baseband, ~1.5 GB/hr iqlog, localizes a slip to ~18 us). decim MUST be a
 * multiple of the period N (= fs/gcd(fs,ftone)) so the Goertzel bin lands on an
 * integer DFT index and each block starts at the same LO phase (otherwise a
 * fake linear phase ramp appears across blocks). */
#define DEFAULT_RATE_MSPS  129.6
#define DEFAULT_FTONE_HZ   27.0e6
#define DEFAULT_DECIM      2400u

/* iqlog ring buffer: SPSC, sample callback produces, iqlog thread consumes ->
 * file. 2^20 records * 8 B = 8 MB. */
#define IQRING_CAP         (1u << 20)
#define IQRING_MASK        (IQRING_CAP - 1u)

/* File-source read chunk (int16 samples). */
#define FILE_CHUNK         (1u << 18)

/* ------------------------------ GETSTATS -------------------------------- */

struct fw_stats {
    uint32_t dma_count;
    uint32_t drain_prod, drain_cons, drain_raw;
    int      drain_valid;
    uint32_t pib_errors;
    uint32_t streaming_faults;
    uint32_t boot_count;
    int      valid;
};

static inline uint32_t fw_backlog(const struct fw_stats *s) {
    return s->drain_prod - s->drain_cons;       /* both wrap together */
}

/* ------------------------------- context -------------------------------- */

typedef struct { float i, q; } iqrec;

struct tone_ctx {
    /* Goertzel state — touched ONLY by the sample callback (single thread). */
    double   coeff;          /* 2*cos(w), w = 2*pi*cyc/N */
    double   cw, sw;         /* cos(w), sin(w) for the final phasor extraction */
    double   s1, s2;         /* recurrence taps */
    uint32_t block_len;      /* decim: samples integrated per baseband sample */
    uint32_t block_pos;      /* samples accumulated into the current block */
    uint64_t sample_index;   /* cumulative input samples seen */
    double   prev_phase_deg; /* last block's phase, for the step */
    int      have_prev;
    double   amp_scale;      /* 2/block_len: Goertzel mag -> tone amplitude (LSB) */

    /* Per-second accumulator — written by the callback (under lock), snapshot
     * and reset once per second. Raw measurements only. */
    pthread_mutex_t acc_lock;
    uint64_t acc_blocks;
    double   acc_amp_sum, acc_amp_min, acc_amp_max;
    double   acc_dstep_sum;  /* net unwrapped phase advance (deg) -> residual freq */
    double   acc_max_step;   /* largest |single-block phase step| (deg) */
    uint64_t acc_worst_sample;
    double   acc_last_phase; /* most recent block phase (deg), for CSV reference */

    /* iqlog ring (only if enabled). */
    int      iqlog_on;
    iqrec   *ring;
    _Atomic uint32_t ring_head;   /* producer */
    _Atomic uint32_t ring_tail;   /* consumer */
    _Atomic uint64_t iq_dropped;  /* records lost to ring-full (file too slow) */
    _Atomic uint64_t iq_written;
};

/* Per-second snapshot of the accumulator (already reduced to reportable form). */
struct acc_snap {
    uint64_t blocks;
    double   amp_mean, amp_min, amp_max;
    double   resid_hz;       /* net phase advance / 360 over the window */
    double   max_step;       /* deg */
    uint64_t worst_sample;
    double   phase_end;      /* deg */
};

/* iqlog header (self-describing; native little-endian). */
struct iqlog_header {
    char     magic[8];       /* "RX888IQ\0" */
    uint32_t version;        /* 1 */
    uint32_t decim;          /* samples per baseband record */
    uint32_t period_n;       /* N = fs/gcd(fs,ftone) */
    uint32_t period_cyc;     /* cycles per N samples */
    double   fs_hz;
    double   ftone_hz;
    double   start_unix;     /* wall-clock at start */
};

struct iqlog_thread_arg {
    struct tone_ctx *ctx;
    FILE            *f;
    _Atomic int      stop;
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ----------------------------- iqlog thread ------------------------------ */

static void *iqlog_writer(void *p)
{
    struct iqlog_thread_arg *a = p;
    struct tone_ctx *c = a->ctx;
    for (;;) {
        uint32_t tail = atomic_load_explicit(&c->ring_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&c->ring_head, memory_order_acquire);
        if (tail == head) {
            if (atomic_load_explicit(&a->stop, memory_order_acquire)) break;
            struct timespec ts = {0, 1000000};   /* 1 ms */
            nanosleep(&ts, NULL);
            continue;
        }
        uint32_t end = (head > tail) ? head : IQRING_CAP;   /* contiguous span */
        uint32_t span = end - tail;
        size_t wrote = fwrite(&c->ring[tail], sizeof(iqrec), span, a->f);
        atomic_fetch_add_explicit(&c->iq_written, wrote, memory_order_relaxed);
        atomic_store_explicit(&c->ring_tail, (tail + (uint32_t)wrote) & IQRING_MASK,
                              memory_order_release);
        if (wrote < span && atomic_load_explicit(&a->stop, memory_order_acquire))
            break;                               /* write error during shutdown */
    }
    fflush(a->f);
    return NULL;
}

static inline void iqring_push(struct tone_ctx *c, float i, float q)
{
    uint32_t head = atomic_load_explicit(&c->ring_head, memory_order_relaxed);
    uint32_t next = (head + 1u) & IQRING_MASK;
    if (next == atomic_load_explicit(&c->ring_tail, memory_order_acquire)) {
        atomic_fetch_add_explicit(&c->iq_dropped, 1, memory_order_relaxed);
        return;                                  /* ring full: drop, count it */
    }
    c->ring[head].i = i;
    c->ring[head].q = q;
    atomic_store_explicit(&c->ring_head, next, memory_order_release);
}

/* ------------------------------- callback -------------------------------- */

static inline double wrap180(double d)
{
    while (d >   180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

/* Goertzel over the buffer; on each completed block emit one baseband phasor.
 * Block stats are batched into call-local vars and merged under one lock at the
 * end (the lock is taken ~once per callback, not per block). Identical whether
 * driven by librx888's writer thread or the --source file loop. */
static void sample_cb(const int16_t *samples, size_t nsamples, void *user)
{
    struct tone_ctx *c = user;

    uint64_t l_blocks = 0;
    double   l_amp_sum = 0.0, l_amp_min = INFINITY, l_amp_max = -INFINITY;
    double   l_dstep_sum = 0.0, l_max_step = 0.0, l_last_phase = c->acc_last_phase;
    uint64_t l_worst = 0;

    double s1 = c->s1, s2 = c->s2;
    const double coeff = c->coeff;
    uint32_t bpos = c->block_pos;

    for (size_t n = 0; n < nsamples; n++) {
        double s0 = (double)samples[n] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
        c->sample_index++;
        if (++bpos < c->block_len)
            continue;

        /* Block complete: extract phasor, reset taps. */
        double re = s1 - s2 * c->cw;
        double im = s2 * c->sw;
        s1 = 0.0; s2 = 0.0; bpos = 0;

        double i_n = re * c->amp_scale;          /* phasor in tone-amplitude LSB */
        double q_n = im * c->amp_scale;
        double amp = hypot(i_n, q_n);
        double phase = atan2(im, re) * (180.0 / M_PI);

        l_blocks++;
        l_amp_sum += amp;
        if (amp < l_amp_min) l_amp_min = amp;
        if (amp > l_amp_max) l_amp_max = amp;
        if (c->have_prev) {
            double step = wrap180(phase - c->prev_phase_deg);
            l_dstep_sum += step;
            double as = fabs(step);
            if (as > l_max_step) { l_max_step = as; l_worst = c->sample_index; }
        } else {
            c->have_prev = 1;
        }
        c->prev_phase_deg = phase;
        l_last_phase = phase;

        if (c->iqlog_on)
            iqring_push(c, (float)i_n, (float)q_n);
    }

    c->s1 = s1; c->s2 = s2; c->block_pos = bpos;

    if (l_blocks == 0)
        return;

    pthread_mutex_lock(&c->acc_lock);
    c->acc_blocks += l_blocks;
    c->acc_amp_sum += l_amp_sum;
    if (l_amp_min < c->acc_amp_min) c->acc_amp_min = l_amp_min;
    if (l_amp_max > c->acc_amp_max) c->acc_amp_max = l_amp_max;
    c->acc_dstep_sum += l_dstep_sum;
    if (l_max_step > c->acc_max_step) {
        c->acc_max_step = l_max_step;
        c->acc_worst_sample = l_worst;
    }
    c->acc_last_phase = l_last_phase;
    pthread_mutex_unlock(&c->acc_lock);
}

/* Lock, copy, reset; reduce to reportable form. */
static void snapshot_accum(struct tone_ctx *c, struct acc_snap *o)
{
    pthread_mutex_lock(&c->acc_lock);
    uint64_t blocks = c->acc_blocks;
    double amp_sum = c->acc_amp_sum, amp_min = c->acc_amp_min, amp_max = c->acc_amp_max;
    double dstep = c->acc_dstep_sum, max_step = c->acc_max_step;
    uint64_t worst = c->acc_worst_sample;
    double ph_end = c->acc_last_phase;
    c->acc_blocks = 0; c->acc_amp_sum = 0.0;
    c->acc_amp_min = INFINITY; c->acc_amp_max = -INFINITY;
    c->acc_dstep_sum = 0.0; c->acc_max_step = 0.0; c->acc_worst_sample = 0;
    pthread_mutex_unlock(&c->acc_lock);

    o->blocks   = blocks;
    o->amp_mean = blocks ? amp_sum / (double)blocks : 0.0;
    o->amp_min  = blocks ? amp_min : 0.0;
    o->amp_max  = blocks ? amp_max : 0.0;
    o->resid_hz = dstep / 360.0;     /* net cycles over the ~1 s window */
    o->max_step = max_step;
    o->worst_sample = worst;
    o->phase_end = ph_end;
}

/* One CSV row. fw/lib may be zeroed (file source has no device counters). */
static void write_stats_row(FILE *f, const char *ts, uint64_t sec,
                            const struct acc_snap *s,
                            const struct fw_stats *fw, const rx888_stats_t *lib)
{
    fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%.6f,%.4f,%" PRIu64
            ",%.4f,%u,%d,%u,%u,%u,%u,%llu,%llu,%llu,%u,%u,%u\n",
            ts, sec, s->blocks, s->amp_mean, s->amp_min, s->amp_max, s->resid_hz,
            s->max_step, s->worst_sample, s->phase_end,
            fw->dma_count, fw->drain_valid, fw->drain_prod, fw->drain_cons,
            fw->drain_raw, fw_backlog(fw),
            lib->ok_xfers, lib->bad_xfers, lib->zero_xfers,
            fw->pib_errors, fw->streaming_faults, fw->boot_count);
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
    if (r >= GETSTATS_DRAIN_END) {
        memcpy(&s->drain_prod, &buf[GETSTATS_DRAIN_PROD], 4);
        memcpy(&s->drain_cons, &buf[GETSTATS_DRAIN_CONS], 4);
        memcpy(&s->drain_raw,  &buf[GETSTATS_DRAIN_RAW],  4);
        s->drain_valid = 1;
    }
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

/* Synthetic HH:MM:SS for the file source (no wall clock relevance). */
static void fmt_elapsed(char *out, size_t n, uint64_t sec)
{
    snprintf(out, n, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".000000",
             sec / 3600, (sec % 3600) / 60, sec % 60);
}

static unsigned long gcd_ul(unsigned long a, unsigned long b)
{
    while (b) { unsigned long t = a % b; a = b; b = t; }
    return a;
}

/* ----------------------------- iqlog setup ------------------------------- */

static int setup_iqlog(struct tone_ctx *ctx, const char *path,
                       unsigned samplerate, unsigned decim,
                       unsigned long N, unsigned long cyc, double ftone_hz,
                       FILE **iqf_out, pthread_t *tid, int *tid_ok,
                       struct iqlog_thread_arg *arg)
{
    FILE *iqf = fopen(path, "wb");
    if (!iqf) {
        fprintf(stderr, "%s: cannot open iqlog '%s': %s\n",
                PROG_NAME, path, strerror(errno));
        return -1;
    }
    ctx->ring = malloc((size_t)IQRING_CAP * sizeof(iqrec));
    if (!ctx->ring) {
        fprintf(stderr, "%s: out of memory for iqlog ring\n", PROG_NAME);
        fclose(iqf);
        return -1;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct iqlog_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "RX888IQ", 8);
    hdr.version    = 1;
    hdr.decim      = decim;
    hdr.period_n   = (uint32_t)N;
    hdr.period_cyc = (uint32_t)cyc;
    hdr.fs_hz      = (double)samplerate;
    hdr.ftone_hz   = ftone_hz;
    hdr.start_unix = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
    fwrite(&hdr, sizeof(hdr), 1, iqf);

    arg->ctx = ctx;
    arg->f   = iqf;
    atomic_init(&arg->stop, 0);
    ctx->iqlog_on = 1;
    if (pthread_create(tid, NULL, iqlog_writer, arg) != 0) {
        fprintf(stderr, "%s: cannot start iqlog thread\n", PROG_NAME);
        free(ctx->ring); ctx->ring = NULL; ctx->iqlog_on = 0;
        fclose(iqf);
        return -1;
    }
    *tid_ok = 1;
    *iqf_out = iqf;
    return 0;
}

static void teardown_iqlog(struct tone_ctx *ctx, FILE *iqf, pthread_t tid,
                           int tid_ok, struct iqlog_thread_arg *arg)
{
    if (tid_ok) {
        atomic_store(&arg->stop, 1);
        pthread_join(tid, NULL);
    }
    if (iqf) fclose(iqf);
    free(ctx->ring);
    ctx->ring = NULL;
}

/* ------------------------------ statslog --------------------------------- */

static FILE *open_statslog(const char *path, unsigned samplerate, double ftone_hz,
                           unsigned long N, unsigned long cyc, unsigned decim,
                           double bb_rate)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "%s: cannot open statslog '%s': %s\n",
                PROG_NAME, path, strerror(errno));
        return NULL;
    }
    fprintf(f, "# fs=%u ftone=%.0f N=%lu cyc=%lu decim=%u bb_rate=%.3f "
            "deg_per_slip=%.4f\n",
            samplerate, ftone_hz, N, cyc, decim, bb_rate,
            360.0 * (double)cyc / (double)N);
    fprintf(f, "time,sec,blocks,amp_mean,amp_min,amp_max,resid_freq_hz,"
            "max_step_deg,worst_step_sample,phase_end_deg,dma_count,drain_valid,"
            "drain_prod,drain_cons,drain_raw,backlog,ok_xfers,bad_xfers,"
            "zero_xfers,pib,faults,boot\n");
    return f;
}

static void summary_common(const struct tone_ctx *ctx, const char *iqlog_path)
{
    if (iqlog_path) {
        unsigned long long w_iq = atomic_load(&ctx->iq_written);
        unsigned long long d_iq = atomic_load(&ctx->iq_dropped);
        printf("iqlog:           %llu records written, %llu dropped (ring-full)\n",
               w_iq, d_iq);
        if (d_iq)
            printf("                 NOTE: dropped baseband records — sink could "
                   "not keep up; the iqlog has gaps.\n");
    }
    printf("Analyze:         feed the iqlog/statslog to tests/tone_quality.py\n");
}

/* ------------------------------ file driver ------------------------------ */

static int run_from_file(struct tone_ctx *ctx, const char *source_path,
                         unsigned samplerate, double rate_msps, double ftone_hz,
                         unsigned decim, unsigned long N, unsigned long cyc,
                         double bb_rate, FILE *statslog, const char *iqlog_path,
                         int verbose)
{
    FILE *src = (strcmp(source_path, "-") == 0) ? stdin : fopen(source_path, "rb");
    if (!src) {
        fprintf(stderr, "%s: cannot open source '%s': %s\n",
                PROG_NAME, source_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "%s: replaying %s @ %g MSPS, tone %g MHz, decim %u "
            "(a 'second' = %u samples)\n", PROG_NAME,
            (src == stdin) ? "<stdin>" : source_path,
            rate_msps, ftone_hz / 1e6, decim, samplerate);

    printf("#time             sec   blocks   amp_mean  amp_min  amp_max%s\n",
           verbose ? "   residHz   maxstep" : "");
    fflush(stdout);

    int16_t *buf = malloc((size_t)FILE_CHUNK * sizeof(int16_t));
    if (!buf) {
        fprintf(stderr, "%s: OOM\n", PROG_NAME);
        if (src != stdin) fclose(src);
        return 1;
    }

    struct fw_stats fw0 = {0};            /* no device counters in file mode */
    rx888_stats_t   lib0 = {0};
    uint64_t sec = 0, next_boundary = samplerate;
    uint64_t total_samples = 0;

    while (!g_stop) {
        size_t got = fread(buf, sizeof(int16_t), FILE_CHUNK, src);
        if (got == 0) break;
        sample_cb(buf, got, ctx);
        total_samples += got;

        while (total_samples >= next_boundary) {       /* crossed a "second" */
            struct acc_snap s;
            snapshot_accum(ctx, &s);
            char ts[32]; fmt_elapsed(ts, sizeof ts, sec);
            printf(" %-16s %5" PRIu64 " %8" PRIu64 " %9.1f %8.1f %8.1f",
                   ts, sec + 1, s.blocks, s.amp_mean, s.amp_min, s.amp_max);
            if (verbose) printf(" %9.3f %9.2f", s.resid_hz, s.max_step);
            printf("\n");
            if (statslog) write_stats_row(statslog, ts, sec, &s, &fw0, &lib0);
            sec++;
            next_boundary += samplerate;
        }
    }

    /* Final partial window. */
    struct acc_snap s;
    snapshot_accum(ctx, &s);
    if (s.blocks) {
        char ts[32]; fmt_elapsed(ts, sizeof ts, sec);
        printf(" %-16s %5" PRIu64 " %8" PRIu64 " %9.1f %8.1f %8.1f",
               ts, sec + 1, s.blocks, s.amp_mean, s.amp_min, s.amp_max);
        if (verbose) printf(" %9.3f %9.2f", s.resid_hz, s.max_step);
        printf("\n");
        if (statslog) write_stats_row(statslog, ts, sec, &s, &fw0, &lib0);
    }
    fflush(stdout);
    free(buf);
    if (src != stdin) fclose(src);

    if (statslog) {
        char tend[32]; fmt_elapsed(tend, sizeof tend, sec);
        fprintf(statslog, "# end %s samples=%" PRIu64 " iq_written=%llu "
                "iq_dropped=%llu\n", tend, total_samples,
                (unsigned long long)atomic_load(&ctx->iq_written),
                (unsigned long long)atomic_load(&ctx->iq_dropped));
    }

    printf("\n=== TONE MONITOR SUMMARY (file replay, raw measurements) ===\n");
    printf("Source:          %s\n", (src == stdin) ? "<stdin>" : source_path);
    printf("Sample rate:     %g MSPS (%u Hz, assumed)\n", rate_msps, samplerate);
    printf("Tone / period:   %g MHz, N=%lu samples (%lu cycles), %.4f deg/slip\n",
           ftone_hz / 1e6, N, cyc, 360.0 * (double)cyc / (double)N);
    printf("Decimation:      %u -> %.3f kHz baseband\n", decim, bb_rate / 1e3);
    printf("Samples read:    %" PRIu64 " (%.3f s of data)\n",
           total_samples, (double)total_samples / (double)samplerate);
    summary_common(ctx, iqlog_path);
    return 0;
}

/* ----------------------------- device driver ----------------------------- */

static int run_from_device(struct tone_ctx *ctx, double hours,
                           unsigned samplerate, double rate_msps, double ftone_hz,
                           unsigned decim, unsigned long N, unsigned long cyc,
                           double bb_rate, const char *firmware_path,
                           unsigned queuedepth, unsigned reqsize,
                           int gain, int att, int gain_high,
                           FILE *statslog, const char *iqlog_path, int verbose)
{
    rx888_config_t cfg;
    rx888_config_init_default(&cfg);
    cfg.samplerate    = samplerate;
    cfg.firmware_path = firmware_path;
    if (queuedepth) cfg.queue_depth = queuedepth;
    if (reqsize)    cfg.req_packets = reqsize;
    if (gain >= 0)      cfg.gain      = (unsigned)gain;   /* AD8370 VGA code */
    if (att  >= 0)      cfg.att       = (unsigned)att;    /* DAT-31 attenuator */
    if (gain_high >= 0) cfg.gain_high = gain_high;        /* VGA range low/high */

    rx888_t *r = NULL;
    int rc = rx888_open(&r, &cfg);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_open: %s\n", PROG_NAME, rx888_strerror(rc));
        return 1;
    }
    rc = rx888_start(r, sample_cb, ctx);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_start: %s\n", PROG_NAME, rx888_strerror(rc));
        rx888_close(r);
        return 1;
    }

    libusb_context *ctx2 = NULL;
    libusb_device_handle *h2 = open_ctrl_handle(&ctx2);  /* best-effort */
    struct fw_stats st_start = {0}, st_end = {0};
    if (h2) read_fw_stats(h2, &st_start);

    fprintf(stderr, "%s: %.3f h @ %g MSPS, tone %g MHz, period N=%lu (%lu cyc), "
            "decim %u -> %.1f kHz baseband (slip step ~%.1f deg/sample)\n",
            PROG_NAME, hours, rate_msps, ftone_hz / 1e6, N, cyc, decim,
            bb_rate / 1e3, 360.0 * (double)cyc / (double)N);
    if (iqlog_path)
        fprintf(stderr, "%s: iqlog (float32 I,Q @ %.1f kHz = %.2f MB/hr)\n",
                PROG_NAME, bb_rate / 1e3, bb_rate * sizeof(iqrec) * 3600.0 / 1e6);

    printf("#time             sec   blocks   amp_mean  amp_min  amp_max%s\n",
           verbose ? "   residHz   maxstep" : "");
    fflush(stdout);

    uint32_t pib_prev = st_start.valid ? st_start.pib_errors : 0;
    int mid_reset = 0, internal_stop = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t total_secs = (uint64_t)(hours * 3600.0 + 0.5);

    for (uint64_t sec = 0; !g_stop; sec++) {
        if (total_secs && sec >= total_secs) break;
        if (!rx888_is_running(r)) { internal_stop = 1; break; }

        struct timespec wake = t0;
        wake.tv_sec += (time_t)(sec + 1);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL) == EINTR
               && !g_stop)
            ;
        if (g_stop) break;

        char ts[48];
        fmt_wallclock(ts, sizeof(ts));

        struct acc_snap s;
        snapshot_accum(ctx, &s);

        struct fw_stats cur = {0};
        char note[64]; note[0] = '\0';
        if (h2 && read_fw_stats(h2, &cur) == 0) {
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

        printf(" %-16s %5" PRIu64 " %8" PRIu64 " %9.1f %8.1f %8.1f",
               ts, sec + 1, s.blocks, s.amp_mean, s.amp_min, s.amp_max);
        if (verbose) printf(" %9.3f %9.2f", s.resid_hz, s.max_step);
        if (note[0]) printf("%s", note);
        printf("\n");
        fflush(stdout);

        if (statslog) {
            rx888_stats_t lib_now;
            rx888_get_stats(r, &lib_now);
            write_stats_row(statslog, ts, sec, &s, &cur, &lib_now);
        }
    }

    /* End FW counters BEFORE rx888_stop() (STOPFX3 resets glDMACount). */
    if (h2) read_fw_stats(h2, &st_end);
    rx888_stats_t lib;
    rx888_get_stats(r, &lib);

    if (statslog) {
        char tend[48]; fmt_wallclock(tend, sizeof tend);
        fprintf(statslog, "# end %s ok=%llu bad=%llu zero=%llu iq_written=%llu "
                "iq_dropped=%llu\n", tend, lib.ok_xfers, lib.bad_xfers,
                lib.zero_xfers,
                (unsigned long long)atomic_load(&ctx->iq_written),
                (unsigned long long)atomic_load(&ctx->iq_dropped));
    }

    rx888_stop(r);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_d = (double)(t1.tv_sec - t0.tv_sec)
                     + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    uint64_t elapsed = (uint64_t)elapsed_d;
    int boot_changed = (st_start.valid && st_end.valid &&
                        st_start.boot_count != st_end.boot_count);

    printf("\n=== TONE MONITOR SUMMARY (raw measurements, no verdict) ===\n");
    printf("Duration:        %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 "\n",
           elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    printf("Sample rate:     %g MSPS (%u Hz)\n", rate_msps, samplerate);
    printf("Tone / period:   %g MHz, N=%lu samples (%lu cycles), %.4f deg/slip\n",
           ftone_hz / 1e6, N, cyc, 360.0 * (double)cyc / (double)N);
    printf("Decimation:      %u -> %.3f kHz baseband\n", decim, bb_rate / 1e3);
    printf("USB transfers:   ok=%llu bad=%llu zero-length=%llu\n",
           lib.ok_xfers, lib.bad_xfers, lib.zero_xfers);
    printf("Xfer status:     ERROR=%llu TIMED_OUT=%llu CANCELLED=%llu STALL=%llu "
           "NO_DEVICE=%llu OVERFLOW=%llu\n",
           lib.status_counts[1], lib.status_counts[2], lib.status_counts[3],
           lib.status_counts[4], lib.status_counts[5], lib.status_counts[6]);
    if (st_start.valid && st_end.valid) {
        printf("PIB errors:      %u\n", st_end.pib_errors - st_start.pib_errors);
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
    summary_common(ctx, iqlog_path);

    rx888_close(r);
    if (h2) { libusb_close(h2); libusb_exit(ctx2); }
    return 0;
}

/* --------------------------------- usage --------------------------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [hours] [--rate MSPS] [--ftone HZ] [--decim N]\n"
        "          [--statslog FILE] [--iqlog FILE] [--source FILE]\n"
        "          [--firmware FILE] [-q N] [-p N] [-v]\n"
        "  hours              Run duration in hours (default 4; 0 = until ^C)\n"
        "  --rate MSPS        ADC sample rate in MSPS (default %g)\n"
        "  --ftone HZ         Coherent tone frequency (default %g)\n"
        "  --decim N          Goertzel block length / decimation (default %u);\n"
        "                     MUST be a multiple of the period N=fs/gcd(fs,ftone)\n"
        "  --statslog FILE    Per-second raw CSV (amplitude, residual freq,\n"
        "                     worst phase step + sample, FW counters)\n"
        "  --iqlog FILE       Decimated complex baseband, float32 (I,Q) @ fs/decim\n"
        "  --source FILE      Read raw int16 samples from FILE ('-' = stdin)\n"
        "                     instead of the device — drives the SAME DSP path,\n"
        "                     for validating with a synthesized waveform\n"
        "  -f, --firmware FILE  Upload FX3 firmware if device is in boot mode\n"
        "  -g, --gain N         AD8370 VGA gain code 0..127 (library default 0)\n"
        "  -a, --att N          DAT-31 attenuator 0..63, half-dB steps (default 0)\n"
        "  --gainmode low|high  AD8370 gain range (library default high)\n"
        "  -q, --queuedepth N   Concurrent in-flight USB transfers (default 32)\n"
        "  -p, --reqsize N      USB transfer size in packets (default 1024)\n"
        "  -v, --verbose        Add residual-freq / max-step to the live line\n"
        "  -h, --help           Show this help\n"
        "\n"
        "Coherent-tone data-QUALITY monitor. Demodulates the tone inline and\n"
        "emits raw telemetry only (no verdict); slip/garble/SINAD analysis is\n"
        "done OFFLINE on the saved output. See doc/tone_monitor_plan.md.\n",
        argv0, DEFAULT_RATE_MSPS, DEFAULT_FTONE_HZ, DEFAULT_DECIM);
}

/* --------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    double hours = 4.0;
    double rate_msps = DEFAULT_RATE_MSPS;
    double ftone_hz = DEFAULT_FTONE_HZ;
    unsigned decim = DEFAULT_DECIM;
    const char *firmware_path = NULL;
    const char *statslog_path = NULL;
    const char *iqlog_path = NULL;
    const char *source_path = NULL;
    int verbose = 0;
    unsigned queuedepth = 0, reqsize = 0;
    int gain = -1, att = -1, gain_high = -1;   /* -1 = leave library default */

    enum { OPT_FTONE = 256, OPT_DECIM, OPT_IQLOG, OPT_SOURCE, OPT_GAINMODE };
    static struct option opts[] = {
        {"rate",       required_argument, 0, 'r'},
        {"ftone",      required_argument, 0, OPT_FTONE},
        {"decim",      required_argument, 0, OPT_DECIM},
        {"statslog",   required_argument, 0, 'l'},
        {"iqlog",      required_argument, 0, OPT_IQLOG},
        {"source",     required_argument, 0, OPT_SOURCE},
        {"gain",       required_argument, 0, 'g'},
        {"att",        required_argument, 0, 'a'},
        {"gainmode",   required_argument, 0, OPT_GAINMODE},
        {"firmware",   required_argument, 0, 'f'},
        {"queuedepth", required_argument, 0, 'q'},
        {"reqsize",    required_argument, 0, 'p'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:f:q:p:l:g:a:vh", opts, NULL)) != -1) {
        char *end = NULL;
        switch (c) {
        case 'r':
            rate_msps = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || rate_msps <= 0) {
                fprintf(stderr, "%s: bad --rate '%s'\n", PROG_NAME, optarg);
                return 2;
            }
            break;
        case OPT_FTONE:
            ftone_hz = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || ftone_hz <= 0) {
                fprintf(stderr, "%s: bad --ftone '%s'\n", PROG_NAME, optarg);
                return 2;
            }
            break;
        case OPT_DECIM:
            decim = (unsigned)strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || decim == 0) {
                fprintf(stderr, "%s: bad --decim '%s'\n", PROG_NAME, optarg);
                return 2;
            }
            break;
        case 'l': statslog_path = optarg; break;
        case OPT_IQLOG: iqlog_path = optarg; break;
        case OPT_SOURCE: source_path = optarg; break;
        case 'g': gain = (int)(strtoul(optarg, NULL, 10) & 0x7fu); break;
        case 'a': att  = (int)(strtoul(optarg, NULL, 10) & 0x3fu); break;
        case OPT_GAINMODE:
            if      (!strcmp(optarg, "low"))  gain_high = 0;
            else if (!strcmp(optarg, "high")) gain_high = 1;
            else { fprintf(stderr, "%s: bad --gainmode '%s' (low|high)\n",
                           PROG_NAME, optarg); return 2; }
            break;
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
        if (end == argv[optind] || *end != '\0' || hours < 0) {
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

    /* Period of the coherent tone: N samples, cyc cycles. decim must be a
     * multiple of N or each block starts at a different LO phase (fake ramp). */
    unsigned long g = gcd_ul(samplerate, (unsigned long)(ftone_hz + 0.5));
    unsigned long N = samplerate / g;
    unsigned long cyc = (unsigned long)(ftone_hz + 0.5) / g;
    if (decim % N != 0) {
        fprintf(stderr, "%s: --decim %u must be a multiple of the period N=%lu "
                "(fs/gcd) for an exact Goertzel bin; try %lu\n",
                PROG_NAME, decim, N, ((decim + N - 1) / N) * N);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ---- context + Goertzel constants (source-independent) ---- */
    struct tone_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    double w = 2.0 * M_PI * (double)cyc / (double)N;
    ctx.coeff     = 2.0 * cos(w);
    ctx.cw        = cos(w);
    ctx.sw        = sin(w);
    ctx.block_len = decim;
    ctx.amp_scale = 2.0 / (double)decim;
    ctx.acc_amp_min = INFINITY;
    ctx.acc_amp_max = -INFINITY;
    pthread_mutex_init(&ctx.acc_lock, NULL);

    double bb_rate = (double)samplerate / (double)decim;

    /* ---- iqlog (source-independent) ---- */
    FILE *iqf = NULL;
    pthread_t iqtid;
    int iqtid_ok = 0;
    struct iqlog_thread_arg iqarg;
    memset(&iqarg, 0, sizeof(iqarg));
    if (iqlog_path &&
        setup_iqlog(&ctx, iqlog_path, samplerate, decim, N, cyc, ftone_hz,
                    &iqf, &iqtid, &iqtid_ok, &iqarg) != 0) {
        pthread_mutex_destroy(&ctx.acc_lock);
        return 1;
    }

    /* ---- statslog (source-independent) ---- */
    FILE *statslog = NULL;
    if (statslog_path) {
        statslog = open_statslog(statslog_path, samplerate, ftone_hz, N, cyc,
                                 decim, bb_rate);
        if (!statslog) {
            teardown_iqlog(&ctx, iqf, iqtid, iqtid_ok, &iqarg);
            pthread_mutex_destroy(&ctx.acc_lock);
            return 1;
        }
    }

    int rc;
    if (source_path)
        rc = run_from_file(&ctx, source_path, samplerate, rate_msps, ftone_hz,
                           decim, N, cyc, bb_rate, statslog, iqlog_path, verbose);
    else
        rc = run_from_device(&ctx, hours, samplerate, rate_msps, ftone_hz, decim,
                             N, cyc, bb_rate, firmware_path, queuedepth, reqsize,
                             gain, att, gain_high,
                             statslog, iqlog_path, verbose);

    if (statslog) fclose(statslog);
    teardown_iqlog(&ctx, iqf, iqtid, iqtid_ok, &iqarg);
    pthread_mutex_destroy(&ctx.acc_lock);
    return rc;
}
