/*
RX888 Streamer (robust rewrite)
--------------------------------
This is a "maximalist" safety/robustness rewrite of the original rx888_stream.c.

Goals:
- Never segfault on missing device / failed libusb calls / unexpected descriptors
- Cleanly handle broken pipes, CTRL+C, and transfer errors
- Defensive USB descriptor parsing (find Bulk IN endpoint; handle USB2/USB3)
- Centralized cleanup path; no uninitialized frees
- Better diagnostics and compile-time friendliness

Copyright (c) 2021 Ruslan Migirov <trapi78@gmail.com>
Copyright (c) 2024 David Goncalves <dave@w1euj.com>
Copyright (c) 2026 (rewrite) OpenAI assistant

License: MIT (same as original)
*/

#define _GNU_SOURCE

#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  else
#    include <libusb.h>
#  endif
#else
#  include <libusb.h>
#endif


#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ezusb.h"
#include "rx888.h"

/* ezusb.c expects this symbol */
int verbose = 0;

/* -------------------------- Defaults / Globals -------------------------- */

static const uint16_t RX888_VID = 0x04b4;
static const uint16_t RX888_PID_BOOT = 0x00f3; /* needs firmware */
static const uint16_t RX888_PID_APP  = 0x00f1; /* normal streaming */

static _Atomic int g_stop = 0;

static int g_verbose = 0;
static int g_dither = 0;
static int g_randomizer = 0;

/* RX888 analog + clock configuration (restored from original) */
static unsigned int g_samplerate = 32000000;
static unsigned int g_gain = 0x80; /* bit7=high/low, bits[6:0]=0..127 */
static unsigned int g_att = 0;     /* 0..63 */
static int g_fixup_samples = 0;    /* original bit-fixup in callback */

/* Reference crystal for reporting actual frequency (default 27 MHz) */
static uint32_t g_xtal_hz = 27000000U;

static const char *g_firmware_path = NULL;

/* queue depth and request size in packets (exported via rx888_stream.h) */
unsigned int g_queue_depth = 32; /* default: 32 in-flight USB transfers */
unsigned int g_req_packets = 1024; /* default: ~1 MiB per transfer on USB3 (1024B packets) */

/* USB timeouts (ms) */
static unsigned int g_ctrl_timeout_ms = 5000;
unsigned int g_stream_timeout_ms = 0; /* 0 = infinite per libusb */

/* Settling delay (µs) between FX3 control transfers.
   The FX3 firmware needs time to process each vendor command before
   accepting the next.  5 ms is empirically reliable. */
#define CTRL_SETTLE_US 5000

/* No-data watchdog: if no USB callbacks arrive for this long (ms) while
   transfers are in-flight, assume the FX3/USB link is wedged and bail out.
   0 = disabled. */
static unsigned int g_watchdog_ms = 3000;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ----------------------------- Small helpers ---------------------------- */

static const char *xfer_status_str(enum libusb_transfer_status st) {
    switch (st) {
        case LIBUSB_TRANSFER_COMPLETED: return "COMPLETED";
        case LIBUSB_TRANSFER_ERROR: return "ERROR";
        case LIBUSB_TRANSFER_TIMED_OUT: return "TIMED_OUT";
        case LIBUSB_TRANSFER_CANCELLED: return "CANCELLED";
        case LIBUSB_TRANSFER_STALL: return "STALL";
        case LIBUSB_TRANSFER_NO_DEVICE: return "NO_DEVICE";
        case LIBUSB_TRANSFER_OVERFLOW: return "OVERFLOW";
        default: return "UNKNOWN";
    }
}


/* On USB3, wMaxPacketSize is the max packet *within* a burst; the endpoint
   companion descriptor may specify additional bursts. Return an "effective"
   max payload per service interval for sizing transfers. If unavailable,
   fall back to wMaxPacketSize. */
static unsigned int effective_max_packet(libusb_device *dev,
                                        const struct libusb_endpoint_descriptor *ep) {
    if (!dev || !ep) return 0;
    unsigned int base = ep->wMaxPacketSize;
#if defined(LIBUSB_SPEED_SUPER)
    int speed = libusb_get_device_speed(dev);
    if (speed == LIBUSB_SPEED_SUPER || speed == LIBUSB_SPEED_SUPER_PLUS) {
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000106)
        struct libusb_ss_endpoint_companion_descriptor *comp = NULL;
        if (libusb_get_ss_endpoint_companion_descriptor(NULL, ep, &comp) == 0 && comp) {
            unsigned int bursts = (unsigned int)comp->bMaxBurst + 1U;
            libusb_free_ss_endpoint_companion_descriptor(comp);
            /* Guard against absurd values */
            if (bursts > 0 && bursts < 256U) return base * bursts;
        }
#endif
    }
#endif
    return base;
}

static void vlogf(int level, const char *fmt, ...) {
    if (g_verbose < level) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int write_full(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) return -EIO;
        off += (size_t)n;
    }
    return 0;
}


/* --------------------- Transfer handoff queue (no-copy) ---------------------
   Goal: Never block in the libusb callback.
   Pattern:
     - libusb callback: enqueue completed transfer pointer, return immediately.
     - writer thread: dequeue, optional sample fixup, write() to stdout, then
                      resubmit the SAME transfer to libusb.

   This prevents stdout backpressure / pipe stalls from wedging libusb event
   handling and reduces jitter even under desktop load.
---------------------------------------------------------------------------- */
typedef struct xfer_queue {
    struct libusb_transfer **ring;
    size_t cap, head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t  cv_nonempty;
    bool shutdown;
} xfer_queue_t;

static int xq_init(struct xfer_queue *q, size_t cap)
{
    memset(q, 0, sizeof(*q));
    q->ring = calloc(cap, sizeof(*q->ring));
    if (!q->ring) return -1;

    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->ring);
        memset(q, 0, sizeof(*q));
        return -1;
    }

    if (pthread_cond_init(&q->cv_nonempty, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q->ring);
        memset(q, 0, sizeof(*q));
        return -1;
    }

    q->cap = cap;
    return 0;
}

static void xq_shutdown(xfer_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->shutdown = true;
    pthread_cond_broadcast(&q->cv_nonempty);
    pthread_mutex_unlock(&q->mu);
}

static void xq_destroy(xfer_queue_t *q) {
    if (!q) return;
    free(q->ring);
    q->ring = NULL;
    pthread_cond_destroy(&q->cv_nonempty);
    pthread_mutex_destroy(&q->mu);
}

static bool xq_try_push(xfer_queue_t *q, struct libusb_transfer *t) {
    bool ok = false;
    pthread_mutex_lock(&q->mu);
    if (!q->shutdown && q->count < q->cap) {
        q->ring[q->tail] = t;
        q->tail = (q->tail + 1) % q->cap;
        q->count++;
        ok = true;
        pthread_cond_signal(&q->cv_nonempty);
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

static struct libusb_transfer *xq_pop_blocking(xfer_queue_t *q) {
    struct libusb_transfer *t = NULL;
    pthread_mutex_lock(&q->mu);
    while (!q->shutdown && q->count == 0) {
        pthread_cond_wait(&q->cv_nonempty, &q->mu);
    }
    if (q->count > 0) {
        t = q->ring[q->head];
        q->ring[q->head] = NULL;
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return t; /* NULL when shutdown and empty */
}


static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* Determine actual (vs requested) clock frequency for logging.
   Adapted from Franco Venturi, K4VZ (as in original code). */
static double actual_freq(double frequency_hz) {
    /* Guard against degenerate inputs:
       - <= 0 would cause the doubling loop to run forever
       - > 900 MHz would overflow the uint32 PLL arithmetic */
    if (frequency_hz <= 0.0 || frequency_hz > 900000000.0)
        return frequency_hz;

    while (frequency_hz < 1000000.0) frequency_hz *= 2.0;

    uint32_t divider = 900000000UL / (uint32_t)frequency_hz;
    if (divider % 2U) divider--;
    if (divider == 0) divider = 2;

    uint64_t pllFreq64 = (uint64_t)divider * (uint64_t)(uint32_t)frequency_hz;
    if (pllFreq64 > UINT32_MAX) return frequency_hz;
    uint32_t pllFreq = (uint32_t)pllFreq64;

    uint8_t mult = (uint8_t)(pllFreq / g_xtal_hz);
    uint32_t l = pllFreq % g_xtal_hz;

    double f = (double)l;
    f *= 1048575.0;
    f /= (double)g_xtal_hz;
    uint32_t num = (uint32_t)f;
    uint32_t denom = 1048575U;

    double actualPll = (double)g_xtal_hz * ((double)mult + (double)num / (double)denom);
    double actualAdc = actualPll / (double)divider;
    return actualAdc;
}

/* ------------------------ RX888 control transfers ----------------------- */

/* The original code uses vendor control transfers (type/vendor/device).
   Keep this stable unless you know the firmware protocol changed. */

static int ctrl_write_u32(libusb_device_handle *h, uint8_t request, uint16_t wValue,
                          uint16_t wIndex, uint32_t value_le) {
    /* value_le is sent as 4 bytes, little-endian */
    uint8_t data[4];
    data[0] = (uint8_t)(value_le & 0xffu);
    data[1] = (uint8_t)((value_le >> 8) & 0xffu);
    data[2] = (uint8_t)((value_le >> 16) & 0xffu);
    data[3] = (uint8_t)((value_le >> 24) & 0xffu);

    int r = libusb_control_transfer(
        h,
        (uint8_t)(LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE),
        request,
        wValue,
        wIndex,
        data,
        (uint16_t)sizeof(data),
        g_ctrl_timeout_ms
    );
    if (r < 0) return r;
    if (r != (int)sizeof(data)) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_write_buf(libusb_device_handle *h, uint8_t request, uint16_t wValue,
                          uint16_t wIndex, const uint8_t *buf, uint16_t len) {
    int r = libusb_control_transfer(
        h,
        (uint8_t)(LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE),
        request,
        wValue,
        wIndex,
        (unsigned char *)buf,
        len,
        g_ctrl_timeout_ms
    );
    if (r < 0) return r;
    if (r != (int)len) return LIBUSB_ERROR_IO;
    return 0;
}

/* Convenience: SETARGFX3 index=arg_id value=arg_val, optionally with extra bytes (none here) */
static int rx888_set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val) {
    /* Match existing ezusb.c behavior: SETARG expects a 1-byte dummy payload */
    uint8_t zero = 0;
    return ctrl_write_buf(h, (uint8_t)SETARGFX3, arg_val, arg_id, &zero, 1);
}

static int rx888_gpio(libusb_device_handle *h, uint32_t gpio_bits) {
    return ctrl_write_u32(h, (uint8_t)GPIOFX3, 0, 0, gpio_bits);
}

static int rx888_cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val) {
    return ctrl_write_u32(h, cmd, 0, 0, val);
}

static int rx888_start_stream(libusb_device_handle *h) {
    /* Original code sent 0 as payload; keep that behavior. */
    return rx888_cmd_u32(h, (uint8_t)STARTFX3, 0u);
}

static int rx888_stop_stream(libusb_device_handle *h) {
    return rx888_cmd_u32(h, (uint8_t)STOPFX3, 0u);
}

/* -------------------------- USB device discovery ------------------------ */

struct dev_match {
    libusb_device *dev;
    struct libusb_device_descriptor desc;
};

static bool is_rx888_desc(const struct libusb_device_descriptor *d, uint16_t pid) {
    return d && d->idVendor == RX888_VID && d->idProduct == pid;
}

/* Find first device with VID/PID. Returns 0 and sets out_dev on success. */
static int find_device(libusb_context *ctx, uint16_t pid, struct dev_match *out) {
    if (!out) return LIBUSB_ERROR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) return (int)n;

    int rc = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        int r = libusb_get_device_descriptor(list[i], &d);
        if (r != 0) continue;
        if (is_rx888_desc(&d, pid)) {
            out->dev = list[i];
            out->desc = d;
            libusb_ref_device(out->dev); /* keep after freeing list */
            rc = 0;
            break;
        }
    }

    libusb_free_device_list(list, 1);
    return rc;
}

/* Open RX888 in app mode; if not found and firmware path provided, try bootloader upload then retry. */
static int open_rx888(libusb_context *ctx, libusb_device_handle **out_h) {
    if (!out_h) return LIBUSB_ERROR_INVALID_PARAM;
    *out_h = NULL;

    struct dev_match m = {0};
    int r = find_device(ctx, RX888_PID_APP, &m);
    if (r == 0) {
        r = libusb_open(m.dev, out_h);
        libusb_unref_device(m.dev);
        if (r < 0 || !*out_h) return (r < 0) ? r : LIBUSB_ERROR_IO;
        return 0;
    }

    /* Not found in app mode. If we have firmware, try bootloader. */
    if (!g_firmware_path) return r;

    vlogf(1, "RX888 app PID not found; trying bootloader + firmware upload...\n");

    r = find_device(ctx, RX888_PID_BOOT, &m);
    if (r != 0) {
        vlogf(0, "RX888 bootloader PID (0x%04x) not found either.\n", RX888_PID_BOOT);
        return r;
    }

    libusb_device_handle *boot = NULL;
    r = libusb_open(m.dev, &boot);
    libusb_unref_device(m.dev);
    if (r < 0 || !boot) return (r < 0) ? r : LIBUSB_ERROR_IO;

    int u = ezusb_load_ram(boot, g_firmware_path, FX_TYPE_FX3, IMG_TYPE_IMG, 1);
    libusb_close(boot);
    boot = NULL;

    if (u != 0) {
        vlogf(0, "Firmware upload failed.\n");
        return LIBUSB_ERROR_IO;
    }

    vlogf(0, "Firmware uploaded; waiting for re-enumeration...\n");
    /* Give the device time to re-enumerate */
    usleep(500 * 1000);

    /* Retry a few times */
    for (int attempt = 0; attempt < 10; attempt++) {
        r = find_device(ctx, RX888_PID_APP, &m);
        if (r == 0) {
            r = libusb_open(m.dev, out_h);
            libusb_unref_device(m.dev);
            if (r < 0 || !*out_h) return (r < 0) ? r : LIBUSB_ERROR_IO;
            return 0;
        }
        usleep(300 * 1000);
    }

    return LIBUSB_ERROR_NO_DEVICE;
}

/* ------------------------- Endpoint / interface pick -------------------- */

struct stream_ep {
    uint8_t iface;
    uint8_t alt;
    uint8_t ep_in;
    uint16_t max_packet;
};

static int pick_bulk_in_endpoint(libusb_device_handle *h, struct stream_ep *out) {
    if (!h || !out) return LIBUSB_ERROR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));

    libusb_device *dev = libusb_get_device(h);
    if (!dev) return LIBUSB_ERROR_NO_DEVICE;

    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0 || !cfg) return (r != 0) ? r : LIBUSB_ERROR_IO;

    /* Prefer interface 0, but be flexible. Choose first BULK IN endpoint found. */
    bool found = false;
    int best_score = 0;
    struct stream_ep best = {0};

    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
            for (uint8_t e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                const uint8_t addr = ep->bEndpointAddress;
                const uint8_t attr = (uint8_t)(ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK);

                if ((addr & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN) continue;
                if (attr != LIBUSB_TRANSFER_TYPE_BULK) continue;

                struct stream_ep cand;
                cand.iface = alt->bInterfaceNumber;
                cand.alt = alt->bAlternateSetting;
                cand.ep_in = addr;
                cand.max_packet = effective_max_packet(dev, ep);

                /* Heuristic: prefer interface 0, alt 0, bigger packets */
                int score = 0;
                if (cand.iface == 0) score += 100;
                if (cand.alt == 0) score += 10;
                score += (int)cand.max_packet;

                if (!found || score > best_score) {
                    found = true;
                    best_score = score;
                    best = cand;
                }
            }
        }
    }

    libusb_free_config_descriptor(cfg);

    if (!found) return LIBUSB_ERROR_NOT_FOUND;
    *out = best;
    return 0;
}

/* ---------------------------- Streaming logic --------------------------- */

struct stream_ctx {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint8_t ep_in;
    unsigned int pktsize;
    unsigned int buf_bytes;

    struct libusb_transfer **xfer;
    uint8_t **buf;
    unsigned int nxfers;

    /* handoff queue + writer thread */
    xfer_queue_t q;
    pthread_t writer_thread;
    bool writer_started;

    /* stats (written by writer thread, read by main thread) */
    atomic_ullong ok_xfers;
    atomic_ullong bad_xfers;
    atomic_ullong bytes_out;

    atomic_bool pipe_broken;
    atomic_uint in_flight;
    atomic_ullong last_cb_ms; /* watchdog: last callback time */
};


static void LIBUSB_CALL stream_cb(struct libusb_transfer *t) {
    struct stream_ctx *s = (struct stream_ctx *)t->user_data;
    if (!s) return;

    /* watchdog timestamp */
    atomic_store(&s->last_cb_ms, monotonic_ms());

    /* Transfer completed (successfully or not): it is no longer in-flight.
       Unconditional: a completed transfer was in-flight by definition.
       A conditional load-then-subtract would be a TOCTOU race. */
    unsigned prev = atomic_fetch_sub(&s->in_flight, 1);
    if (prev == 0) {
        /* Would mean a double-completion (libusb bug) — log it. */
        atomic_fetch_add(&s->bad_xfers, 1);
        vlogf(0, "BUG: in_flight underflow\n");
    }

    if (g_stop) {
        /* don't enqueue/resubmit */
        return;
    }

    /* Enqueue for writer thread; never block here. */
    if (!xq_try_push(&s->q, t)) {
        /* Downstream stalled enough to fill the queue: fail-fast.
           This prevents blocking libusb event handling (which is worse).
           Wake the writer thread so it can exit promptly. */
        atomic_fetch_add(&s->bad_xfers, 1);
        g_stop = 1;
        xq_shutdown(&s->q);
        return;
    }
}

static void sample_fixup_i16_inplace(uint8_t *buf, int len_bytes) {
    size_t nsamp = (size_t)len_bytes / 2u;
    uint16_t *samples = (uint16_t *)buf;
    for (size_t i = 0; i < nsamp; i++) {
        samples[i] ^= (uint16_t)(0xfffeu * (samples[i] & 1u));
    }
}

static void *writer_main(void *arg) {
    struct stream_ctx *s = (struct stream_ctx *)arg;

    while (1) {
        struct libusb_transfer *t = xq_pop_blocking(&s->q);
        if (!t) break; /* shutdown and empty */

        if (g_stop) continue;

        if (t->status != LIBUSB_TRANSFER_COMPLETED) {
            atomic_fetch_add(&s->bad_xfers, 1);
            vlogf(0, "xfer status=%s actual=%d\n", xfer_status_str(t->status), t->actual_length);
            if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
                g_stop = 1;
                continue;
            }
            /* For transient errors we can attempt resubmit below */
        } else {
            atomic_fetch_add(&s->ok_xfers, 1);

            if (t->actual_length > 0) {
                if (g_fixup_samples) {
                    sample_fixup_i16_inplace((uint8_t *)t->buffer, t->actual_length);
                }
                int w = write_full(STDOUT_FILENO, (const uint8_t *)t->buffer, (size_t)t->actual_length);
                if (w != 0) {
                    atomic_store(&s->pipe_broken, true);
                    g_stop = 1;
                    continue;
                }
                atomic_fetch_add(&s->bytes_out, (uint64_t)t->actual_length);
            }
        }

        if (!g_stop) {
            /* NOTE: We call libusb_submit_transfer() here in the writer
               thread while libusb_handle_events_timeout_completed() runs
               in the main thread.  This is safe on Linux because libusb's
               default backend (poll-based) is fully thread-safe for
               submit/cancel operations.  This would NOT be safe on all
               libusb backends (e.g. some Windows/macOS backends).  If
               porting, verify thread-safety or move resubmission into
               the event-handling thread. */
            int r = libusb_submit_transfer(t);
            if (r == 0) {
                atomic_fetch_add(&s->in_flight, 1);
            } else {
                atomic_fetch_add(&s->bad_xfers, 1);
                vlogf(0, "submit_transfer failed: %s\n", libusb_error_name(r));
                g_stop = 1;
            }
        }
    }
    return NULL;
}

int stream_setup(struct stream_ctx *s,
                        libusb_context *ctx,
                        libusb_device_handle *h,
                        uint8_t ep_in,
                        unsigned int pktsize)
{
    int r = 0;

    /* Start from a known state so stream_teardown() is always safe
       once we've successfully initialized the queue. */
    memset(s, 0, sizeof(*s));
    atomic_store(&s->last_cb_ms, monotonic_ms());

    s->ctx = ctx;
    s->h = h;
    s->ep_in = ep_in;
    s->pktsize = pktsize;

    s->nxfers = g_queue_depth;

    /* compute transfer size */
    uint64_t bb = (uint64_t)g_req_packets * (uint64_t)pktsize;
    if (bb == 0 || bb > (uint64_t)INT_MAX) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    s->buf_bytes = (unsigned int)bb;

    /* queue capacity: extra elasticity beyond nxfers */
    size_t qcap = (size_t)s->nxfers * 2u;
    if (qcap < 2u) qcap = 2u;

    r = xq_init(&s->q, qcap);
    if (r != 0) {
        /* xq_init returns negative errno-style in your file */
        return r;
    }

    atomic_store(&s->in_flight, 0);

    if (pthread_create(&s->writer_thread, NULL, writer_main, s) != 0) {
        r = LIBUSB_ERROR_OTHER;
        goto fail;
    }
    s->writer_started = true;

    s->xfer = calloc(s->nxfers, sizeof(*s->xfer));
    s->buf  = calloc(s->nxfers, sizeof(*s->buf));
    if (!s->xfer || !s->buf) {
        r = LIBUSB_ERROR_NO_MEM;
        goto fail;
    }

    for (unsigned int i = 0; i < s->nxfers; i++) {
        s->buf[i] = (uint8_t *)malloc((size_t)s->buf_bytes);
        if (!s->buf[i]) {
            r = LIBUSB_ERROR_NO_MEM;
            goto fail;
        }

        s->xfer[i] = libusb_alloc_transfer(0);
        if (!s->xfer[i]) {
            r = LIBUSB_ERROR_NO_MEM;
            goto fail;
        }

        libusb_fill_bulk_transfer(
            s->xfer[i], h, ep_in,
            s->buf[i], (int)s->buf_bytes,
            stream_cb, s,
            g_stream_timeout_ms
        );
    }

    return 0;

fail:
    /* stream_teardown() expects queue to be initialized if called,
       which it is at this point. */
    stream_teardown(s);
    return r;
}

void stream_teardown(struct stream_ctx *s) {
    if (!s) return;

    /* Stop resubmissions first (writer thread checks g_stop). */
    g_stop = 1;

    /* Request cancellation of all transfers */
    for (unsigned int i = 0; i < s->nxfers; i++) {
        if (s->xfer && s->xfer[i]) {
            (void)libusb_cancel_transfer(s->xfer[i]);
        }
    }

    /* Pump events for a short time so cancellations can complete and callbacks enqueue results. */
    uint64_t start = monotonic_ms();
    while (atomic_load(&s->in_flight) > 0 && (monotonic_ms() - start) < 2000) {
        struct timeval tv = {0, 50 * 1000};
        (void)libusb_handle_events_timeout_completed(s->ctx, &tv, NULL);
    }

    /* Stop writer thread and drain queue */
    xq_shutdown(&s->q);
    if (s->writer_started) {
        (void)pthread_join(s->writer_thread, NULL);
        s->writer_started = false;
    }

    xq_destroy(&s->q);

    unsigned int leaked = atomic_load(&s->in_flight);
    if (leaked > 0) {
        fprintf(stderr,
                "warning: %u transfers still in-flight after drain; "
                "leaking to avoid use-after-free\n", leaked);
    }

    for (unsigned int i = 0; i < s->nxfers; i++) {
        if (leaked > 0) {
            /* Can't tell which transfers libusb still owns;
               skip freeing to avoid use-after-free.
               Process exit will reclaim the memory. */
            break;
        }
        if (s->xfer && s->xfer[i]) {
            libusb_free_transfer(s->xfer[i]);
            s->xfer[i] = NULL;
        }
        if (s->buf && s->buf[i]) {
            free(s->buf[i]);
            s->buf[i] = NULL;
        }
    }
    free(s->xfer);
    free(s->buf);
    s->xfer = NULL;
    s->buf = NULL;
    s->nxfers = 0;
}

static int samplerate_allowed(unsigned int sr) {
    return (sr == 32000000u) || (sr == 135000000u);
}

static int parse_u32(const char *s, unsigned int *out) {
    if (!s || !*s || !out) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || (end && *end != '\0')) return -1;
    if (v > 0xffffffffUL) return -1;
    *out = (unsigned int)v;
    return 0;
}

static int parse_i32(const char *s, long *out) {
    if (!s || !*s || !out) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || (end && *end != '\0')) return -1;
    *out = v;
    return 0;
}


/* ------------------------------- CLI / main ----------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] > iq.raw\n"
        "\n"
        "Options:\n"
        "  -f, --firmware <file>     Upload firmware if device is in boot mode\n"
        "  -v, --verbose[=N]         Verbosity (default 0). If -v w/out N => 1\n"
        "  -d, --dither              Enable dither GPIO\n"
        "  -r, --rand                Enable randomizer GPIO\n"
        "  -s, --samplerate <Hz>     Sample rate (supported: 32000000, 135000000)\n"
        "  -m, --gainmode <low|high> Gain mode (sets bit7). Default: high\n"
        "  -g, --gain <0..127>       VGA gain value (bits[6:0])\n"
        "  -a, --att <0..63>         Attenuation value\n"
        "      --fixup               Enable legacy sample fixup (bit tweak)\n"
        "  -q, --queuedepth <N>      Concurrent USB transfers (default %u)\n"
        "  -p, --reqsize <N>         Transfer size in packets (default %u)\n"
        "      --ctrl-timeout <ms>   Control transfer timeout (default %u)\n"
        "      --stream-timeout <ms> Stream transfer timeout (default %u; 0=infinite)\n"
        "      --watchdog-timeout <ms> No-data watchdog (default %u; 0=disabled)\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Notes:\n"
        "  Streams bulk IN samples to stdout (int16 real).\n"
        "  For USB3 performance, tune -p/-q to stay within usbfs_memory_mb.\n",
        argv0,
        g_queue_depth,
        g_req_packets,
        g_ctrl_timeout_ms,
        g_stream_timeout_ms,
        g_watchdog_ms
    );
}


int main(int argc, char **argv) {
    /* Signals */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL); /* we detect EPIPE via write_full */

    /* Options */
    while (1) {
	/* in your option parsing loop */
	static struct option long_opts[] = {
		{"firmware",       required_argument, 0, 'f'},
		{"verbose",        optional_argument, 0, 'v'},
		{"dither",         no_argument,       0, 'd'},
		{"rand",           no_argument,       0, 'r'},

		{"samplerate",     required_argument, 0, 's'},
		{"gainmode",       required_argument, 0, 'm'},
		{"gain",           required_argument, 0, 'g'},
		{"att",            required_argument, 0, 'a'},
		{"fixup",          no_argument,       0, 1003},

		{"queuedepth",     required_argument, 0, 'q'},
		{"reqsize",        required_argument, 0, 'p'},
		{"ctrl-timeout",   required_argument, 0, 1001},
		{"stream-timeout", required_argument, 0, 1002},
		{"watchdog-timeout", required_argument, 0, 1004},

		{"help",           no_argument,       0, 'h'},
		{0,0,0,0}
	};

	int opt_idx = 0;
	int c = getopt_long(argc, argv, "f:v::drs:m:g:a:q:p:h", long_opts, &opt_idx);
	if (c == -1) break;

	switch (c) {
		case 'f':
		g_firmware_path = optarg;
		break;

		case 'v':
		if (optarg) g_verbose = (int)strtoul(optarg, NULL, 10);
		else g_verbose = 1;
		break;

		case 'd':
		g_dither = 1;
		break;

		case 'r':
		g_randomizer = 1;
		break;

		case 's': {
		unsigned int sr = 0;
		if (parse_u32(optarg, &sr) != 0 || !samplerate_allowed(sr)) {
			fprintf(stderr,
			        "Unsupported samplerate '%s'. Supported: 32000000, 135000000\n",
			        optarg ? optarg : "(null)");
			return 2;
		}
		g_samplerate = sr;
		break;
		}

		case 'm':
		if (strcmp(optarg, "high") == 0) {
			g_gain |= 0x80u;
		} else if (strcmp(optarg, "low") == 0) {
			g_gain &= 0x7fu;
		} else {
			fprintf(stderr, "Invalid gain mode '%s' (use 'low' or 'high')\n", optarg);
			return 2;
		}
		break;

		case 'g': {
		long gv = 0;
		if (parse_i32(optarg, &gv) != 0 || gv < 0 || gv > 127) {
			fprintf(stderr, "Invalid gain value '%s' (expected 0..127)\n", optarg);
			return 2;
		}
		g_gain = (g_gain & 0x80u) | (unsigned int)gv;
		break;
		}

		case 'a': {
		long av = 0;
		if (parse_i32(optarg, &av) != 0 || av < 0 || av > 63) {
			fprintf(stderr, "Invalid attenuation value '%s' (expected 0..63)\n", optarg);
			return 2;
		}
		g_att = (unsigned int)av;
		break;
		}

		case 'q': {
		unsigned int qd = 0;
		if (parse_u32(optarg, &qd) != 0 || qd < 1 || qd > 4096) {
			fprintf(stderr, "Invalid queuedepth '%s'\n", optarg);
			return 2;
		}
		g_queue_depth = qd;
		break;
		}

		case 'p': {
		unsigned int rp = 0;
		if (parse_u32(optarg, &rp) != 0 || rp < 1 || rp > (1024u * 1024u)) {
			fprintf(stderr, "Invalid reqsize '%s'\n", optarg);
			return 2;
		}
		g_req_packets = rp;
		break;
		}

		case 1001: { /* --ctrl-timeout */
		unsigned int ms = 0;
		if (parse_u32(optarg, &ms) != 0) {
			fprintf(stderr, "Invalid ctrl-timeout '%s'\n", optarg);
			return 2;
		}
		g_ctrl_timeout_ms = ms;
		break;
		}

		case 1002: { /* --stream-timeout */
		unsigned int ms = 0;
		if (parse_u32(optarg, &ms) != 0) {
			fprintf(stderr, "Invalid stream-timeout '%s'\n", optarg);
			return 2;
		}
		g_stream_timeout_ms = ms;
		break;
		}

		case 1004: { /* --watchdog-timeout */
		unsigned int ms = 0;
		if (parse_u32(optarg, &ms) != 0) {
			fprintf(stderr, "Invalid watchdog-timeout '%s'\n", optarg);
			return 2;
		}
		g_watchdog_ms = ms;
		break;
		}

		case 1003: /* --fixup */
		g_fixup_samples = 1;
		break;

		case 'h':
		default:
		usage(argv[0]);
		return (c == 'h') ? 0 : 2;
        } 
    }

	/* Sync verbose for ezusb.c before anything else */
	verbose = g_verbose;

	/* After parsing options */
	if (!samplerate_allowed(g_samplerate)) {
		fprintf(stderr, "Internal error: samplerate %u not allowed\n", g_samplerate);
		return 2;
	}

	/* Gain sanity: g_gain bit7 = mode, low 7 bits = 0..127 */
	if ((g_gain & 0x7fu) > 127u) {
		fprintf(stderr, "Invalid gain internal value: 0x%02x\n", g_gain);
		return 2;
	}
	if (g_att > 63u) {
		fprintf(stderr, "Invalid attenuation internal value: %u\n", g_att);
		return 2;
	}
	if (g_queue_depth == 0 || g_req_packets == 0) {
		fprintf(stderr, "Invalid USB sizing (-q/-p)\n");
		return 2;
	}

	if (isatty(STDOUT_FILENO)) {
		fprintf(stderr, "rx888_stream: stdout is a TTY; redirect to a file or pipe\n");
		return 2;
	}

    int rc = 0;
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;

    struct stream_ep sep = {0};
    struct stream_ctx sctx;
    bool claimed = false;

    /* Init libusb */
    int r = libusb_init(&ctx);
    if (r != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(r));
        return 1;
    }
    if (g_verbose >= 3) {
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    } else if (g_verbose >= 2) {
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
    } else if (g_verbose >= 1) {
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    }

    /* Open device (with optional firmware upload) */
    r = open_rx888(ctx, &h);
    if (r != 0) {
        fprintf(stderr, "Could not open RX888: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    /* Detach kernel driver if needed */
    int kd = libusb_kernel_driver_active(h, 0);
    if (kd == 1) {
        vlogf(0, "Kernel driver active; detaching...\n");
        r = libusb_detach_kernel_driver(h, 0);
        if (r != 0) {
            fprintf(stderr, "Failed to detach kernel driver: %s\n", libusb_error_name(r));
            rc = 1;
            goto out;
        }
    }

    /* Claim interface and pick endpoint */
    r = pick_bulk_in_endpoint(h, &sep);
    if (r != 0) {
        fprintf(stderr, "Failed to find bulk IN endpoint: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    vlogf(1, "Using iface=%u alt=%u ep=0x%02x maxpkt=%u\n",
          sep.iface, sep.alt, sep.ep_in, sep.max_packet);

    r = libusb_claim_interface(h, sep.iface);
    if (r != 0) {
        fprintf(stderr, "Failed to claim interface %u: %s\n", sep.iface, libusb_error_name(r));
        rc = 1;
        goto out;
    }
    claimed = true;

    if (sep.alt != 0) {
        r = libusb_set_interface_alt_setting(h, sep.iface, sep.alt);
        if (r != 0) {
            fprintf(stderr, "Failed to set alt setting %u: %s\n", sep.alt, libusb_error_name(r));
            rc = 1;
            goto out;
        }
    }


    /* Ensure FX3 streaming engine is stopped before reconfiguring. */
    (void)rx888_stop_stream(h);
        /* Log requested configuration */
    if (g_verbose) {
        double actual = actual_freq((double)g_samplerate);
        fprintf(stderr, "Ref. Clock: %u Hz\n", g_xtal_hz);
        fprintf(stderr, "Requested Sample Rate: %u Hz (actual ~ %.3f Hz)\n", g_samplerate, actual);
        fprintf(stderr, "Randomizer %s, Dither %s\n", g_randomizer ? "On" : "Off", g_dither ? "On" : "Off");
        fprintf(stderr, "Gain Mode: %s, Gain: %u, Att: %u\n", (g_gain & 0x80u) ? "High" : "Low", (g_gain & 0x7fu), g_att);
        fprintf(stderr, "Sample fixup: %s\n", g_fixup_samples ? "On" : "Off");
    }

    /* Apply front-end and clock configuration (restored from original) */
    uint32_t gpio = 0;
    if (g_dither) gpio |= (uint32_t)DITH;
    if (g_randomizer) gpio |= (uint32_t)RANDO;

    usleep(CTRL_SETTLE_US);
    r = rx888_gpio(h, gpio);
    if (r != 0) {
        fprintf(stderr, "GPIO set failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    usleep(CTRL_SETTLE_US);
    r = rx888_set_arg(h, (uint16_t)DAT31_ATT, (uint16_t)g_att);
    if (r != 0) {
        fprintf(stderr, "Set attenuation failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    usleep(CTRL_SETTLE_US);
    r = rx888_set_arg(h, (uint16_t)AD8340_VGA, (uint16_t)g_gain);
    if (r != 0) {
        fprintf(stderr, "Set gain failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    usleep(CTRL_SETTLE_US);
    r = rx888_cmd_u32(h, (uint8_t)STARTADC, (uint32_t)g_samplerate);
    if (r != 0) {
        fprintf(stderr, "STARTADC failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    usleep(CTRL_SETTLE_US);
    r = rx888_start_stream(h);
    if (r != 0) {
        fprintf(stderr, "STARTFX3 failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto out;
    }

    /* Put tuner in standby as original did (best-effort). */
    usleep(CTRL_SETTLE_US);
    (void)rx888_cmd_u32(h, (uint8_t)TUNERSTDBY, 0u);


    /* Setup transfers (malloc-only for safety) */
    r = stream_setup(&sctx, ctx, h, sep.ep_in, sep.max_packet);
    if (r != 0) {
        fprintf(stderr, "Stream setup failed: %s\n", libusb_error_name(r));
        rc = 1;
        goto stop_stream;
    }

    /* Submit transfers */
    for (unsigned int i = 0; i < sctx.nxfers; i++) {
        r = libusb_submit_transfer(sctx.xfer[i]);
        if (r == 0) atomic_fetch_add(&sctx.in_flight, 1);
        else {
            fprintf(stderr, "submit_transfer failed: %s\n", libusb_error_name(r));
            rc = 1;
            g_stop = 1;
            break;
        }
    }

    uint64_t t0 = monotonic_ms();
    uint64_t last = t0;

    /* Event loop */
    while (!g_stop) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000; /* 200ms tick */
        r = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        
        if (r == LIBUSB_ERROR_INTERRUPTED) {
        /* SIGINT/SIGTERM commonly interrupts poll/select inside libusb.
        If we're stopping, treat as clean shutdown; otherwise retry. */
        if (g_stop) {
            break;
            }
        continue;
        }
        
        if (r != 0) {
            fprintf(stderr, "handle_events error: %s\n", libusb_error_name(r));
            rc = 1;
            break;
        }

        /* No-data watchdog: if callbacks stop arriving while transfers are in-flight,
           the FX3/USB link may be wedged. Bail out so caller can reset device. */
        uint64_t last_cb = atomic_load(&sctx.last_cb_ms);
        if (!g_stop && g_watchdog_ms > 0 && atomic_load(&sctx.in_flight) > 0) {
            uint64_t now_ms = monotonic_ms();
            if (last_cb != 0 && (now_ms - last_cb) > g_watchdog_ms) {
                fprintf(stderr, "No USB data for %" PRIu64 " ms; stopping. (in_flight=%u)\n",
                        (now_ms - last_cb), atomic_load(&sctx.in_flight));
                g_stop = 1;
            }
        }

        uint64_t now = monotonic_ms();
        if (g_verbose && (now - last) >= 1000) {
            double secs = (double)(now - t0) / 1000.0;
            uint64_t bout = atomic_load(&sctx.bytes_out);
            double mbps = (secs > 0) ? (double)bout / (1024.0 * 1024.0) / secs : 0.0;
            fprintf(stderr,
                    "t=%.0fs ok=%" PRIu64 " bad=%" PRIu64 " in_flight=%u out=%.2f MiB (%.2f MiB/s)\n",
                    secs, (uint64_t)atomic_load(&sctx.ok_xfers),
                    (uint64_t)atomic_load(&sctx.bad_xfers),
                    atomic_load(&sctx.in_flight),
                    (double)bout / (1024.0 * 1024.0), mbps);
            last = now;
        }
    }

    /* Stop: cancel and teardown */
    stream_teardown(&sctx);

stop_stream:
    (void)rx888_stop_stream(h);

out:
    if (h) {
        if (claimed) {
            (void)libusb_release_interface(h, sep.iface);
        }
        libusb_close(h);
        h = NULL;
    }
    if (ctx) {
        libusb_exit(ctx);
        ctx = NULL;
    }

    return rc;
}
