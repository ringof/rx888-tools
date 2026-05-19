/*
 * librx888 — minimal streaming library for the RX888 mk II
 *
 * v0.0: lifted from rx888_stream.c. Same wire protocol, same defaults,
 * same threading model. Only the output target changes: instead of
 * writing int16 samples to stdout, the writer thread invokes a
 * user-supplied callback.
 *
 * Architecture is intentionally narrow for v0.0:
 *   - One device per process (no multi-device scan).
 *   - Configuration is set once at open time; no runtime setters.
 *   - Sample callback runs on librx888's writer thread; the consumer
 *     must hand off to its own ring/queue and return promptly.
 *
 * Thread model: open() blocks. start() spawns two threads (libusb
 * event handler + writer) and returns. The user callback is invoked
 * from the writer thread. stop() joins both. close() frees.
 *
 * Copyright 2026 Free Software Foundation, Inc.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LIBRX888_H
#define LIBRX888_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct rx888 rx888_t;

/* Live counters, snapshot via rx888_get_stats().
 *
 * The "short transfer" fields track DMA commits whose actual_length is
 * less than the configured full transfer size (expected_xfer_bytes).
 * rx888-firmware uses GPIF-side PPS injection to force such a commit on
 * the rising edge of PPS; the bytes between consecutive short transfers
 * therefore equal the number of ADC samples (x2 for int16) produced in
 * one PPS interval. With stock firmware that does not force commits,
 * short_xfers stays at zero on healthy USB.
 */
typedef struct {
    unsigned long long ok_xfers;            /* completed bulk transfers */
    unsigned long long bad_xfers;           /* errored / cancelled / submit failures */
    unsigned long long bytes_out;           /* cumulative bytes delivered to callback */
    unsigned int       in_flight;           /* outstanding USB transfers */
    unsigned long long last_cb_ms;          /* monotonic ms of most recent callback */

    /* Transfer-length classification. */
    unsigned int       expected_xfer_bytes; /* configured full transfer length */
    unsigned long long full_xfers;          /* transfers at expected length */
    unsigned long long short_xfers;         /* shorter than expected (incl. synthetic) */
    unsigned long long zero_xfers;          /* zero-length transfers (should be 0) */
    unsigned int       min_actual_len;      /* smallest non-zero actual_length observed */
    unsigned int       max_actual_len;      /* largest actual_length observed */

    /* PPS window byte counting. */
    unsigned long long bytes_in_window;     /* bytes since last short transfer */
    unsigned long long last_window_bytes;   /* bytes in most recent completed PPS window */
} rx888_stats_t;

/*
 * Sample callback. Invoked from librx888's writer thread.
 *
 *   samples   pointer to int16 samples (real, not IQ).
 *   nsamples  number of int16 samples (NOT bytes).
 *   user      opaque pointer passed to rx888_start().
 *
 * The callback MUST return promptly. Long work blocks the writer
 * thread, which then blocks the libusb callback queue, which causes
 * USB overruns. Hand off to a ring buffer and return.
 *
 * The samples buffer is owned by librx888 and is only valid for the
 * duration of the callback. Copy what you need.
 */
typedef void (*rx888_sample_cb_t)(const int16_t *samples,
                                  size_t nsamples,
                                  void *user);

/*
 * PPS event. Delivered to the registered rx888_pps_cb_t once per
 * closed PPS-window — including synthetic shorts when debug mode is
 * enabled. Fired from the writer thread, after the closing short
 * transfer's samples have been handed to the sample callback.
 *
 *   event_id          monotonic counter, 0-based, reset at rx888_start().
 *   host_monotonic_ms CLOCK_MONOTONIC milliseconds at the moment the
 *                     window closed on the host. Useful for host-side
 *                     jitter analysis; not a UTC time.
 *   bytes_since_prev  bytes accumulated in the just-closed window.
 *                     For a healthy 1 Hz PPS this is fs*2 bytes
 *                     (int16 real samples).
 *   sample_index      bytes_out/2 at the boundary — i.e. the index of
 *                     the first sample AFTER the PPS edge. This is
 *                     the offset to use when emitting a GNURadio
 *                     rx_time stream tag for the corresponding UTC
 *                     second.
 */
typedef struct {
    uint64_t event_id;
    uint64_t host_monotonic_ms;
    uint64_t bytes_since_prev;
    uint64_t sample_index;
} rx888_pps_event_t;

/*
 * PPS event callback. Invoked from librx888's writer thread, on the
 * same thread that runs the sample callback. Must return promptly —
 * blocking the writer blocks the libusb event queue. Hand off to a
 * ring buffer / queue and return.
 *
 * The event pointer is only valid for the duration of the callback;
 * copy what you need.
 */
typedef void (*rx888_pps_cb_t)(const rx888_pps_event_t *e, void *user);

/*
 * Register a PPS event callback. Pass NULL to disable.
 *
 * Intended use: call after rx888_open() and before rx888_start(); the
 * pthread_create() in rx888_start() then provides the happens-before
 * barrier for the writer thread to see the callback. Replacing the
 * callback while a stream is running is not race-free in this
 * implementation; stop, set, restart if you need that.
 *
 * Safe to call with a NULL handle (no-op).
 */
void rx888_set_pps_callback(rx888_t *r, rx888_pps_cb_t cb, void *user);

/*
 * Configuration. Filled by the caller before rx888_open().
 *
 * samplerate    Hz. Firmware-supported values: 32000000, 135000000.
 *               Other values are passed through to STARTADC; whether
 *               they work depends on firmware variant.
 * att           DAT-31 attenuator raw register, 0..63 (half-dB steps).
 *               0 = 0 dB, 63 = 31.5 dB.
 * gain          AD8370 VGA raw gain code, 0..127. Combined with
 *               gain_high in the wire encoding.
 * gain_high     0 = low-gain range (-11..+17 dB),
 *               1 = high-gain range (+6..+34 dB).
 * dither        0/1. ADC dither GPIO.
 * randomizer    0/1. ADC output-randomization GPIO. If on, samples
 *               must be un-randomized; set fixup_samples accordingly.
 * fixup_samples 0/1. If 1, librx888 un-randomizes int16 samples in
 *               the writer thread before the callback. Must match
 *               what the device is doing.
 * firmware_path Optional path to FX3 firmware image. If non-NULL and
 *               the device is in bootloader mode, it'll be uploaded.
 *               If NULL, the device must already be loaded.
 *
 * The fields below are tuning knobs. Call rx888_config_init_default()
 * before mutating to get sane values; rx888_open() rejects a struct
 * with queue_depth, req_packets, or ctrl_timeout_ms set to zero.
 *
 * queue_depth                Concurrent in-flight USB transfers. Default 32.
 * req_packets                Transfer size in USB packets. Default 1024.
 * ctrl_timeout_ms            Vendor control-transfer timeout. Default 5000.
 * stream_timeout_ms          Bulk transfer timeout, 0 = infinite (default).
 * watchdog_ms                No-data watchdog; 0 disables. Default 3000.
 * debug_synthetic_pps_every  0 = off (default). When >0, every Nth completed
 *                            transfer is classified as if it were a PPS-aligned
 *                            short commit. Real data is unaffected. For
 *                            exercising the short-transfer detector end-to-end
 *                            against firmware that does not (yet) force commits.
 *                            Requires a real device; see debug_no_device for
 *                            a fully hardware-free alternative.
 * debug_no_device            0 = normal (default). When 1, rx888_open()
 *                            succeeds without any USB device present.
 *                            rx888_start() generates zero-valued samples at
 *                            samplerate, pacing the callbacks in real time.
 *                            Combine with debug_synthetic_pps_every to drive
 *                            the PPS callback path in CI or Docker without
 *                            hardware. Transfer size is req_packets × 512 B
 *                            (USB HS bulk max packet).
 */
typedef struct {
    unsigned int samplerate;
    unsigned int att;
    unsigned int gain;
    int          gain_high;
    int          dither;
    int          randomizer;
    int          fixup_samples;
    const char  *firmware_path;

    unsigned int queue_depth;
    unsigned int req_packets;
    unsigned int ctrl_timeout_ms;
    unsigned int stream_timeout_ms;
    unsigned int watchdog_ms;
    unsigned int debug_synthetic_pps_every;
    int          debug_no_device;
} rx888_config_t;

/*
 * Populate cfg with library defaults (32 MS/s, no gain, no att, etc.
 * plus the tuning knobs above). Callers should use this and then
 * override only the fields they care about.
 */
void rx888_config_init_default(rx888_config_t *cfg);

/*
 * Open the device, upload firmware if needed, claim the interface,
 * apply the configuration. Does NOT begin streaming.
 *
 * Returns 0 on success. On error returns a negative libusb error
 * code (LIBUSB_ERROR_*). Use rx888_strerror() to format.
 *
 * On success *out points to a heap-allocated handle that must be
 * released with rx888_close(). On error *out is set to NULL.
 */
int rx888_open(rx888_t **out, const rx888_config_t *cfg);

/*
 * Begin streaming. Spawns the libusb event-handler thread and the
 * writer thread. The user callback will start firing shortly after
 * this returns.
 *
 * Returns 0 on success, negative libusb error otherwise.
 */
int rx888_start(rx888_t *r, rx888_sample_cb_t cb, void *user);

/*
 * Stop streaming.  Cancels in-flight transfers; joins the event and
 * writer threads when called from a thread other than the writer.
 * After stop() returns, the user callback will not be invoked again.
 *
 * Safe to call multiple times.
 *
 * Safe to call from inside the user callback.  In that case stop()
 * sets the stop flag, cancels transfers, signals the writer's queue
 * to wake, and returns immediately.  The writer thread exits when
 * the callback returns; the event thread exits when it next checks
 * the flag.  The caller MUST then invoke rx888_close() (or another
 * rx888_stop()) from a different thread to finish teardown —
 * otherwise the streaming threads and their resources stay alive
 * until process exit.
 */
void rx888_stop(rx888_t *r);

/*
 * Release all resources. Implies rx888_stop() if streaming is active.
 */
void rx888_close(rx888_t *r);

/*
 * Snapshot live counters. Safe to call from any thread while
 * streaming. Fields are read atomically but not as a coherent group;
 * use only for monitoring, not for invariants.
 */
void rx888_get_stats(const rx888_t *r, rx888_stats_t *out);

/*
 * Returns 1 while the stream is active, 0 once librx888 has
 * internally stopped streaming (NO_DEVICE, watchdog fired, libusb
 * error, or rx888_stop() was called).  Before rx888_start() the
 * return value is also 1 — call after start() to be meaningful.
 *
 * Callers should poll this from their main loop to notice when the
 * library has decided to stop on its own, since stop() does not yet
 * have a notification port.
 */
int rx888_is_running(const rx888_t *r);

/*
 * Format a libusb error code as a human-readable string.
 * Returned pointer is to static storage; do not free.
 */
const char *rx888_strerror(int err);

/*
 * Library version string (compile-time constant).
 */
const char *rx888_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRX888_H */
