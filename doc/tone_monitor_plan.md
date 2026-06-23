# Plan: coherent-tone corruption monitor (`tone_monitor` + offline analyzer)

Status: STEP 1 BUILT + DSP-VALIDATED (no hardware yet). Branch
`claude/pps-development`. `src/tone_monitor.c` exists with `--statslog`,
`--iqlog`, and a `--source FILE` replay mode; the inline Goertzel demod is
validated against synthesized clean/drop/garble waveforms by
`tests/tone_monitor_replay.sh` (in `make check`): clean -> flat phase, drop ->
75.00 deg grid slip, garble -> amplitude dip with no phase step. The offline
tracking-demod analyzer (step 2) is not built yet.

## `--source`: hardware-free validation of the real DSP

`--source FILE` (or `-` for stdin) drives the SAME `sample_cb`/Goertzel/iqlog
path from raw int16 samples instead of the device, emitting the same `--iqlog`
and `--statslog`. A "second" in file mode is `fs` input samples. This lets a
synthesized waveform exercise the production DSP with no rig — used by
`tests/tone_monitor_replay.sh` and handy for replaying captured streams.

## Goal

Watch RX888 data **quality** (not just delivery) over hours-long runs on a
Raspberry Pi, the same way `pps_integrity`/`stream_soak` watch delivery: the
inline tool emits **raw, collectable telemetry only — no verdicts** — and all
interpretation happens **offline on the saved output**, so the analysis can be
refined repeatedly without re-running the rig.

This is the corruption half of the campaign (corruption before timing —
nothing about timing matters if the data is corrupted).

## Why a coherent tone, restated as the design contract

A single GPSDO 27 MHz output is split to BOTH the Si5351 reference AND the RF
input. The Si5351 uses INTEGER multipliers only (PLL ×24 → 648 MHz VCO → ÷5 →
129.6 MHz; no fractional MultiSynth / delta-sigma). Therefore

```
f_tone / fs = 27 / (27 × 24/5) = 5/24      (exact, drift-immune)
```

GPSDO absolute-frequency drift scales numerator and denominator identically and
is **perfectly rejected** by the ratio — it cannot create a residual frequency.
What remains for the demodulator to absorb as *benign* is only: Si5351 PLL phase
noise, differential split-path delay (slow, thermal), and level drift. A real
sample **slip** (drop/dup) is a persistent phase STEP of 75°/sample; a **garble**
is a transient spike in amplitude/phase. These are discontinuities no benign
variation produces — which is exactly what a tracking demodulator separates.

## Architecture: cheap inline, refinable offline

| Stage | Where | Cost | Output |
|---|---|---|---|
| Single-bin Goertzel block demod | inline C, on the Pi | 1 MAC + 2 adds / sample, no per-sample trig | decimated complex baseband + per-block (|X|, φ) |
| Per-second aggregation | inline C | trivial | CSV telemetry rows |
| Tracking demod + slip/garble discrimination + SINAD | offline Python | heavy, runs on saved data | analysis, refinable forever |

The inline tool never decides PASS/FAIL. It demodulates and logs. The offline
analyzer is where the tracking loop, thresholds, and classification live, and is
the only part we expect to keep changing.

## The inline primitive: single-bin Goertzel at bin 5/24

Block length `M` = decimation factor, chosen as a multiple of 24 so the bin is
exact (coherent integration, zero leakage). The Goertzel coefficient is
independent of `M` because `k/M = f0 = 5/24`:

```
w     = 2π·5/24
coeff = 2·cos(w)
per sample:   s0 = x[n] + coeff·s1 − s2 ;  s2 = s1 ;  s1 = s0     (1 mul, 2 add)
per block M:  re = s1 − s2·cos(w)
              im =      s2·sin(w)
              reset s1 = s2 = 0
```

`X = re + j·im` is one complex baseband sample at rate `fs/M`. `|X|` is the tone
amplitude; `atan2(im, re)` its phase. This single mechanism BOTH produces the
decimated baseband (for the binary log) AND the per-block amplitude/phase (for
the CSV) — no second DSP path.

Localization note: a slip is a permanent phase step, visible at any decimation;
decimation only sets how finely an event is located in time (see size table).

## Output 1 — per-second CSV (always on, ~MB/hour)

`--statslog FILE` (matching the PPS tools). One row per logging interval
(default 1 Hz), raw measurements only:

```
time,sec,blocks,amp_mean,amp_min,amp_max,phase_resid_rms_deg,resid_freq_hz,max_phase_step_deg,worst_step_sample,floor,nblocks_over_floor
```

- `amp_mean/min/max` — |X| stats over the interval (level + dropouts).
- `resid_freq_hz` — mean per-block phase advance → residual frequency (should
  sit at ~0; slow wander = benign GPSDO/PLL).
- `max_phase_step_deg` + `worst_step_sample` — largest single-block phase jump
  in the interval and its sample index. **This preserves slip evidence even when
  the per-second mean looks clean**, so an offline pass can still find it.
- `floor`, `nblocks_over_floor` — a coarse inline disturbance count against a
  slow baseline; purely informational, NOT a verdict.

Plus a start row (fs, ftone, N, M, decim, rate) and an end row (totals), exactly
like `pps_integrity --statslog`.

## Output 2 — decimated-baseband binary (optional, the refinable artifact)

`--iqlog FILE`: the per-block Goertzel complex output written as interleaved
`float32` `(I,Q)` pairs at rate `fs/M`. This baseband IS the demodulated tone —
amplitude, phase, and frequency preserved — so offline you can redo slip
detection, garble detection, phase-residual, and even baseband SINAD with any
algorithm, as many times as you want, **with no rerun**. Header: a small fixed
struct (magic, fs, ftone, M/decim, start unix time) so the analyzer is
self-describing.

Size is the decimation knob:

| decim (M) | baseband rate | bytes/s | per hour | localizes slip to |
|---|---|---|---|---|
| 2400 | 54 kHz | 432 KB/s | ~1.5 GB | ~18 µs (ample for marker correlation) |
| 24000 | 5.4 kHz | 43 KB/s | ~155 MB | ~185 µs |

Default **decim = 2400**. `--decim N` (must be a multiple of 24) to trade size
vs. localization.

## Output 3 — event-triggered raw window (optional, later)

`--dump-window` could ring-buffer the last ~64 KB of *full-rate* int16 and dump
a short window when a block exceeds the inline floor, for sample-exact offline
forensics. Deferred — the baseband binary covers the common case; add only if we
find we need full-rate localization.

## The C tool: `src/tone_monitor.c`

Mirror `stream_soak.c`:
- librx888 owns the bulk stream; `sample_cb` runs the Goertzel inline.
- Second libusb handle for EP0 GETSTATS (reuse `read_fw_stats`, same 48-byte
  layout) so the firmware counters ride along in the CSV — cross-reference a
  quality event against PIB overflow / faults / glDMACount in one file.
- CLI: `[hours] --decim N --interval SEC --statslog FILE --iqlog FILE
  --fs HZ --ftone HZ --firmware FILE -q -p -v`. Defaults: fs 129.6e6,
  ftone 27e6, decim 2400, interval 1 s.
- Sample rate is fixed by the rig; `--fs`/`--ftone` only set the Goertzel bin
  (assert `gcd`-derived N divides decim).
- Add to `Makefile` next to `stream_soak`; link librx888 + libusb; `make check`
  must stay green.

Inline budget check: 129.6 MSPS × (1 mul + 2 add) ≈ 0.4 Gflop/s of scalar work,
NEON-vectorizable, plus one `atan2`/block (54k/s at decim 2400). Same order as
the per-transfer counting the PPS tools already sustain on the Pi.

## The offline analyzer (extend `tests/tone_quality.py` or sibling)

Ingest both artifacts (auto-detect: `.iq` binary via header magic; CSV via
header `time,sec,blocks`), then run the analysis we keep refining:

1. **Tracking demod / discriminator** — unwrap baseband phase, fit a slow model
   (EWMA frequency or 1st/2nd-order loop) for benign PLL/path wander, take the
   residual. Report slips (phase steps ~75°/slipped-sample, count + sample
   index + magnitude) and garbles (transient amp/phase spikes). The loop
   bandwidth is the one tunable that says "benign vs. discontinuity."
2. **Frequency** — residual freq vs. time (should be flat at ~0; structure means
   coherence broke / not split / fractional divider engaged).
3. **Amplitude** — level vs. time, dropouts.
4. **SINAD / SFDR / ENOB** — on the baseband or on an on-demand full-rate window.
5. **Marker correlation** — `--markers FILE` from `pps_integrity`: are events
   within N samples of a marker splice?

Keep the existing full-capture path in `tone_quality.py` working for bench
captures; the new path just reads the decimated baseband instead of the raw
stream. No verdicts emitted by the inline tool; the analyzer may classify, but
always alongside the raw tracked amplitude/freq/phase.

## Validation (before any rig run)

Synthesize a coherent int16 tone at 5/24, and a decimated-baseband `.iq` from it:
- clean → residual flat, 0 slips, ENOB sane;
- inject a single-sample drop → one ~75° phase step, 1 slip, located;
- inject a garble (corrupt a run of samples) → transient spike, phase recovers,
  flagged as garble not slip;
- inject slow frequency wander → tracked out, NOT flagged (proves benign-variation
  tolerance — the whole point of this redesign).
Reuse the synthetic generators already used to validate `tone_quality.py`.

## iqlog binary format (v1)

48-byte little-endian header, then interleaved `float32 (I,Q)` records at
`fs/decim`:

```
char[8] magic "RX888IQ\0" ; u32 version=1 ; u32 decim ; u32 period_n ;
u32 period_cyc ; f64 fs_hz ; f64 ftone_hz ; f64 start_unix
```

The phasor is scaled by `2/decim`, so `|I+jQ|` is the tone amplitude in LSB and
`atan2(Q,I)` its phase. A grid slip is a persistent `360*cyc/period_n` deg step
(75 deg at the rig); a garble is an amplitude/phase transient.

## Sequencing

1. DONE — `src/tone_monitor.c`: Goertzel inline + `--statslog` CSV + `--iqlog`
   binary + `--source` replay; Makefile + smoke + replay tests in `make check`.
2. Offline analyzer path for the `.iq` binary + CSV with the tracking
   demod/discriminator (phase-locked, slip vs garble vs benign wander); validate
   on synthetic data (extend `tests/tone_quality.py`).
3. Doc: fold method + formats into `doc/pps_integrity.md` (or a new
   `doc/tone_quality.md`); update PR #32 (still DRAFT).
4. Rig: bare-stream tone baseline (confirm clean), then run alongside
   `pps_integrity --markers` for the splice correlation.
```
