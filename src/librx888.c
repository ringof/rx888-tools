/*
 * librx888 — minimal streaming library for the RX888 mk II
 *
 * Lifted from rx888_stream.c (Ruslan Migirov 2021, David Goncalves 2024-).
 * The wire-protocol path, descriptor parsing, queue handoff, watchdog,
 * and teardown sequence are unchanged from that file. The only
 * structural change is replacing the writer thread's stdout write
 * with a user-supplied callback, and replacing main()/CLI parsing
 * with rx888_open/start/stop/close.
 *
 * Copyright 2021 Ruslan Migirov
 * Copyright 2024- David Goncalves
 * Copyright 2026 Free Software Foundation, Inc.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ezusb.h"
#include "librx888.h"
#include "rx888.h"

/* ezusb.c expects this symbol */
int verbose = 0;

#define LIBRX888_VERSION_STR "0.0.1"

/* USB ids */
static const uint16_t RX888_VID      = 0x04b4;
static const uint16_t RX888_PID_BOOT = 0x00f3;
static const uint16_t RX888_PID_APP  = 0x00f1;

/* Tunables (compiled-in for v0.0; expose as setters later if needed). */
static const unsigned int CFG_QUEUE_DEPTH       = 32;
static const unsigned int CFG_REQ_PACKETS       = 1024;
static const unsigned int CFG_CTRL_TIMEOUT_MS   = 5000;
static const unsigned int CFG_STREAM_TIMEOUT_MS = 0;     /* infinite */
static const unsigned int CFG_WATCHDOG_MS       = 3000;
static const unsigned int CFG_CTRL_SETTLE_US    = 5000;

/* ----------------------------- helpers ---------------------------------- */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

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
            if (bursts > 0 && bursts < 256U) return base * bursts;
        }
#endif
    }
#endif
    return base;
}

/* ------------------------ Transfer handoff queue ------------------------ */

typedef struct xfer_queue {
    struct libusb_transfer **ring;
    size_t cap, head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t  cv_nonempty;
    bool shutdown;
} xfer_queue_t;

static int xq_init(xfer_queue_t *q, size_t cap) {
    memset(q, 0, sizeof(*q));
    q->ring = calloc(cap, sizeof(*q->ring));
    if (!q->ring) return -1;
    if (pthread_mutex_init(&q->mu, NULL) != 0) { free(q->ring); return -1; }
    if (pthread_cond_init(&q->cv_nonempty, NULL) != 0) {
        pthread_mutex_destroy(&q->mu); free(q->ring); return -1;
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
    if (!q || !q->ring) return;
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
    while (!q->shutdown && q->count == 0)
        pthread_cond_wait(&q->cv_nonempty, &q->mu);
    if (q->count > 0) {
        t = q->ring[q->head];
        q->ring[q->head] = NULL;
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return t;
}

/* ------------------------ control transfers ----------------------------- */

static int ctrl_write_u32(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex, uint32_t value_le) {
    uint8_t data[4] = {
        (uint8_t)(value_le & 0xffu),
        (uint8_t)((value_le >> 8) & 0xffu),
        (uint8_t)((value_le >> 16) & 0xffu),
        (uint8_t)((value_le >> 24) & 0xffu),
    };
    int r = libusb_control_transfer(
        h, (uint8_t)(LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE),
        request, wValue, wIndex, data, (uint16_t)sizeof(data), CFG_CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)sizeof(data)) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_write_buf(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex,
                          const uint8_t *buf, uint16_t len) {
    int r = libusb_control_transfer(
        h, (uint8_t)(LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE),
        request, wValue, wIndex, (unsigned char *)buf, len, CFG_CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)len) return LIBUSB_ERROR_IO;
    return 0;
}

static int rx888_set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val) {
    uint8_t zero = 0;
    return ctrl_write_buf(h, (uint8_t)SETARGFX3, arg_val, arg_id, &zero, 1);
}

static int rx888_gpio(libusb_device_handle *h, uint32_t gpio_bits) {
    return ctrl_write_u32(h, (uint8_t)GPIOFX3, 0, 0, gpio_bits);
}

static int rx888_cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val) {
    return ctrl_write_u32(h, cmd, 0, 0, val);
}

/* -------------------------- discovery / open --------------------------- */

static int find_device(libusb_context *ctx, uint16_t pid, libusb_device **out_dev) {
    *out_dev = NULL;
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) return (int)n;
    int rc = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor == RX888_VID && d.idProduct == pid) {
            *out_dev = list[i];
            libusb_ref_device(*out_dev);
            rc = 0;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return rc;
}

static int open_device(libusb_context *ctx, const char *firmware_path,
                       libusb_device_handle **out_h) {
    *out_h = NULL;
    libusb_device *dev = NULL;
    int r = find_device(ctx, RX888_PID_APP, &dev);
    if (r == 0) {
        r = libusb_open(dev, out_h);
        libusb_unref_device(dev);
        return (r < 0) ? r : (*out_h ? 0 : LIBUSB_ERROR_IO);
    }
    if (!firmware_path) return r;

    r = find_device(ctx, RX888_PID_BOOT, &dev);
    if (r != 0) return r;
    libusb_device_handle *boot = NULL;
    r = libusb_open(dev, &boot);
    libusb_unref_device(dev);
    if (r < 0 || !boot) return (r < 0) ? r : LIBUSB_ERROR_IO;

    int u = ezusb_load_ram(boot, firmware_path, FX_TYPE_FX3, IMG_TYPE_IMG, 1);
    libusb_close(boot);
    if (u != 0) return LIBUSB_ERROR_IO;
    usleep(500 * 1000);

    for (int attempt = 0; attempt < 10; attempt++) {
        r = find_device(ctx, RX888_PID_APP, &dev);
        if (r == 0) {
            r = libusb_open(dev, out_h);
            libusb_unref_device(dev);
            return (r < 0) ? r : (*out_h ? 0 : LIBUSB_ERROR_IO);
        }
        usleep(300 * 1000);
    }
    return LIBUSB_ERROR_NO_DEVICE;
}

struct stream_ep {
    uint8_t  iface;
    uint8_t  alt;
    uint8_t  ep_in;
    uint16_t max_packet;
};

static int pick_bulk_in_endpoint(libusb_device_handle *h, struct stream_ep *out) {
    memset(out, 0, sizeof(*out));
    libusb_device *dev = libusb_get_device(h);
    if (!dev) return LIBUSB_ERROR_NO_DEVICE;
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0 || !cfg) return (r != 0) ? r : LIBUSB_ERROR_IO;

    bool found = false;
    int best_score = 0;
    struct stream_ep best = {0};
    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
            for (uint8_t e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN) continue;
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                struct stream_ep cand = {
                    .iface = alt->bInterfaceNumber,
                    .alt   = alt->bAlternateSetting,
                    .ep_in = ep->bEndpointAddress,
                    .max_packet = effective_max_packet(dev, ep),
                };
                int score = (cand.iface == 0 ? 100 : 0)
                          + (cand.alt   == 0 ? 10  : 0)
                          + (int)cand.max_packet;
                if (!found || score > best_score) {
                    found = true; best_score = score; best = cand;
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!found) return LIBUSB_ERROR_NOT_FOUND;
    *out = best;
    return 0;
}

/* ----------------------------- handle struct ---------------------------- */

struct rx888 {
    /* config snapshot */
    rx888_config_t cfg;
    int            fixup_samples;

    /* USB */
    libusb_context        *ctx;
    libusb_device_handle  *h;
    bool                   claimed;
    struct stream_ep       ep;

    /* user callback */
    rx888_sample_cb_t cb;
    void             *cb_user;

    /* streaming state */
    struct libusb_transfer **xfer;
    uint8_t                **buf;
    unsigned int             nxfers;
    unsigned int             buf_bytes;

    xfer_queue_t q;
    pthread_t    writer_thread;
    pthread_t    event_thread;
    bool         writer_started;
    bool         event_started;

    atomic_int     stop_flag;
    atomic_uint    in_flight;
    atomic_ullong  last_cb_ms;
    atomic_ullong  ok_xfers;
    atomic_ullong  bad_xfers;
};

/* ------------------------------ streaming ------------------------------- */

static void sample_fixup_i16_inplace(uint8_t *buf, int len_bytes) {
    size_t nsamp = (size_t)len_bytes / 2u;
    uint16_t *samples = (uint16_t *)buf;
    for (size_t i = 0; i < nsamp; i++) {
        samples[i] ^= (uint16_t)(0xfffeu * (samples[i] & 1u));
    }
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *t) {
    rx888_t *r = (rx888_t *)t->user_data;
    if (!r) return;

    atomic_store(&r->last_cb_ms, monotonic_ms());
    atomic_fetch_sub(&r->in_flight, 1);

    if (atomic_load(&r->stop_flag)) return;

    if (!xq_try_push(&r->q, t)) {
        atomic_fetch_add(&r->bad_xfers, 1);
        atomic_store(&r->stop_flag, 1);
        xq_shutdown(&r->q);
    }
}

static void *writer_main(void *arg) {
    rx888_t *r = (rx888_t *)arg;
    for (;;) {
        struct libusb_transfer *t = xq_pop_blocking(&r->q);
        if (!t) break;
        if (atomic_load(&r->stop_flag)) continue;

        if (t->status == LIBUSB_TRANSFER_COMPLETED) {
            atomic_fetch_add(&r->ok_xfers, 1);
            if (t->actual_length > 0) {
                if (r->fixup_samples)
                    sample_fixup_i16_inplace((uint8_t *)t->buffer, t->actual_length);
                if (r->cb) {
                    r->cb((const int16_t *)t->buffer,
                          (size_t)t->actual_length / sizeof(int16_t),
                          r->cb_user);
                }
            }
        } else {
            atomic_fetch_add(&r->bad_xfers, 1);
            if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
                atomic_store(&r->stop_flag, 1);
                continue;
            }
        }

        if (!atomic_load(&r->stop_flag)) {
            int rc = libusb_submit_transfer(t);
            if (rc == 0) atomic_fetch_add(&r->in_flight, 1);
            else { atomic_fetch_add(&r->bad_xfers, 1); atomic_store(&r->stop_flag, 1); }
        }
    }
    return NULL;
}

static void *event_main(void *arg) {
    rx888_t *r = (rx888_t *)arg;
    while (!atomic_load(&r->stop_flag)) {
        struct timeval tv = {0, 200 * 1000};
        int rc = libusb_handle_events_timeout_completed(r->ctx, &tv, NULL);
        if (rc == LIBUSB_ERROR_INTERRUPTED) continue;
        if (rc != 0) { atomic_store(&r->stop_flag, 1); break; }

        if (CFG_WATCHDOG_MS > 0 && atomic_load(&r->in_flight) > 0) {
            uint64_t last = atomic_load(&r->last_cb_ms);
            if (last && monotonic_ms() - last > CFG_WATCHDOG_MS) {
                atomic_store(&r->stop_flag, 1);
                break;
            }
        }
    }
    return NULL;
}

/* ----------------------------- public API ------------------------------- */

const char *rx888_strerror(int err) { return libusb_error_name(err); }
const char *rx888_version(void)     { return LIBRX888_VERSION_STR; }

int rx888_open(rx888_t **out, const rx888_config_t *cfg) {
    if (!out || !cfg) return LIBUSB_ERROR_INVALID_PARAM;
    *out = NULL;

    rx888_t *r = calloc(1, sizeof(*r));
    if (!r) return LIBUSB_ERROR_NO_MEM;
    r->cfg           = *cfg;
    r->fixup_samples = cfg->fixup_samples;
    atomic_store(&r->last_cb_ms, monotonic_ms());

    int rc = libusb_init(&r->ctx);
    if (rc != 0) { free(r); return rc; }

    rc = open_device(r->ctx, cfg->firmware_path, &r->h);
    if (rc != 0) goto fail;

    int kd = libusb_kernel_driver_active(r->h, 0);
    if (kd == 1) {
        rc = libusb_detach_kernel_driver(r->h, 0);
        if (rc != 0) goto fail;
    }

    rc = pick_bulk_in_endpoint(r->h, &r->ep);
    if (rc != 0) goto fail;

    rc = libusb_claim_interface(r->h, r->ep.iface);
    if (rc != 0) goto fail;
    r->claimed = true;

    if (r->ep.alt != 0) {
        rc = libusb_set_interface_alt_setting(r->h, r->ep.iface, r->ep.alt);
        if (rc != 0) goto fail;
    }

    /* Make sure streaming engine is idle before reconfiguring. */
    (void)rx888_cmd_u32(r->h, (uint8_t)STOPFX3, 0u);

    /* Apply config: GPIO -> attenuator -> VGA -> clock. */
    uint32_t gpio = 0;
    if (cfg->dither)     gpio |= (uint32_t)DITH;
    if (cfg->randomizer) gpio |= (uint32_t)RANDO;
    usleep(CFG_CTRL_SETTLE_US);
    rc = rx888_gpio(r->h, gpio);
    if (rc != 0) goto fail;

    usleep(CFG_CTRL_SETTLE_US);
    rc = rx888_set_arg(r->h, (uint16_t)DAT31_ATT, (uint16_t)(cfg->att & 0x3f));
    if (rc != 0) goto fail;

    usleep(CFG_CTRL_SETTLE_US);
    uint16_t vga = (uint16_t)((cfg->gain & 0x7f) | (cfg->gain_high ? 0x80 : 0));
    rc = rx888_set_arg(r->h, (uint16_t)AD8340_VGA, vga);
    if (rc != 0) goto fail;

    usleep(CFG_CTRL_SETTLE_US);
    rc = rx888_cmd_u32(r->h, (uint8_t)STARTADC, (uint32_t)cfg->samplerate);
    if (rc != 0) goto fail;

    *out = r;
    return 0;

fail:
    rx888_close(r);
    return rc;
}

int rx888_start(rx888_t *r, rx888_sample_cb_t cb, void *user) {
    if (!r) return LIBUSB_ERROR_INVALID_PARAM;
    r->cb      = cb;
    r->cb_user = user;
    atomic_store(&r->stop_flag, 0);

    /* Allocate transfer ring. */
    r->nxfers    = CFG_QUEUE_DEPTH;
    uint64_t bb  = (uint64_t)CFG_REQ_PACKETS * (uint64_t)r->ep.max_packet;
    if (bb == 0 || bb > (uint64_t)INT_MAX) return LIBUSB_ERROR_INVALID_PARAM;
    r->buf_bytes = (unsigned int)bb;

    int rc = xq_init(&r->q, (size_t)r->nxfers * 2u);
    if (rc != 0) return LIBUSB_ERROR_NO_MEM;

    if (pthread_create(&r->writer_thread, NULL, writer_main, r) != 0) {
        xq_destroy(&r->q);
        return LIBUSB_ERROR_OTHER;
    }
    r->writer_started = true;

    r->xfer = calloc(r->nxfers, sizeof(*r->xfer));
    r->buf  = calloc(r->nxfers, sizeof(*r->buf));
    if (!r->xfer || !r->buf) { rc = LIBUSB_ERROR_NO_MEM; goto fail; }

    for (unsigned int i = 0; i < r->nxfers; i++) {
        r->buf[i]  = (uint8_t *)malloc(r->buf_bytes);
        r->xfer[i] = libusb_alloc_transfer(0);
        if (!r->buf[i] || !r->xfer[i]) { rc = LIBUSB_ERROR_NO_MEM; goto fail; }
        libusb_fill_bulk_transfer(r->xfer[i], r->h, r->ep.ep_in,
                                  r->buf[i], (int)r->buf_bytes,
                                  stream_cb, r, CFG_STREAM_TIMEOUT_MS);
    }

    /* Kick off STARTFX3 then submit transfers. */
    usleep(CFG_CTRL_SETTLE_US);
    rc = rx888_cmd_u32(r->h, (uint8_t)STARTFX3, 0u);
    if (rc != 0) goto fail;
    usleep(CFG_CTRL_SETTLE_US);
    (void)rx888_cmd_u32(r->h, (uint8_t)TUNERSTDBY, 0u);

    for (unsigned int i = 0; i < r->nxfers; i++) {
        rc = libusb_submit_transfer(r->xfer[i]);
        if (rc != 0) { atomic_store(&r->stop_flag, 1); goto fail; }
        atomic_fetch_add(&r->in_flight, 1);
    }

    if (pthread_create(&r->event_thread, NULL, event_main, r) != 0) {
        rc = LIBUSB_ERROR_OTHER;
        atomic_store(&r->stop_flag, 1);
        goto fail;
    }
    r->event_started = true;
    return 0;

fail:
    rx888_stop(r);
    return rc;
}

void rx888_stop(rx888_t *r) {
    if (!r) return;
    atomic_store(&r->stop_flag, 1);

    /* Cancel transfers. */
    if (r->xfer) {
        for (unsigned int i = 0; i < r->nxfers; i++)
            if (r->xfer[i]) (void)libusb_cancel_transfer(r->xfer[i]);
    }

    /* Pump events briefly so cancellations complete. */
    uint64_t start = monotonic_ms();
    while (atomic_load(&r->in_flight) > 0 && (monotonic_ms() - start) < 2000) {
        struct timeval tv = {0, 50 * 1000};
        (void)libusb_handle_events_timeout_completed(r->ctx, &tv, NULL);
    }

    if (r->event_started)  { pthread_join(r->event_thread, NULL);  r->event_started = false; }
    xq_shutdown(&r->q);
    if (r->writer_started) { pthread_join(r->writer_thread, NULL); r->writer_started = false; }
    xq_destroy(&r->q);

    /* Tell device to stop. */
    if (r->h) (void)rx888_cmd_u32(r->h, (uint8_t)STOPFX3, 0u);

    /* Free transfers (may leak if libusb still owns any). */
    unsigned int leaked = atomic_load(&r->in_flight);
    for (unsigned int i = 0; r->xfer && i < r->nxfers; i++) {
        if (leaked) break;  /* don't risk use-after-free */
        if (r->xfer[i]) { libusb_free_transfer(r->xfer[i]); r->xfer[i] = NULL; }
    }
    if (r->buf) {
        for (unsigned int i = 0; i < r->nxfers; i++)
            if (r->buf[i]) { free(r->buf[i]); r->buf[i] = NULL; }
    }
    free(r->xfer); r->xfer = NULL;
    free(r->buf);  r->buf  = NULL;
    r->nxfers = 0;
}

void rx888_close(rx888_t *r) {
    if (!r) return;
    rx888_stop(r);
    if (r->h) {
        if (r->claimed) (void)libusb_release_interface(r->h, r->ep.iface);
        libusb_close(r->h);
    }
    if (r->ctx) libusb_exit(r->ctx);
    free(r);
}
