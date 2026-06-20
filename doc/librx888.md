# librx888

Minimal C streaming library for the RX888 / RX888mk2.  Owns the libusb
context, FX3 firmware upload, control-plane configuration, and the
transfer/event/writer thread machinery that delivers `int16` real samples
to a user-supplied callback.

`rx888_stream` and (planned) `gr-rx888` are both thin consumers of this
library.  Same code path, one place to fix bugs and add features.

---

## Status

**Version:** 0.0.1 (single device per process; no runtime setters)

**License:** GPL-3.0-or-later (because it links the GPL-2.0-or-later
`ezusb.c` upstream).

**ABI:** No stability guarantees yet — public API is small and explicit
(see `nm -D --defined-only librx888.so`).  The CI workflow asserts the
exact set of exported `T rx888_*` symbols on every build.

---

## API summary

```c
#include <librx888.h>

/* Lifecycle */
void rx888_config_init_default(rx888_config_t *cfg);
int  rx888_open (rx888_t **out, const rx888_config_t *cfg);
int  rx888_start(rx888_t  *r,   rx888_sample_cb_t cb, void *user);
void rx888_stop (rx888_t  *r);
void rx888_close(rx888_t  *r);

/* Diagnostics */
void        rx888_get_stats(const rx888_t *r, rx888_stats_t *out);
int         rx888_is_running(const rx888_t *r);
const char *rx888_strerror(int err);
const char *rx888_version(void);

/* Planned (pps_integrity) */
size_t      rx888_get_transfer_bytes(const rx888_t *r);
```

Full type and field documentation lives in
[`include/librx888.h`](../include/librx888.h).

> **Planned addition.** `rx888_get_transfer_bytes()` is a one-line
> read-only accessor returning `buf_bytes` (`req_packets * max_packet`,
> the size of a full bulk transfer). It is **not implemented yet** — it
> arrives with the `pps_integrity` tool, which needs the full-transfer
> size to distinguish short PPS-marker transfers. The value is only
> populated after `rx888_start()`; the getter returns 0 before then. See
> [`doc/pps_integrity.md`](pps_integrity.md). When it lands it becomes
> the only public-API change for that feature and must be added to the
> exported-symbol assertion in CI.

### Typical sequence

```c
rx888_config_t cfg;
rx888_config_init_default(&cfg);
cfg.firmware_path = "/usr/local/share/rx888_tools/firmware/SDDC_FX3.img";
cfg.samplerate    = 32000000;

rx888_t *r = NULL;
int rc = rx888_open(&r, &cfg);
if (rc != 0) { fprintf(stderr, "open: %s\n", rx888_strerror(rc)); return 1; }

rc = rx888_start(r, my_sample_cb, my_userdata);
if (rc != 0) { rx888_close(r); return 1; }

/* main thread does whatever it wants — librx888 owns its own threads */

rx888_close(r);
```

---

## Threading model

```
  caller thread                librx888 owns:
  ┌──────────────┐           ┌─────────────────────┐
  │ rx888_open() │──────────▶│ libusb context      │
  │ rx888_start()│           │ event_thread        │ poll libusb,
  │              │           │ writer_thread       │ deliver samples
  │ rx888_stop() │──────────▶│ control mutex       │
  │ rx888_close()│           └─────────────────────┘
  └──────────────┘                   │
        ▲                            │ user callback
        │ (sample data via cb)       ▼
        └────────────── caller-supplied function
```

- **event_thread** drives `libusb_handle_events_timeout_completed` and
  the no-data watchdog.
- **writer_thread** dequeues completed transfers, runs optional sample
  fixup, invokes the user callback, and resubmits the transfer.
- **The user callback runs on the writer thread.**  It must return
  promptly — long work blocks the queue and causes USB overruns.  Hand
  off to your own ring buffer / GR scheduler / file write and return.
- The sample buffer is owned by librx888 and is only valid for the
  duration of the callback.  Copy what you need.

`rx888_stop` cancels in-flight transfers, joins both threads, and tells
the device to stop.  Safe to call multiple times.

---

## Configuration

`rx888_config_t` carries everything `rx888_open` needs.  Always
initialise via `rx888_config_init_default()`; `rx888_open` rejects a
zero-initialised struct so a `calloc`'d config can't accidentally
start the streamer with an unsigned-zero queue depth.

| Field               | Default     | Notes                                       |
|---------------------|-------------|---------------------------------------------|
| `samplerate`        | 32 000 000  | Hz; firmware supports 32M and 135M.         |
| `att`               | 0           | DAT-31 attenuator, 0..63 (half-dB steps).   |
| `gain`              | 0           | AD8340 VGA, 0..127.                         |
| `gain_high`         | 1           | 1 = high-gain range, 0 = low.               |
| `dither`            | 0           | ADC dither GPIO.                            |
| `randomizer`        | 0           | ADC output randomizer GPIO.                 |
| `fixup_samples`     | 0           | Un-randomize samples before callback.       |
| `firmware_path`     | NULL        | Optional FX3 image; uploaded if in DFU.     |
| `queue_depth`       | 32          | Concurrent in-flight USB transfers.         |
| `req_packets`       | 1024        | Transfer size in USB packets.               |
| `ctrl_timeout_ms`   | 5000        | Vendor control-transfer timeout.            |
| `stream_timeout_ms` | 0           | Bulk transfer timeout, 0 = infinite.        |
| `watchdog_ms`       | 3000        | No-data watchdog; 0 disables.               |

---

## Stats

```c
rx888_stats_t s;
rx888_get_stats(r, &s);
//   s.ok_xfers   completed bulk transfers
//   s.bad_xfers  errored / cancelled / submit failures
//   s.bytes_out  cumulative bytes delivered to callback
//   s.in_flight  outstanding USB transfers
//   s.last_cb_ms monotonic ms of most recent callback
```

Safe to call from any thread.  Fields are read atomically but not as a
coherent snapshot — use only for monitoring, not for invariants.

---

## Linking

The library installs a pkg-config file:

```sh
pkg-config --cflags --libs librx888
# -I/usr/local/include -L/usr/local/lib -lrx888
```

Or just `-lrx888` if `librx888.so` is on the linker path.  It pulls
`libusb-1.0` transitively via `Requires.private`.

The in-tree build uses `$ORIGIN` rpath so `./rx888_stream` finds
`./librx888.so` without `LD_LIBRARY_PATH`.

---

## Limits and non-goals (v0.0)

- One device per process.  No multi-device enumeration.
- Configuration is set at `rx888_open` time only.  No runtime
  `set_gain` etc.  (Adding setters is straightforward; the control
  endpoint already has a mutex; it just hasn't been needed yet.)
- No automatic stream recovery after `LIBUSB_TRANSFER_NO_DEVICE` or
  watchdog timeout — both currently terminate the stream.  The
  `gr-rx888` plan describes the recovery layer that will eventually
  expose `rx888_device::restart()` here; see
  [`doc/gr-rx888/stream-division-of-recovery.md`](gr-rx888/stream-division-of-recovery.md).
- VHF / R82XX tuner path is not exercised — current ringof
  `ExtIO_sddc` firmware doesn't include it.
