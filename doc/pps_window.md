# PPS-window sample-slip detector

A host-side instrument in `librx888` that counts bytes between
*short* USB bulk transfers — transfers shorter than the configured
buffer size. With firmware that forces a DMA commit on a hardware
PPS edge (rx888-firmware GPIF-side PPS injection), each PPS interval
arrives as one short transfer; the byte count between consecutive
short transfers equals one second of PCLK edges. A deficit vs the
configured sample rate localises an ADC→GPIF silent drop — the one
failure mode the existing `ok_xfers` / `bad_xfers` / `bytes_out`
counters cannot see.

This document covers:

1. **As-is** — what the detector tells you today, against any firmware
2. **With PPS firmware** — how the same code becomes a sample-slip
   detector and a calibration aid for the firmware feature itself
3. **Future** — how this can grow into a timing side-channel that
   delivers PPS/UTC to interested consumers without ever touching
   the IQ sample stream

---

## Background: what the existing counters can and can't see

`rx888_stats_t` already carries:

| Counter      | Catches                                                 |
|--------------|---------------------------------------------------------|
| `ok_xfers`   | Completed USB bulk transfers                             |
| `bad_xfers`  | Errored / cancelled / submit-failed transfers           |
| `bytes_out`  | Cumulative bytes delivered to the user callback          |
| `in_flight`  | Outstanding USB transfers                                |
| `last_cb_ms` | Monotonic ms of most recent callback                     |

Between these and the FX3 watchdog, every failure mode *except one* is
either reported as an error or causes the stream to stop:

- USB-wire errors → `bad_xfers`
- Device disconnect → `LIBUSB_TRANSFER_NO_DEVICE`, stream halts
- Host-side back-pressure (slow consumer) → FX3 NAKs → flow control
  propagates to the GPIF FIFO
- libusb queueing → bounded by `xfer_queue` depth; overflow trips
  `bad_xfers` and stops the stream

**The one silent path is the GPIF FIFO itself.** The state machine
samples on PCLK; if PCLK edges are lost (signal-integrity event,
clock glitch, ADC frontend issue), the FX3 doesn't know samples
were supposed to exist. No counter increments. The data stream has
N samples instead of N+k and nothing in the host stack can tell.

The PPS-window detector closes that gap.

---

## 1. Using the detector as-is (no firmware support yet)

Stock firmware never short-commits. `short_xfers` stays at zero
for the duration of any healthy capture. That is *itself* the
useful result on current firmware: the detector silently confirms
the noise floor.

### Fields added to `rx888_stats_t`

```c
unsigned int       expected_xfer_bytes; /* configured full transfer length */
unsigned long long full_xfers;          /* transfers at expected length */
unsigned long long short_xfers;         /* shorter than expected */
unsigned long long zero_xfers;          /* zero-length transfers (should be 0) */
unsigned int       min_actual_len;      /* smallest non-zero actual_length */
unsigned int       max_actual_len;      /* largest actual_length */
unsigned long long bytes_in_window;     /* bytes since last short transfer */
unsigned long long last_window_bytes;   /* bytes in most recent closed window */
```

### What it tells you today

Run a sustained capture and inspect the final stats:

```sh
./rx888_stream -v -f firmware/SDDC_FX3.img -s 32000000 > /dev/null 2>stream.log
# … run for N seconds, ^C …
tail -3 stream.log
```

A healthy stream on stock firmware should show:

- `bad=0` throughout
- `short=0` throughout
- `bytes_out / elapsed_s ≈ samplerate × 2`
- Per-second MiB/s line stable to within rounding

A determinism check: run two captures of equal duration and compare
final `bytes_out`. They should agree to within one transfer
(`expected_xfer_bytes`). If they don't, the host-side instrument
has noise that needs investigation before any short-transfer
signal can be trusted as evidence of a real drop.

Documented hardware-baseline observation (32 MS/s, 1 MiB transfers,
30 s captures):

| Run | ok | full | short | bytes_out | MiB/s |
|-----|----|------|-------|-----------|-------|
| 1   | 1783 | 1783 | 0 | 1869 824 000 | 61.0 |
| 2   | 1784 | 1784 | 0 | 1870 872 576 | 61.0 |

Δ = 1 transfer = one boundary effect. Instrument is calibrated.

### Synthetic-PPS debug mode

To exercise the detection path end-to-end against stock firmware,
`rx888_stream` exposes:

```
--debug-synth-pps <N>
```

When `N > 0`, every Nth completed transfer is classified as a
forced short. Real sample data is unaffected — only the
classifier sees the synthetic event. This lets you verify the
detector wiring works *before* relying on it.

Example (32 MS/s, 1 MiB transfers → ~61 transfers/sec, so
N=64 gives ~1 synthetic short per second, mimicking PPS cadence):

```sh
./rx888_stream -v -f firmware/SDDC_FX3.img -s 32000000 \
    --debug-synth-pps 64 > /dev/null 2>synth.log
grep "short=" synth.log | tail -5
```

Documented synthetic observation (29 s capture, N=64):

- `short=27` (expected 64/61 × 29 = 30.4; observed slightly low
  because boundary effects clip a partial window at start)
- `last_window=67 108 864` = exactly `64 × 1 MiB`. Window byte
  arithmetic is bit-exact.
- `full + short == ok` everywhere.
- No throughput penalty from synthetic mode.

### What "I'm not slipping samples" looks like, today

Combine all three observations and you can say something concrete:

> Over a sustained capture at sample rate `fs`,
> `bytes_out / elapsed_s ≈ fs × 2`, `bad_xfers == 0`,
> `short_xfers == 0`, and the per-second byte count is stable.
> The host-side stack delivered every sample the FX3 sent.

That does *not* prove the FX3 didn't lose samples to a GPIF FIFO
overrun or PCLK glitch upstream of the USB endpoint — only that
the path from USB endpoint to user callback is honest. The
remaining gap is what firmware support closes.

---

## 2. As an instrument for firmware PPS evaluation

The rx888-firmware design forces a DMA commit on the rising edge
of a PPS signal observed on a GPIF CTL pin. The commit is performed
by the GPIF state machine itself, atomic with the PCLK edge — no
ARM intervention. The current DMA buffer is committed mid-fill, so
the USB host sees one short transfer per PPS event. **No data is
injected.** Every byte in the sample stream is pristine ADC output;
the marker lives entirely in the DMA framing.

### Mapping the firmware behaviour onto host-side observables

| Firmware event                          | Host-side observable                  |
|-----------------------------------------|---------------------------------------|
| Normal GPIF fill                         | Full-length transfer, `full_xfers++` |
| PPS rising edge while filling buffer     | Short transfer, `short_xfers++`,      |
|                                          | `last_window_bytes` snapshots the     |
|                                          | byte count since the previous short   |
| GPIF FIFO overrun                        | Caught by firmware `0xB7` GETSTATS    |
|                                          | (separate counter, future work)       |
| ADC→GPIF silent PCLK loss                | `last_window_bytes < fs × 2`          |

### The check that makes this an instrument

For every closed window, assert:

```
last_window_bytes == fs × 2  (bytes per second, int16 real)
```

Any negative deviation = PCLK edges were lost between the ADC and
the GPIF state machine in that 1-second interval. Magnitude in
samples = `fs - last_window_bytes / 2`. Time resolution = 1 s
(by PPS cadence); cumulative count is exact regardless of how
often the host polls.

### Evaluating firmware correctness

The same detector that catches sample slips also calibrates the
PPS injection mechanism itself. Expected behaviours when firmware
PPS support lands:

- **PPS present, no slips:** `short_xfers` increments at exactly
  1 Hz; `last_window_bytes == 2 × fs` every second; `full + short == ok`.
- **PPS absent (no source connected):** `short_xfers == 0`;
  detector silently goes idle. The instrument never produces a
  false positive in the absence of a marker.
- **Marker injected at wrong cadence:** `last_window_bytes` is
  stable but not equal to `2 × fs`. Indicates firmware-side PPS
  edge detection or commit timing is off — the same instrument
  measures it.
- **Marker emitted by something other than PPS:** `short_xfers`
  ticks but at the wrong rate, or `last_window_bytes` varies
  wildly. Diagnostic: the firmware is committing for reasons
  other than the PPS edge.

The detector therefore serves as both *user-facing slip detection*
and *firmware-developer regression check* for the PPS feature.

### Cross-check via `0xB7` GETSTATS (planned)

Once the firmware exports a `pps_commits` counter alongside its
existing `0xB7` GETSTATS scaffolding, the driver should compare
device-reported PPS-commit events against host-observed short
transfers:

```
pps_commits (from firmware) == short_xfers (from librx888)
```

Any divergence flags a misclassification — for example, a short
transfer caused by something other than a PPS commit, or a PPS
commit that didn't manifest as a short transfer at the host. This
turns the detector from "trust the short-transfer signal" to
"verified short-transfer signal."

This driver-side work is deferred until the firmware counter
exists. The instrument as-shipped does not require it.

---

## 3. Toward a timing side-channel

The detector already has the information needed to expose a
*timing subband*: a stream of PPS event records, separable from
the IQ data, available to consumers that want UTC alignment
without burdening consumers that don't.

### Design constraints

- **No modification to the IQ data plane.** Sample bytes remain
  pristine. The PPS subband is metadata, not payload.
- **Pull, not push.** Consumers subscribe to the side channel
  explicitly. Default behaviour is unchanged: short transfers
  are silently classified for drop detection only.
- **Opt-out costs nothing.** A consumer that ignores the side
  channel pays zero CPU and zero protocol overhead.

### Shape of the API extension (sketch, not committed)

```c
/* Optional PPS event sink. NULL = drop events on the floor
 * (default; matches current behaviour). */
typedef struct {
    uint64_t event_id;            /* monotonic counter, 0-based */
    uint64_t host_monotonic_ms;   /* when librx888 saw the short xfer */
    uint64_t bytes_since_prev;    /* last_window_bytes at this event */
    uint64_t sample_index;        /* bytes_out/2 at this event */
} rx888_pps_event_t;

typedef void (*rx888_pps_cb_t)(const rx888_pps_event_t *e, void *user);

void rx888_set_pps_callback(rx888_t *r,
                            rx888_pps_cb_t cb, void *user);
```

The classifier already runs on the writer thread; emitting a small
struct to a registered callback on each closed window is
near-zero-cost.

### What ka9q-radio (and similar) could do with it

Subscribe to the PPS callback, package each event as a multicast
record on its own subband (separate group from the IQ subband), and
publish:

- `event_id` — sequence number; consumers detect dropped PPS records
- `sample_index` — exact IQ-stream offset of this PPS edge
- `host_monotonic_ms` — local clock at observation, for jitter analysis
- `bytes_since_prev` — confirms the sample count over the prior 1 s

A consumer that wants UTC-aligned spectrograms reads the PPS
subband and the IQ subband and aligns them by `sample_index`.
A consumer that doesn't care about timing reads only the IQ subband
and pays no cost. Neither path touches the other.

### What a UTC-bearing PPS payload would look like (further future)

The current GPIF design injects no data — the marker is purely DMA
framing. UTC time-of-day, when available, would be carried *out of
band* via `0xB7` GETSTATS (or a parallel control endpoint), keyed
to the same `event_id`. The IQ stream remains untouched.

Sequence:

1. PPS edge → GPIF commit → librx888 sees short transfer →
   `rx888_pps_event_t{event_id=N, ...}` emitted.
2. Driver (or higher consumer) polls firmware GETSTATS for
   `pps_event_N_utc_us` if firmware tracks it.
3. Consumer correlates `event_id` ↔ UTC time-of-day ↔ host monotonic ms.

ka9q-radio gets its UTC alignment. The IQ stream gets nothing
glued on. Consumers who don't need UTC don't pay for it.

---

## Honest residuals

The detector as-built does not catch:

- **Bit errors in samples that weren't dropped.** Corruption is
  a different problem; would need an ADC-side parity or known-test-pattern
  injection mechanism.
- **LTC2208 internal failures upstream of DCO.** Extraordinarily
  rare; would also corrupt rather than drop.
- **PPS source itself being inaccurate.** The detector trusts the
  PPS edge. A free-running 1 Hz oscillator that drifts will look
  identical to a PCLK that drifts in the opposite direction. For
  serious metrology, a GPS-disciplined PPS is essential; for
  drop detection, any reasonably stable 1 Hz suffices.
- **PCLK extra-edge events.** Glitches that *add* samples would
  show `last_window_bytes > fs × 2`. The detector reports them
  as easily as deficits; what matters is treating both directions
  as anomalies.

---

## See also

- [`librx888.md`](librx888.md) — public API
- [`rx888_stream.md`](rx888_stream.md) — CLI consumer
- [`src/pps_audit.h`](../src/pps_audit.h) — classification logic
- [`tests/pps_audit_test.c`](../tests/pps_audit_test.c) — unit tests
