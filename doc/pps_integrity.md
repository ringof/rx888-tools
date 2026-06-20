# `pps_integrity` — long-duration PPS marker fidelity tool

> Refined from `PLAN_PPS_INTEGRITY.md` against the current tree, with the
> open questions resolved, then implemented as `src/pps_integrity.c`.
> See "Decisions" below for what changed from the original draft.

## Problem

The PPS in-band marker (ExtIO_sddc #125) injects short USB bulk
transfers as 1 Hz delimiters in the ADC sample stream. We need
empirical proof that short transfers correlate 1:1 with the marker
GPIO rising edges — no spurious shorts, no missed markers — across all
production sample rates and over multi-hour runs.

A synchronous single-threaded bulk reader works at 8/16 MSPS but
fails at 32+ MSPS: it can't drain 64+ MB/s, causing cascading PIB
overflows that destroy marker detection. librx888's async I/O
(multiple in-flight transfers, event + writer threads) handles all
rates cleanly, so the tool is built on librx888 rather than a bespoke
reader.

## Tool

```
pps_integrity [hours] [--rate MSPS] [--firmware FILE] [-v]   # default: 4 hours, 16 MSPS
```

`--rate` accepts fractional MSPS (e.g. `--rate 129.6`) so the test can
match production rates such as ka9q-radio's 129.6 Msps exactly; it is
rounded to the nearest Hz and passed to `STARTADC`.

Standalone binary in rx888-tools, statically linked against
`librx888.a`. Streams at the requested rate while toggling the marker
GPIO at 1 Hz, verifying every short transfer correlates with a GPIO
edge.

## Decisions (resolving the draft's open questions)

1. **Full-transfer size** — added a one-line read-only getter to
   librx888 (`rx888_get_transfer_bytes()`) rather than runtime-learning
   or hard-coding. The value `buf_bytes = req_packets * ep.max_packet`
   is already computed and stored in `struct rx888`, set in
   `rx888_start()`. Exposing it is the smallest correct API surface, is
   robust to USB 2.0/3.0 negotiation and any `req_packets`, and is
   useful to any consumer that cares about transfer boundaries.
   **Caveat:** `buf_bytes` is only populated after `rx888_start()`; the
   getter returns 0 before then, so the tool reads it after `start()`
   and before arming the first GPIO edge.

2. **GETSTATS decoder** — copy the ~25-line decode into
   `pps_integrity.c` rather than refactoring `fx3_stats`. The firmware
   payload layout (`include/`-adjacent `src/fx3_cmd/fx3_stats.h`) is
   the source of truth; the copy must cite it and stay in sync. This
   keeps `pps_integrity` self-contained and avoids dragging the
   `fx3_usb.c` / `fx3_core.c` device lifecycle into the new binary.

3. **GPIO base state** — `librx888.c:503-507` sets GPIO to exactly
   `dither|randomizer` at open (no LED). `GPIOFX3` writes the *full*
   control word, so the tool must OR librx888's base bits back in when
   it toggles, or it would clear dither/randomizer mid-run. The tool
   builds its word from the same cfg it passed:
   `marker_bit | (dither?DITH:0) | (randomizer?RANDO:0)`, optionally
   `| LED_BLUE` for a visible heartbeat. No readback needed; for the
   default config (dither=0, randomizer=0) the base is just the marker
   bit (+ optional LED).

## Marker bit

The marker is GPIO bit 9 of the `GPIOFX3` control word — `OUTXIO9`,
which `include/rx888.h` names `BIAS_VHF` (`0x200`). The firmware
overloads that control bit as the PPS-marker trigger (ExtIO_sddc
\#125). `pps_integrity` defines a named local constant
(e.g. `PPS_MARKER_BIT = BIAS_VHF`) with a comment flagging the alias,
so the overload is explicit and nobody mistakes it for a magic number.

## Architecture

### Two libusb handles

- **Handle 1 (librx888):** `rx888_open()` claims interface 0, manages
  async bulk EP1-IN, and handles STARTADC / STARTFX3 / STOPFX3
  internally. No API changes beyond the getter above.
- **Handle 2 (direct libusb):** opened on a *separate* libusb context
  via `libusb_open_device_with_vid_pid()`. Sends EP0 control transfers
  only — `GPIOFX3` (toggle the marker bit) and `GETSTATS` (firmware
  diagnostic counters). No interface claim needed; EP0 is
  device-level. A separate context keeps these synchronous control
  transfers off librx888's event loop.

### Short-transfer detection

librx888's sample callback delivers
`(const int16_t *samples, size_t nsamples, void *user)` where
`nsamples = actual_length / 2` (`librx888.c:386`). A full USB 3.0
transfer with `req_packets=1024`, `max_packet=1024` is 1 MB = 524288
samples. The expected full size comes from
`rx888_get_transfer_bytes() / 2`. A PPS marker produces a short
transfer with significantly fewer samples.

Detection runs in librx888's writer-thread callback and coordinates
with the main thread through `_Atomic` variables:

```c
if (nsamples < ctx->expected_nsamples - SHORT_MARGIN) {     /* a short transfer */
    if (atomic_exchange(&ctx->expecting_marker, 0))
        atomic_store(&ctx->marker_arrived, 1);              /* first short in window */
    else
        atomic_fetch_add(&ctx->spurious, 1);                /* short with no edge, or a late/extra short */
}
```

State machine (avoids double-counting a marker that spans two short
transfers, or a late short after the window):

- Main thread sets `expecting_marker = 1` *before* the rising edge.
- First short while expecting → `atomic_exchange` clears the flag and
  sets `marker_arrived`; the cleared flag means any further shorts in
  the same second are counted `spurious`.
- Any short while not expecting → `spurious`.

Counting "extra" shorts as spurious is intentional: this is an
integrity test, and a genuine USB hiccup or a doubled marker is a real
finding.

### Marker size, and the blind spot (empirical)

The marker is the **leftover partial buffer** at each PPS second. A
second's worth of samples packs into a whole number of full transfers
plus one partial, and that partial *is* the short transfer we detect.
Its size is therefore:

```
marker_samples ≈ samplerate mod (transfer_bytes / 2)
```

Measured against a 524288-sample full transfer:

| Rate     | `rate mod 524288` | observed marker size |
|----------|-------------------|----------------------|
| 16 MSPS  | 271 360           | ~272 000             |
| 32 MSPS  |  18 432           | ~18k–20k             |
| 64 MSPS  |  36 864           | (predicted ~37k)     |

The value drifts slowly with the ADC-vs-host clock offset (a few ppm).
This is why marker sizes differ per rate — it is expected, not a bug.

It also exposes an **inherent blind spot**: when the remainder drifts
within a buffer-boundary band of `0` or `full`, the partial is either
empty (suppressed by firmware) or indistinguishable from a full
transfer — so *no in-band short can exist*. That was always a design
risk of a short-transfer marker; here it is characterised. The blind
spot is a property of the marker scheme, eliminable only at the source
(e.g. firmware emitting a zero-length packet or a forced minimum-length
delimiter regardless of buffer fill).

A miss is a marker-**timing** event, **not** a data-loss verdict (data
integrity is judged separately, below). Two kinds:

- **blind-spot** — `minxfer < full` (a near-full partial slipped past
  `SHORT_MARGIN`), or `minxfer == full` while the live remainder (last
  good marker) sits within `DANGER_BAND` of a boundary. Inherent to the
  marker scheme; a NOTE.
- **displaced** — otherwise: the skipped flush rolled into an adjacent
  edge (which then flushes ~2×). The delimiter moved one edge and is
  reconstructable in post (the skipped boundary sits one normal remainder
  into the oversized buffer). Reported, but does **not** fail the run.

> Earlier builds tried to label a miss "recovered" vs "lost" from the
> recovery-marker *size* (a `MERGE_PCT` threshold) and later from the
> per-interval continuity deficit. Both proved unreliable: at 64–129 MSPS
> the recovery sizes form one continuous population and marker-detection
> timing jitters by ~a marker's worth, so single-interval deficits fire
> with no real loss. **Data loss is now decided only by the authoritative
> byte-exact produced-vs-delivered check below**, so the miss count is
> purely about marker timing.

`-v` adds a `minxfer` column (smallest transfer that second) so the
classification is auditable line by line.

### Sample-loss accounting

Marker classification answers "is the delimiter there?" — not "did we
lose samples?" That is checked by independent detectors:

1. **Bytes produced vs delivered — the authoritative, clock-independent
   check.** The firmware's `glDMACount` (GETSTATS byte 0) counts the DMA
   buffers the GPIF *produced*. **These are the firmware's small DMA
   buffers (`FW_DMA_BUF_BYTES`, 16 KB on SDDC_FX3), not the host's 1 MB
   USB transfers** — the host aggregates ~64 DMA buffers per transfer, so
   `glDMACount` runs ~64× `ok_xfers`. The comparison must therefore be in
   **bytes**, not buffer counts:
   ```
   produced_bytes  = (glDMACount delta) × FW_DMA_BUF_BYTES
   delivered_bytes = (samples delta)    × 2
   ```
   Both count the same data, so a slow ADC clock lowers *both* equally and
   the difference stays at zero — only real loss makes produced exceed
   delivered. This directly answers "were all DMA buffers delivered?".
   *Caveats:* `glDMACount` resets on `STOPFX3` (the end snapshot is read
   *before* `rx888_stop()`); the counters aren't sampled atomically and
   the pipeline holds transfers in flight, so produced runs ahead by
   ~`in_flight` — the report subtracts `in_flight` (in bytes) and allows a
   few transfers of slack. `FW_DMA_BUF_BYTES` is an asserted firmware
   constant, guarded at runtime (if produced/delivered disagree by a large
   factor the assumption is wrong and the check is marked *indeterminate*
   rather than failing).
2. **librx888 `bad_xfers`** — errored/cancelled USB transfers
   (transport-level loss).
3. **Inter-marker continuity (diagnostic localizer, not the verdict).**
   Samples between consecutive markers vs `(span) × rate`, `rate` = median
   of single-second inter-marker counts. It localizes *when* a deficit
   appears, but marker-detection timing jitters by ~a marker's worth at
   high rates, so an isolated short interval (with the next running long,
   net zero) is **not** loss. It therefore does not fail the run on its
   own — it is reported and **corroborated against detector 1**: a deficit
   that coincides with `produced > delivered` is real; one that doesn't is
   flagged as marker-flush jitter.

**PIB errors** (firmware GPIF overflow) are **polled every second** on
the EP0 handle, so an overflow is localised to the second it happens and
correlated with that interval's continuity result: PIB *with* a deficit
lost host-visible samples; PIB with none did not. The same poll catches
a mid-run device reset (boot-count change) immediately.

### Separating clock offset from loss

`delivered / (samplerate × elapsed)`, as ppm, is the ADC-vs-host **clock
offset combined with any uniform loss** — the two are identical in that
one number. They are separated by the fact that **loss always leaves a
fingerprint** (a `bad_xfer`, a continuity step, or `produced > delivered`)
whereas a **slow clock delivers everything, just at a steadily lower
rate** — a smooth drift with no fingerprints (the ~+0.16 ppm marker-size
drift we observe is exactly this).

So when every loss detector is clear, `produced == delivered` *proves* no
data was dropped, and the rate offset is reported as a pure **ADC clock
calibration** (`ADC clock: ±X ppm`). This assumes a disciplined
(NTP/PTP/GPS) host clock; on a free-running host the ppm is host+ADC
combined. If any detector fires, the offset line instead reads
`Sample budget: … (clock+loss combined)` and the run fails.

The summary prints its own **sensitivity**, which differs by detector:
the continuity diagnostic has a fine floor (`LOSS_TOL`, ~0.1 buffer) but
jitters; the authoritative produced-vs-delivered has a coarser floor (the
in-flight pipeline, a few MB) but is clock-independent and reliable.
Together: produced-vs-delivered is the verdict, continuity localizes.

### What we deliberately do *not* do

No sequence number or known pattern is embedded in the sample stream —
that is a separate RF-injection / TimeSpec idea, out of scope here. The
airtight version of produced-vs-delivered would be a *firmware* "buffers
delivered" (DMA consumer) counter, making the comparison atomic and
buffer-exact; the FX3 SDK can do it (`CY_U3P_DMA_CB_CONS_EVENT`) but adds
a callback per transfer, which needs benchmarking at 64+ MSPS. Until then
the host-side `produced_bytes` (`glDMACount × FW_DMA_BUF_BYTES`) vs
`delivered_bytes` inference (with in-flight slack) is the recommended
approach and is what this tool implements.

### What fails the run

Any confirmed loss — `bad_xfers > 0` or `produced_bytes − delivered_bytes`
beyond the in-flight slack — plus spurious shorts, device resets, early
library stop, or marker-handle control faults. Blind-spot and displaced
misses do not (marker timing only); an uncorroborated continuity dip does
not.

### Main thread — 1 Hz toggle loop

1. `clock_gettime(CLOCK_REALTIME)` — wall-clock timestamp per edge.
2. Rising edge → `GPIOFX3` `PPS_MARKER_BIT | base_gpio` on handle 2,
   set `expecting_marker`.
3. 10 ms dwell → falling edge → `GPIOFX3` `base_gpio`.
4. Check `marker_arrived`; emit the per-second status line.
5. SIGINT → clean shutdown (`rx888_stop`/`rx888_close`, close handle 2)
   + summary.

### Per-second output

```
pps_integrity: starting 0.033 hour run @ 32 MSPS
#time             stat   edges  marks  spur  miss   minxfer
 14:23:01.384521  ok         1      1     0     0      18432
 14:23:02.384892  ok         2      2     0     0      19960
 14:23:03.385201  MISS       3      2     0     1     524288
 14:23:04.385600  ok         4      3     0     1      37120
```

Wall-clock microsecond timestamps so the operator can visually catch
cadence skips. `stat` is `ok`, `BLIND`, or `MISS` (a displaced delimiter —
marker timing only); the `minxfer` column (samples) appears under `-v`.
Here edge 3's marker was displaced into edge 4's oversized flush.

### Final report

```
=== PPS INTEGRITY RESULT ===
Duration:        03:00:00
Sample rate:     64 MSPS (64000000 Hz)
Transfer size:   524288 samples (1048576 bytes)
Edges sent:      10800
Markers seen:    10799
Spurious shorts: 0
Missed markers:  1  (blind-spot: 0, displaced: 1) — marker timing only, see loss below
Samples in:      9474614008 (9.47 Gsa)
DMA buffers:     produced 18.95 GB (1156532 x 16384 B), delivered 18.95 GB, in-flight 0.03 GB, undelivered +0.000 MB (slack 4.19 MB)
USB transfers:   ok=18204 bad=0
Sample loss:     0 continuity dip(s), ~0 samples (diagnostic; NOT corroborated ...)
Loss floor:      continuity 65536 samples (0.12 buffer), largest dip 0 (0.00 buffer); gross floor ~4.19 MB (in-flight)
ADC clock:       +19.330 ppm (lossless: produced==delivered, no USB errors; assumes disciplined host)
PIB errors:      2 total in 2 second(s); 0 coincided with a deficit
Stream faults:   0
Boot count:      unchanged
Result: PASS
```

The headline integrity line is **`DMA buffers: produced ≈ delivered`** (in
bytes) — clock-independent proof no buffer was dropped. With it and
`bad_xfers == 0`, the rate offset is reported as a pure **`ADC clock`**
calibration rather than loss. The `Sample loss` / continuity line is a
diagnostic, flagged corroborated or not by the produced-vs-delivered
check.

**Note on firmware counters:** GETSTATS exposes `dma_count` (glDMACount,
DMA buffers produced — 16 KB each, *not* host transfers), `pib_errors`,
`streaming_faults`, and `boot_count`, but no dedicated
`pps_count`/`pps_fail`, and no DMA-*consumer* (delivered) counter.

### Pass criteria

- **No sample loss** — `bad_xfers == 0` and `produced_bytes − delivered_bytes`
  within the in-flight slack. (Continuity dips are diagnostic, not failing.)
- `spurious_count == 0` — no shorts without a preceding rising edge.
- No device resets (`boot_count` unchanged, no mid-run reset), no
  streaming faults, no early library stop, no marker-handle control
  faults.
- Blind-spot and displaced misses do not fail the run — they are
  marker-timing events; data integrity is decided by the loss detectors
  above. PIB errors are informational unless corroborated by a deficit.

## Implementation

### New file: `src/pps_integrity.c` (~220 lines)

```
Includes: librx888.h, rx888.h, libusb.h, signal.h, time.h, stdatomic.h

struct pps_ctx {
    _Atomic int      expecting_marker;
    _Atomic int      marker_arrived;
    _Atomic uint64_t spurious;
    size_t           expected_nsamples;
};

sample_cb()        — librx888 callback, short-transfer detection
ctrl_write_u32()   — self-contained EP0 vendor-OUT helper (handle 2)
ctrl_read()        — self-contained EP0 vendor-IN helper (handle 2)
decode_stats()     — copy of the fx3_stats GETSTATS decoder
open_ctrl_handle() — second libusb context + handle for EP0
main()             — arg parse, rx888_open/start, read transfer size,
                     toggle loop, summary
```

The EP0 helpers are ~15 lines each (mirroring
`src/fx3_cmd/fx3_usb.c`) and are kept local so the binary does not link
the fx3_cmd core.

### librx888 change (one function)

```c
/* include/librx888.h */
/* Bytes per full bulk transfer (req_packets * max_packet). Valid only
 * after rx888_start(); returns 0 before the transfer ring is sized. */
size_t rx888_get_transfer_bytes(const rx888_t *r);

/* src/librx888.c */
size_t rx888_get_transfer_bytes(const rx888_t *r) {
    return r ? (size_t)r->buf_bytes : 0;
}
```

This is the only public-API addition. Bump the doc comment in
`librx888.h`; no behavioral change.

### Makefile target

```makefile
pps_integrity: $(SRCDIR)/pps_integrity.c $(LIBRX_A) $(LIBRX_HDR)
	$(CC) $(CFLAGS_STREAM) $(LIBUSB_CFLAGS) -I$(INCDIR) \
	    $(SRCDIR)/pps_integrity.c \
	    $(LIBRX_A) $(LIBUSB_LIBS) -lpthread -o $@
```

Note `$(LIBUSB_CFLAGS)` — `CFLAGS_STREAM` does not include it, but
`pps_integrity.c` includes `libusb.h` directly. Static link against
`librx888.a` so there is no `.so` deployment concern. Add
`pps_integrity` to `BINS`, `.gitignore`, and the `install`/`uninstall`
targets.

### Shared code reuse

- **`include/rx888.h`** — protocol constants (`GPIOFX3`, `GETSTATS`,
  `STOPFX3`, GPIO bit definitions including `BIAS_VHF`).
- **librx888 API** — `rx888_open`, `rx888_start`, `rx888_stop`,
  `rx888_close`, `rx888_config_init_default`, `rx888_get_stats`,
  `rx888_is_running`, and the new `rx888_get_transfer_bytes`.
- **GETSTATS wire format** — copied decoder; keep in sync with
  `src/fx3_cmd/fx3_stats.h` (the firmware layout authority).

## Tests

- **Non-HW smoke** (`tests/pps_integrity_smoke.sh`): `--help`, bad
  args, and the no-device path return cleanly. Wire into the `check`
  target alongside the existing smoke tests.
- **HW** (`hw-check`): short runs at each rate confirm `ok` every
  second, zero spurious, zero missed; SIGINT yields a clean partial
  summary.

## Verification

```bash
# Build
make librx888.a pps_integrity

# Smoke at each rate (2 min each)
./pps_integrity 0.033 --rate 8
./pps_integrity 0.033 --rate 16
./pps_integrity 0.033 --rate 32
./pps_integrity 0.033 --rate 64

# All rates: ok every second, zero spurious, zero missed
# Ctrl-C: clean summary with partial data
# Post-test: run `fx3_cmd` GETSTATS to confirm no contamination

# Long run
./pps_integrity 4 --rate 64
```

## Work breakdown

1. Add `rx888_get_transfer_bytes()` to `include/librx888.h` +
   `src/librx888.c`. Rebuild `librx888.a`; confirm existing tests pass.
2. `src/pps_integrity.c`: arg parse, two-handle setup, copied GETSTATS
   decoder, runtime size from the new getter, atomic marker state
   machine, 1 Hz toggle loop with `CLOCK_REALTIME` stamps, per-second
   line, SIGINT summary with GETSTATS delta.
3. Makefile: `pps_integrity` target (with `$(LIBUSB_CFLAGS)`), `BINS`,
   `.gitignore`, `install`/`uninstall`.
4. Tests: `pps_integrity_smoke.sh` in `check`; HW path in `hw-check`.
5. Docs: keep this file current; add a short testplan mirroring
   `doc/rx888_stream_testplan.md`.
