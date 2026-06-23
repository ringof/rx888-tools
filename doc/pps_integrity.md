# `pps_integrity` — long-duration PPS marker fidelity tool

> Refined from `PLAN_PPS_INTEGRITY.md` against the current tree, with the
> open questions resolved, then implemented as `src/pps_integrity.c`.
> See "Decisions" below for what changed from the original draft.

> **Status (resolved):** the in-band PPS marker is **byte-lossless** —
> confirmed over a 3 h run by the byte-exact producer/consumer drain
> counters (backlog net 0, leak bound < 6 ppb). The "~42 ppm loss" we chased
> for ~3 days (2026-06-20 → 06-23) was a measurement artifact: `glDMACount ×
> 16 KB` counts each *partial* marker buffer as a full 16 KB (~10.8 KB/marker).
> The tools now report measurements, not verdicts. **Still open:** data
> *corruption* (delivered ≠ correct) — the 10 MHz-tone test. See "Resolution"
> below.

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
pps_integrity [hours] [--rate MSPS] [--firmware FILE] [-q N] [-p N] [-l CSV] [-v]   # default: 4 hours, 16 MSPS
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

1. **Bytes produced vs delivered, and the byte-exact drain backlog.** The
   firmware's `glDMACount` (GETSTATS byte 0) counts DMA buffers the GPIF
   *produced* (`PROD_EVENT`, one per 16 KB buffer **regardless of fill**).
   `produced_bytes = glDMACount × FW_DMA_BUF_BYTES` (16 KB, confirmed against
   the SDDC_FX3 GPIF→DMA config) vs `delivered_bytes = samples × 2` gives a
   gross, clock-independent gap — a slow ADC clock lowers both equally.
   **But the marker abandons *partial* buffers, which `glDMACount × 16 KB`
   counts as full, so while the marker runs this gap is dominated by that
   over-count (~10.8 KB/marker ≈ 42 ppm), not loss** — the tool reports it as
   a raw `gap`, not a verdict. The *authoritative* loss check is the
   **byte-exact drain backlog** = producer-minus-consumer socket `xferCount`
   (see "Where the loss is" below): a real orphan accumulates it; genuine
   in-flight just wobbles ±1 buffer. *Caveats:* `glDMACount` resets on
   `STOPFX3` (the end snapshot is read *before* `rx888_stop()`); the counters
   aren't sampled atomically and the pipeline holds transfers in flight, so
   produced runs ahead by ~`in_flight` — corrected in bytes with a few
   transfers of slack.
2. **librx888 `bad_xfers`** — errored/cancelled USB transfers
   (transport-level loss).
3. **Inter-marker continuity (diagnostic localizer, not the verdict).**
   Samples between consecutive markers vs `(span) × rate`, `rate` = median
   of single-second inter-marker counts. It localizes *when* a deficit
   appears, but marker-detection timing jitters by ~a marker's worth at
   high rates, so an isolated short interval (with the next running long,
   net zero) is **not** loss — it is reported as a localizer only and does
   not fail the run. (Historically it was "corroborated against the
   produced-vs-delivered gap"; since that gap is now known to be
   partial-buffer over-count, that corroboration was spurious — the
   authoritative check is the drain backlog.)

**PIB errors** (firmware GPIF overflow) are **polled every second** on
the EP0 handle, so an overflow is localised to the second it happens and
correlated with that interval's continuity result: PIB *with* a deficit
lost host-visible samples; PIB with none did not. The same poll catches
a mid-run device reset (boot-count change) immediately.

**Where the loss is: the byte-exact drain backlog (and there is none).**
`glDMACount` counts on `CY_U3P_DMA_CB_PROD_EVENT` — once per buffer the
**producer** socket commits into the channel, regardless of fill — so
`glDMACount × 16 KB` over-counts the marker's partial buffers and is *not* a
clean loss measure (see the Resolution section). The real check is the
**consumer** side: the firmware exposes the producer and consumer DMA-socket
`xferCount` registers (bytes) — producer `[36..39]`, consumer-API
`[40..43]`, consumer-raw `[44..47]`, in a 48-byte GETSTATS response. (An
earlier attempt used a `CY_U3P_DMA_CB_CONS_EVENT` callback counter; it never
fires on an AUTO channel and read a flat 0, so the register read replaced it
— and it must sum **both** PIB producer sockets to match the single
consumer's rate.) These wrap ~every 16 s at 129.6 MSPS, so the meaningful
quantity is the *instantaneous* **backlog = producer − consumer**, read as
**signed** (read-skew can leave the consumer one buffer ahead, wrapping to
~2³²). A real orphan makes the producer permanently outrun the consumer, so
the backlog **accumulates** (net start→end ≠ 0, or a sustained step);
genuine in-flight wobbles ±1 buffer and returns to ~0. Over 3 h the backlog
held net 0 with peak ±1 buffer and no sustained step — bounding any leak at
**< 6 ppb** — so nothing is orphaned, and the `glDMACount` gap is the
partial-buffer over-count. The tools report the backlog raw (start/end/net,
peak |in-flight|, `prod≠cons` count); `stream_soak` is the no-marker
baseline. This supersedes an earlier reading of the gap as "drain-side
orphaned loss": the byte-exact backlog shows no orphaning.

**This also rules out host backpressure:** a host too slow to drain would
show in the no-marker `stream_soak` control at the same rate — it does not —
and would also make the backlog accumulate, which it does not.

### Separating clock offset from loss

`delivered / (samplerate × elapsed)`, as ppm, is the ADC-vs-host **clock
offset combined with any uniform loss** — the two are identical in that
one number. They are separated by the fact that **loss always leaves a
fingerprint** (a `bad_xfer`, a continuity step, or `produced > delivered`)
whereas a **slow clock delivers everything, just at a steadily lower
rate** — a smooth drift with no fingerprints (the ~+0.16 ppm marker-size
drift we observe is exactly this).

Loss and clock are separated by the **byte-exact drain backlog**: when it
does not accumulate (it did not, over 3 h), no data was dropped and the rate
offset is the ADC-vs-host clock. The tool reports the offset *neutrally* as
`Rate offset: ±X ppm`, alongside the `budget` ppm and the `glDMACount` gap
ppm, noting the two are conflated — it does **not** assert which dominates
(here the gap is over-count, not loss). This assumes a disciplined
(NTP/PTP/GPS) host clock; on a free-running host the budget is host+ADC
combined.

Sensitivities differ by detector: the continuity diagnostic has a fine
floor (`LOSS_TOL`, ~0.1 buffer) but jitters; the `glDMACount` gap has a
coarse floor (the in-flight pipeline, a few MB) *and* is confounded by the
marker over-count; the **drain backlog** is the clean, clock-independent
loss check, with single-buffer resolution that tightens with run length
(< 6 ppb over 3 h).

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

### Resolution: the marker is byte-lossless; the "42 ppm loss" was a glDMACount over-count

After a long chase (history below), byte-exact firmware counters and a 3 h
run settled it: **the in-band PPS marker drops no data.** What we pursued for
~3 days (2026-06-20 → 06-23) as "~42 ppm of loss" is an artifact in our own
`glDMACount × 16 KB` accounting — it counts each *partial* marker buffer as a
full 16 KB.

Measured over 3 h at 129.6 MSPS (10,800 markers, sleep inhibited):

| quantity | value | meaning |
|---|---|---|
| `glDMACount × 16384 − delivered` | +119 MB = **42.6 ppm** | the apparent "loss" |
| per marker | **10.8 KB** (~0.7 buffer) | the partial-buffer over-count |
| drain backlog (`apiProd − apiCons`) | **net 0**, peak ±1 buffer, 0 sustained steps | no bytes orphaned |
| `apiProd ≠ apiCons` | 1027 / 10,800 s | producer counter is independent, not mirrored |
| real clock (`budget`) | **−0.47 ppm** | TCXO ~nominal; no fast clock |
| `bad_xfers`, PIB, faults | 0 | clean device |

**The over-count.** `glDMACount` increments once per committed DMA buffer
(`PROD_EVENT`), regardless of fill. The marker fires a GPIF thread-switch that
abandons a *partially*-filled 16 KB buffer; `glDMACount × 16384` books that
partial as a full buffer, over-stating "produced" bytes by `16384 − fill` each
marker — ~10.8 KB on average. At 1 marker/s that is a **fixed ~42 ppm
regardless of run length**, which is exactly why it was suspiciously constant
across every run while the real clock varied. `stream_soak` (no marker) shows
no gap because it makes no partial buffers.

**Why "no loss" (byte-exact).** The firmware exposes the producer and consumer
socket `xferCount` registers (bytes); their instantaneous difference is the
backlog actually sitting in the DMA channel. It stays at **0 ± exactly one
buffer** (genuine in-flight / read-skew) for the entire 3 h and **ends where it
started**. A real orphan would make the producer permanently outrun the
consumer, so the end backlog bounds the *total* leak: **< 1 buffer over 2.8 TB
≈ < 6 ppb**. And the producer counter is independent, not a mirror — it differs
from the consumer in 1027 of 10,800 seconds. So every produced byte was
delivered.

**The clock.** With the gap exposed as over-count, the old "implied clock / the
fast clock was masking the loss" reading is dead: the real ADC-vs-host offset
is the `budget` (−0.47 ppm this run) and it varies run to run like a real
clock; the +42.6 ppm gap is a constant artifact sitting on top of it, not a
clock effect.

**Tools report measurements now, not verdicts.** `pps_integrity` /
`stream_soak` print the numbers (glDMACount gap; drain backlog start/end/net;
peak in-flight; `prod≠cons` count; raw counters; rate offset). PASS/FAIL is
reserved for unambiguous faults (`bad_xfers`, device reset). Interpretation
lives here and in the `--statslog` CSV via `tests/pps_log_stats.py`.

#### How we got here (and what each step ruled out)

The dead ends are worth recording — the same data now explained drove a long
chase:

| run / step | what it showed |
|---|---|
| `stream_soak` (no marker), 64 / 129.6 MSPS | lossless — isolates the effect to the marker (we now know: to its partial buffers) |
| `pps_integrity` 100 kΩ slow edge, 129.6 | gap 42.3 ppm; spurious shorts 403 |
| `pps_integrity` 1 kΩ fast edge, 129.6 | gap unchanged (41.2 ppm); spurious 403 → 0 — edge governs *fidelity*, not the gap |
| 3 h, byte-exact drain counters, 129.6 | gap 42.6 ppm but backlog **net 0** — the gap is over-count, not loss |

- **KBA231382 (`INVALID_SEQUENCE` on commit-buffer)** — investigated, ruled
  out: the marker isn't a CPU commit at all (next bullet). The
  `SetWrapUp`-on-AUTO anti-pattern it warns about lives only in `synth_pps.c`,
  the `PPS_CTL_ENABLE=0` path, which never runs here.
- **Edge quality / metastability** — the marker reaches CTL[2] through a
  100 kΩ → ~4 µs edge, sampled directly (no synchronizer) by a TOGGLE
  comparator (`GPIF_CONFIG` bit 12) on the 129.6 MHz clock. The 1 kΩ fast-edge
  run left the gap untouched (so the slow edge was not the cause of the gap)
  — **but it was a real fix, not a dead end:** the slow edge had been
  chattering the comparator, and the fast edge took **spurious shorts 403 → 0
  and displaced markers 61 → 32**. That is a genuine marker-*fidelity* win
  (clean 1:1 edge↔marker correspondence, zero false delimiters) and is a kept
  change — it just fixed fidelity, not "loss" (there was none).
- **The marker is a forced *thread-switch*, not a commit.** `BETA_THR_WRAPUP`
  is set in no GPIF state; `TH0_PPS_COMMIT` is the normal `DATA_CNT_HIT`
  transition firing *unconditionally*, abandoning a partial buffer, which the
  DMA adapter then wraps to USB (the short transfer). This is the source of the
  partial buffers `glDMACount` over-counts. The channel holds 4 × 16 KB buffers
  (`AUTO_MANY_TO_ONE`, 2 PIB producers → 1 UIB consumer); the host's "64
  buffers per 1 MB transfer" is URB-side assembly.
- **The "drain-side orphan" hypothesis — refuted.** We suspected the abandoned
  partial collided with the ping-pong handoff and orphaned ~12 buffers in the
  DMA→USB drain. Building the consumer-side telemetry to *test* that is what
  produced the byte-exact backlog — which shows **no** orphaning over 3 h.
  (Getting the telemetry right took three firmware iterations: `CONS_EVENT`
  never fires on an AUTO channel; the socket-register read had to sum **both**
  producer sockets; and the host had to read the wrap-around backlog as
  signed.) The "~12-buffer dip" and "26.9× boundary enrichment" we chased were
  **continuity-detector artifacts** — that detector jitters by ~a marker's
  worth and was reading partial-transfer boundaries, not real loss — consistent
  with the backlog showing nothing orphaned.

#### Still open: data *corruption* (delivered ≠ correct)

"Not orphaned" is not "uncorrupted." A sample slipped or garbled at a
partial-buffer splice would balance every byte count above yet be wrong. The
sensitive test is a **known 10 MHz tone**: down-convert to baseband and track
the unwrapped phase — a single dropped/duplicated/corrupted sample is a
`2π·10/129.6 = 27.8°` phase step, trivially detectable; the FFT gives
SINAD/SFDR for analog quality; and correlating phase-step indices with the
marker positions tests the splice directly. The LTC2208 has no digital
test-pattern mode, so the injected tone is the ground truth. This is the next
experiment.

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
Sample rate:     129.6 MSPS (129600000 Hz)
Transfer size:   524288 samples (1048576 bytes)
Edges sent:      10800
Markers seen:    10797
Spurious shorts: 0
Missed markers:  3  (blind-spot: 0, displaced: 3) — marker timing only, see loss below
Displaced rate:  0.028% of edges (~1.0/hour) — delimiter moved one edge; reconstructable.
Samples in:      1399681021808 (1399.68 Gsa)
DMA buffers:     glDMACount 2799.48 GB (170866581 x 16384 B), delivered 2799.36 GB, gap +119.165 MB (in-flight 32->32, slack 4.19 MB)
DMA drain:       backlog start +0.000 end +0.000 net +0.000 MB; peak |in-flight| 16384 B (1.0 buf); prod!=cons 1027/10800 s; API-vs-rawreg skew 0 B
  drain raw:     start P=3538944 C=3538944 | end P=3338661600 C=3338661600 Craw=3338661600
USB transfers:   ok=2678321 bad=0
Sample loss:     310 continuity dip(s), ~28895477 samples (diagnostic localizer)
Loss floor:      continuity 65536 samples (0.12 buffer), largest dip 238485 (0.45 buffer); in-flight slack ~4.19 MB
  glDMACount-delivered gap: 119.165 MB = 42.6 ppm (0.0043%) — compare against the DMA-drain backlog above and --statslog
Rate offset:     -1.188 ppm (delivered vs rate x elapsed; budget -0.470 ppm, glDMACount gap +42.6 ppm — conflated, see drain/CSV)
PIB errors:      0 total in 0 second(s); 0 coincided with a deficit
Stream faults:   0
Boot count:      unchanged
Result: PASS
```

The integrity headline is the **`DMA drain` backlog** (producer−consumer
bytes still in the channel): net ~0 with peak ±1 buffer = nothing orphaned,
clock-independent. The `DMA buffers: … gap` line is `glDMACount × 16 KB −
delivered` — *gross*, and over-counts the partial marker buffers, so it reads
~42 ppm here **even with no loss**; it is a raw measurement, not a verdict.
`Rate offset` reports the delivered-vs-expected ppm with its `budget` and gap
terms, asserting neither as the cause. `Sample loss` (continuity) is a
localizer only. `Result: PASS/FAIL` keys on **unambiguous faults** —
`bad_xfers`, device reset, streaming faults, spurious shorts — not the
over-count gap.

**Note on firmware counters:** GETSTATS exposes `glDMACount` (producer DMA
buffers, 16 KB each — `PROD_EVENT`, regardless of fill), the producer and
consumer socket `xferCount` registers (bytes; the drain backlog),
`pib_errors`, `streaming_faults`, and `boot_count`. There is no
`pps_count`/`pps_fail` for the GPIF-driven marker.

**Host-side transfer diagnostics** (libusb layer, in `rx888_stats_t`): the
summary also prints `zero-length=N` — `LIBUSB_TRANSFER_COMPLETED` transfers
with `actual_length == 0` (ZLP / empty-buffer terminations, e.g. a marker
partial landing on a boundary) — and an `Xfer status:` line breaking out the
per-`libusb_transfer_status` tally (`ERROR / TIMED_OUT / CANCELLED / STALL /
NO_DEVICE / OVERFLOW`). This names what `bad_xfers` used to lump together, so
a short/ZLP-handling fault (`OVERFLOW` = device sent more than asked;
`TIMED_OUT` = a transfer that never terminated) is explicit rather than
inferred from a single `bad=0`. Both stayed 0 across the clean runs.

### Pass criteria

- **No transport/device fault** — `bad_xfers == 0`, no device reset, no
  streaming faults. (The `glDMACount` gap and continuity dips are *reported,
  not failed on* — the gap is partial-buffer over-count; real orphaning would
  show as `DMA drain` backlog accumulation, which it does not.)
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

## Future work: firmware features that would sharpen these tools

The marker turned out lossless, so this is no longer a fallback list — it is
the set of device-reported counters that would replace what the host still
*infers*. Some already landed during the investigation: the byte-exact
producer/consumer socket `xferCount` registers (the drain backlog) are exactly
the "byte-granular produced/consumed counter" idea, and the backlog's
accumulation is the drop detector. The rest stand as candidates (feasibility is
a firmware call — FX3 SDK 1.3.4, `AUTO_MANY_TO_ONE` channel, GPIF counter
hardware). The single highest-value *remaining* item is not firmware at all:
the **10 MHz-tone corruption test** (see "Still open" above), since the byte
accounting proves delivery, not correctness.

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
