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
pps_integrity [hours] [--rate MSPS] [--firmware FILE] [-q N] [-p N] [-v]   # default: 4 hours, 16 MSPS
```

`--rate` accepts fractional MSPS (e.g. `--rate 129.6`) so the test can
match production rates such as ka9q-radio's 129.6 Msps exactly; it is
rounded to the nearest Hz and passed to `STARTADC`. `-q`/`--queuedepth`
and `-p`/`--reqsize` tune librx888's in-flight buffering and transfer
size — at rates near the USB drain ceiling, more buffering can ride out
drain stalls (see "Throughput ceiling" below).

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
   buffers (`FW_DMA_BUF_BYTES` = 16 KB, confirmed against the SDDC_FX3
   GPIF→DMA config), not the host's 1 MB USB transfers** — the host aggregates ~64 DMA buffers per transfer, so
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

**Where the loss is — inside the FX3, on the DMA→USB drain (firmware-confirmed).**
`glDMACount` increments in the firmware DMA callback on
`CY_U3P_DMA_CB_PROD_EVENT` — i.e. when the **producer** socket commits a
filled buffer *into* the DMA channel, **before** USB consumption. So
`produced > delivered` means buffers that **entered the channel but never
came out the USB consumer socket**: the gap is *inside the chip*, on the
DMA-channel→USB drain, not across the USB wire. This localises the loss
precisely — the producer fill is fine (every lost buffer was filled and
counted); it is the **drain** that loses them. It also **rules out host
backpressure**: a host too slow to drain would (a) show in the no-marker
`stream_soak` control at the same data rate — it does not — and (b) the
loss correlates 26.9× with the *device-internal* buffer-fill phase
(`minxfer`), which the host neither sees nor controls, so it cannot be a
host-delivery artifact.

One accounting nuance follows from the PROD_EVENT semantics: it fires per
buffer *regardless of fill*, so `produced_bytes = glDMACount × 16 KB`
could in principle over-count the *partial* buffers the marker forces.
This cross-checks out as a small secondary term, not the headline: the
**continuity detector is delivered-side** (gaps in *received* samples) and
therefore immune to producer-count rounding, yet independently attributes
~87 MB (397 events × ~110k samples) to real loss in the *same* events the
byte check flags. Two detectors — one producer-side, one consumer-side —
agree the bulk loss is real and ~order-118 MB. A byte-granular produced
counter (see "Takeaways", #2) would close the ~87→118 MB residual cleanly.

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

### Marker injection, not throughput, perturbs the stream (and the loss is anomalously large — root cause OPEN)

Measured on real hardware, 3 h each (sleep inhibited; in-flight-corrected):

| run | rate | short xfers | loss | device |
|-----|------|-------------|------|--------|
| `stream_soak` (no marker) | 64 MSPS | 0 | 0 | clean |
| `stream_soak` (no marker) | 129.6 MSPS | 0 | 0 | clean |
| `pps_integrity` (1 Hz marker) | 129.6 MSPS | 403 | **118 MB = 42.3 ppm** | clean (PIB=0, bad=0, faults=0) |

Two firm conclusions and one open question:

**Firm: it is not a throughput ceiling, it's the marker.** Same rate (129.6),
same duration, healthy device both ways: the bare stream loses nothing, the
marker run loses 42.3 ppm. The streaming path sustains 259 MB/s fine. (The
bare stream is even clean at the *higher*-stress comparison — a drain limit
would fail it first.)

**Firm: the budget alone would have missed it.** With a +41.8 ppm fast ADC
clock and 42.3 ppm loss, the sample-rate budget read **−0.5 ppm** —
pristine. The clock-independent produced-vs-delivered check exposed it (and
prints the *implied* clock, `budget + loss added back`). This is the case
the whole loss-accounting design exists for.

**OPEN: the loss is consistent with a fixable rig artifact — but that is
a hypothesis, not a measured result.** A correct forced buffer commit
should cost ~0 samples (it ships a partial early, then continues). What we
measure instead is loss that is **rare but, when it happens, large**:

- Of 10,800 markers (1 Hz × 3 h), only **397 (3.68 %)** caused a *detected*
  loss event, and those 397 carry essentially all 118 MB (median ~98k
  samples each, ~1 ms). So the headline 42.3 ppm is **not** a per-marker
  cost; it is a rare catastrophic mode. The other 96.3 % did not trip the
  continuity detector — but that detector is jittery (it is demoted to a
  diagnostic for exactly that reason), so "clean" means "below a noisy
  floor," **not** verified zero. The true per-marker floor is unknown from
  this data.
- The device is *pristine* throughout — `PIB = 0`, `bad_xfers = 0`,
  `faults = 0` — i.e. buffers `glDMACount` counts as produced but that
  never become a USB transfer: the loss is in the handoff, not an overflow.

Loss events are **26.9× enriched at buffer boundaries** (`pps_log_stats.py`),
which points at a commit-vs-buffer-completion collision. Two cautions on
how far that carries: the marker *size* distribution across all clean
seconds is ~uniform (≈2.5k–521k samples) — that is **expected** from the
incommensurate PPS-vs-fill phase, **not** itself evidence of anything; and
**43.6 % of loss events are not boundary-adjacent**, so the boundary-race
story does not account for all of them. The leading reading is still
**fixable input conditions rather than the in-band concept**, but it
rests on the suspects below, none yet tested:

The firmware source (reading the actual GPIF config and vendored SDK)
turns the input-condition suspects from speculation into a **confirmed
signal path** — though not yet a confirmed *cause*. The `PPS_CTL_ENABLE=1`
marker commit is performed **entirely in GPIF hardware** (states
`TH0_PPS_COMMIT`/`TH1_PPS_COMMIT`), on an `AUTO_MANY_TO_ONE` channel with
cross-route correct by construction (see the KBA note below — the software
commit-ordering class is eliminated). What the config shows about the CTL
path:

- **Edge quality (confirmed slow).** The PPS reaches the FX3 through a
  100 kΩ series resistor → a ~4 µs RC edge. CTL[2] is sampled on the
  **external ADC clock (129.6 MHz)**, not the faster PIB fabric clock, so
  that edge spans **~500 consecutive sample clocks in the input threshold**
  — 500 edges each able to violate setup/hold. Untested with a
  fast/buffered (<1 clock) edge.
- **No synchronizer, and edge-triggered (confirmed).** The control
  comparator is in **TOGGLE mode** (`GPIF_CONFIG` bit 12) — it fires for
  one clock whenever the *sampled* CTL[2] value differs from the previous
  sample — and it samples CTL[2] **directly**: no intermediate sampling
  state, no 2-flop synchronizer. A metastable sample propagates straight
  into the `PPS_COMMIT` transition. The PPS vs original GPIF waveforms
  differ in exactly two bits — `CTRL_COMP_ENABLE` (bit 0) and
  `CTRL_COMP_TOGGLE` (bit 12) — so the marker is *only* "turn the toggle
  comparator on," riding the raw async edge.
- **DLL: correctly off, not a lever.** `pibClock.isDllEnable = CyFalse`;
  the GPIF is in **sync mode** (`GPIF_CONFIG` bit 8 = 1) clocked by the
  external ADC clock. The DLL exists for *async*-mode input timing, which
  this interface does not use, so disabling it is correct — ruling out the
  one firmware-config lever I had flagged (KBA210733).
- **SDK 1.3.4** (`SDK/fw_lib/1_3_4/`).

So the metastability *mechanism path* is real and firmware-confirmed: a
~4 µs async edge, sampled directly with no synchronizer by a toggle
comparator on a 129.6 MHz clock, driving a state transition — the textbook
setup for a metastable sample to push the state machine into an undefined
state and stall it long enough to lose ~15 DMA buffers (~250 KB, ~1 ms).
**What is still not proven** is that this path is what produces the 397
catastrophic events: we have not captured a metastable stall, and **43.6 %
of loss events are not boundary-adjacent**, which a pure edge-collision
story does not yet explain. The causal test is the edge fix.

So this is **not** a verdict either way — neither that in-band marking is
unworkable nor that it is fixable. With the path confirmed, the experiment
order is now clear and **edge-first**:
(1) **done — 129.6 MSPS baseline = 42.3 ppm** (n = 1, slow-edge resistor);
(2) **fast/buffered edge (lower R)** — directly attacks the ~500-cycle
threshold dwell, the cheapest change and the one most likely to move the
result; (3) **a synchronizer state in the GPIF waveform** — belt-and-
suspenders for any residual metastability once the edge is clean. Both are
**tests that can fail.** Watch the loss and the boundary-enrichment
together: if a clean edge collapses the per-event loss and pulls
`pps_log_stats.py`'s `bnd-enr` toward 1×, the metastability reading holds
and in-band marking becomes viable — and it would then be the *finest*
time source available (the short transfer's length encodes the edge's
sample index sample-exactly, with no firmware machinery), worth recovering.
If the loss persists, or the non-boundary 43.6 % survives, the cause is
something else and the out-of-band MCU latch (buffer-level, plus an
optional byte-offset) stays the fallback. Until (2)/(3) run, that fallback
is the only proven option.

`-q`/`-p` raise in-flight buffering, which *might* let the pipeline ride
out the perturbation; `tests/pps_knob_sweep.sh` measures whether it does.
But the real questions are edge quality and input synchronization, above.

#### Investigated and ruled out: Infineon KBA231382 (commit-buffer failures)

Infineon's KBA *"Handling Commit Buffer Failures Occurred during Video
Transfers using FX3"* (KBA231382, AN75779/UVC reference) describes
`CyU3PDmaMultiChannelCommitBuffer()` returning
`CY_U3P_ERROR_INVALID_SEQUENCE` (error 71) when a CPU-side commit lands
out of the socket order the GPIF fill sequence expects on the two
ping-pong producer sockets `PIB_SOCKET_0`/`PIB_SOCKET_1` — a buffer
silently fails to ship. That looked, on the face of it, like a strong
match for the 26.9× boundary-enrichment: a forced partial commit at an
arbitrary phase committing out of socket order.

**It does not apply to this build.** The firmware source confirms the
RX888 runs `PPS_CTL_ENABLE=1`, in which the marker commit is performed
**entirely in GPIF hardware** (states `TH0_PPS_COMMIT`/`TH1_PPS_COMMIT`):

- There is **no CPU `CommitBuffer()` or `SetWrapUp()`** in the marker path
  at all — the KBA is about CPU-driven commits, so its mechanism cannot be
  the cause here.
- The streaming channel is `CY_U3P_DMA_TYPE_AUTO_MANY_TO_ONE`. Cross-route
  is **correct by construction**: `TH0_PPS_COMMIT` only fires while
  thread 0 is the active producer, `TH1_PPS_COMMIT` only while thread 1 is
  — the state machine cannot commit the "wrong" socket.
- (The `SetWrapUp`-on-AUTO anti-pattern the KBA warns about *does* exist in
  the firmware tree — in `synth_pps.c`, which guesses socket 0 then falls
  back to socket 1 with no return-code recovery — but that path belongs to
  the `PPS_CTL_ENABLE=0` build and **never runs here**.)

So the KBA was worth chasing and is now closed as a cause for this build.
What it eliminates is the *software commit-ordering* class; what remains is
a loss **internal to the GPIF state machine**, with the slow CTL[2] edge
the leading (not the only) suspect. The loss *magnitude* — ~250 KB/event
(~15 DMA buffers, ~1 ms), too large for a clean descriptor swap (µs) — is
**consistent with** a state-machine stall on an indeterminate latch, but
that is an interpretation, not a measurement; we have not captured a stall.
The fast-edge run is the next test and a real test: it may collapse the
per-event loss toward ~1–2 samples (metastability confirmed), or leave it
unchanged (cause is elsewhere — e.g. the AUTO channel not tolerating an
async mid-descriptor commit at all). Either outcome is informative; only
the first one makes in-band marking viable.

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

## Control: `stream_soak`

`stream_soak` is the no-marker control. It runs the *same* data-integrity
instrumentation — effective rate from delivered-samples/elapsed-time,
produced-vs-delivered byte loss, `bad_xfers`, per-second PIB, streaming
faults, device-reset detection — but injects **no** PPS marker. Two uses:

- It isolates the marker mechanism: anything `stream_soak` reports
  (e.g. **short transfers** — which should be zero with no marker
  applied, so each is a pure FX3-partial-commit anomaly, or sample loss)
  is inherent to the streaming path, not caused by the PPS injection.
- It is the natural baseline for the throughput-ceiling work above and
  shares the `-q`/`-p` knobs, so `tests/pps_knob_sweep.sh`-style sweeps
  can be repeated on the bare stream.

A clean control run at a rate where `pps_integrity` also passes confirms
the marker mechanism adds no loss of its own.

## Tests

- **Non-HW smoke** (`tests/pps_integrity_smoke.sh`,
  `tests/stream_soak_smoke.sh`): `--help`, bad args, and the no-device
  path return cleanly. Both are wired into the `check` target.
- **HW** (`hw-check`): short runs at each rate confirm `ok` every
  second, zero spurious, zero missed; SIGINT yields a clean partial
  summary.
- **Log analysis** (`tests/pps_log_stats.py <log> [<log2> ...]`):
  correlates the dip / MISS / spurious events in a `-v` log — the
  marker-position **boundary-enrichment** of dip seconds (the
  commit-vs-buffer-boundary race signature), the post-MISS ~2× merge
  ratio, dip/spur co-occurrence, and temporal clustering. Pass several
  logs (`_baseline`, `_lowR`, `_sync`) for a comparison table; a working
  fix should drop dips/hour **and** collapse the boundary-enrichment
  toward 1×.

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

## Takeaways even if this doesn't work

The in-band marker may not survive the edge experiment. The harness and the
understanding behind it survive regardless: we now have trustworthy host-side
data-quality, dropped-sample, and timing-error instrumentation
(`pps_integrity`, `stream_soak`, `pps_log_stats.py`, the produced-vs-delivered
and clock-vs-loss accounting). The natural next step is firmware features that
turn what these tools currently **infer** into something the device **reports
exactly** — none of which depend on in-band PPS panning out. Feasibility is a
firmware call (FX3 SDK 1.3.4, `AUTO_MANY_TO_ONE` channel, GPIF counter
hardware); these are ranked candidates with the open questions noted.

**Dropped-sample detection (make it exact, not inferred)**

1. **Drop/overflow event counter in GETSTATS** *(cheapest big win)*. The
   firmware knows the exact moment a producer socket has no free buffer (host
   fell behind) — that is when a sample is lost. Count it and expose it (the
   KBA's own ++commit/−−CONS recipe). *Buys:* turns our `glDMACount`-vs-
   delivered inference into a device-reported drop count and retires the
   `INDETERMINATE if off by >4×` guard. Just more GETSTATS fields.
2. **Sample-granular "produced" counter (BYTE_COUNT) in GETSTATS.** Today we
   count 16 KB DMA buffers, so loss resolution is `LOSS_TOL = 65536` samples;
   a byte-granular produced count drops that to ~1 sample. *Buys:*
   produced-vs-delivered becomes sample-exact.
3. **In-band per-buffer sequence / cumulative sample-count header**
   *(most powerful, most invasive)*. A monotonic counter word per buffer
   catches any drop instantly and exactly. *Caveat:* perturbs the zero-copy
   AUTO path and the host must strip it — heavyweight, only if 1–2 fall short.

**Timing error / clock offset (make it directly measurable)**

4. **Free-running device sample counter readable via EP0.** Host reads it and
   stamps its own clock → a `(host_time, device_sample_index)` pair on demand;
   sampled over a run it measures clock drift/offset **directly**. *Buys:*
   removes the clock-vs-loss ambiguity this doc wrestles with.
5. **GPIF counter-capture-on-CTL-edge — the "right" PPS latch.** Instead of
   forcing a *commit* on the PPS edge (what stalls the stream), have the GPIF
   snapshot the sample counter into a register on the edge; the CPU exposes it
   via EP0. **Sample-exact timing, zero stream perturbation**, reusing the same
   CTL[2] wiring. A genuine *alternative* to the in-band marker, worth pursuing
   in parallel regardless of how the edge experiment goes.

**Monitoring / health (cheap GETSTATS additions)**

6. **In-flight depth + high-water mark** — early warning before the host
   actually drops.
7. **USB3 link-recovery (LTSSM recovery) count** — marginal cables/links
   retrain silently; a leading indicator that correlates with transport stalls.
8. **DMA topology self-report (buffer size + count + sockets)** — removes the
   hardcoded `FW_DMA_BUF_BYTES = 16384` and channel-depth assumptions.

**Validation (ground truth for the tools themselves)**

9. **Test-pattern / counter-ramp mode.** A known deterministic stream verifies
   the transport bit-exactly and separates analog/ADC from transport faults.
   *Buys:* validates the loss math against ground truth. *Caveat:* at 259 MB/s
   the CPU cannot synthesize a ramp — this likely belongs to the **ADC's**
   built-in test mode, with firmware enabling it and flagging it in GETSTATS.

If only two get built, **#1 (drop counter)** and **#2 (sample-granular
produced count)** are the highest value-per-effort: small GETSTATS additions
that make the already-shipped host tools authoritative rather than inferential.
**#5 (GPIF counter-capture latch)** is the standout because it chases the same
prize as the in-band marker — sample-exact device timing — without perturbing
the stream at all.

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
