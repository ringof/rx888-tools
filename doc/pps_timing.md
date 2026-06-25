# Assessing extracted-PPS timing quality (`doc/pps_timing.md`)

Status: PLAN (no rig yet). The timing half of the campaign — corruption first
(`doc/tone_quality.md`), then this. **Tier 2 below is gated on a tone-quality
PASS:** the 27 MHz tone is only a trustworthy timing ruler once coherence is
confirmed intact (no slips, residual carrier ≈ 0).

This doc is the relative / OS-level proof (Tier 1 µs, Tier 2 sub-sample). The
*absolute* tier (Tier 0: an independent GNSS reference to separate GPSDO drift
from everything else, for distributed citizen-science arrays) is forward work,
kept in `doc/timing_next_steps.md` — and gated on the Tier 1 µs proof landing
first.

## The question

The in-band PPS marker gives the *extracted* PPS: a sample index `S_k` where the
firmware latched the PPS edge (CTL[2]) on the 129.6 MHz ADC clock. How good is
that, versus the GPSDO PPS edge itself? Both come from the *same* GPSDO pulse,
so differencing them cancels the GPSDO's own error and leaves only what the
extraction adds.

Two paths for the same edge:
- **Direct:** GPSDO PPS → Pi GPIO → `pps-gpio` → `/dev/pps0`, kernel-timestamped.
- **Extracted:** GPSDO PPS → signal conditioning → FX3 CTL[2] → marker → short
  USB transfer; `pps_integrity` records the marker's cumulative sample index.

## Tier 1 — OS-level (pps-gpio + chrony): absolute correctness, not quality

chrony's job is to **lock the host clock to the same GPSDO** (direct PPS as a
refclock; NMEA over `/dev/ttyACM*` for the integer second). That puts the host
timestamps and the GPS-locked ADC clock in one timebase, so host-vs-ADC drift
doesn't masquerade as extraction error.

```
residual_k = S_k/fs − P_k     (after removing one constant stream-anchor offset)
```

- `std(residual)` → apparent timing quality
- slope → residual rate (should be ~0 with GPS lock; a slope is itself a finding)
- steps / outliers → a misplaced marker, a missed/spurious edge (the thing this
  tier reliably catches)

**The wall.** The marker is quantized to **1 ADC sample = 1/129.6e6 s =
7.716 ns**. pps-gpio's kernel-IRQ timestamp jitters at the **~µs** level — about
**~130 ADC samples**. So Tier 1's noise floor is ~130× coarser than the marker's
own resolution: it can prove the extracted PPS is *absolutely correct* (right
second, no gross misplacement) to ~µs and flag outliers, but it **cannot measure
the marker's intrinsic quality**. If the marker is as good as we claim, Tier 1
reports "unmeasurably good — flat to the OS floor," and that's the most it can
ever say.

Plumbing (Tier 1):
1. chrony.conf: `refclock PPS /dev/pps0 lock NMEA refid PPS`, NMEA via gpsd-SHM
   or chrony's NMEA driver; `log measurements tracking refclocks`.
2. Direct edges: `ppstest /dev/pps0` (or a `time_pps_fetch()` logger) →
   `edge  host_assert_time`.
3. Extracted edges: `pps_integrity --ppslog` → `edge#, sample_index, host_time`.
4. Offline join by edge number → residual CSV → gnuplot (residual vs time +
   histogram). chrony's `tracking.log`/`measurements.log` give an independent
   view of the direct-PPS jitter as a cross-check.

Tier 1 is necessary (absolute anchoring + outlier catch) but floor-limited.

## Tier 2 — the 27 MHz tone as the ruler (OS-clock-free)

Everything in the rig derives from one GPSDO: the 27 MHz tone, the 1 PPS, and —
via the integer-only Si5351 (×24 / ÷5) — the 129.6 MHz ADC clock. So per PPS
interval, exactly:

```
129,600,000 ADC samples   = 27,000,000 tone cycles   = 5,400,000 periods of N=24
phase advance per sample  = 360 × 5/24 = 75°
1 sample  = 7.716 ns  =  75° of 27 MHz      (so 1° of 27 MHz = 0.103 ns)
129,600,000 mod 24 = 0   → a perfect marker lands on the SAME period-24 slot
                            every second
```

This is a timing reference *inside the data stream*, at sample resolution
(7.716 ns), GPS-locked, with no OS clock and no µs floor. The three steps:

1. **Count off the expected samples.** Inter-marker sample count
   `ΔS_k = S_{k+1} − S_k` must be exactly **129,600,000**. Any deviation is the
   marker's placement jitter, in units of 1 sample (7.716 ns) — measured against
   the ADC clock alone, no OS clock involved. (`pps_integrity --ppslog` already
   yields `S_k`.)

2. **Align with a predictable position on the reference.** Because the tone is
   coherent and `129,600,000 mod 24 = 0`, a perfectly placed marker lands on the
   *same* period-24 phase slot every second. The tone phase at sample `S_k`
   (recoverable from `tone_monitor`'s baseband / a Goertzel around `S_k`) is the
   marker's "predictable position" — nominally constant.

3. **Show what phase it holds, over time.** Plot the marker-slot phase (and
   `ΔS_k`) over hours:
   - **flat** → the marker is sample-perfect *and* the GPSDO's PPS, 27 MHz, and
     ADC clock are mutually coherent;
   - **a 75° step / a ±1 in `ΔS_k`** → a one-sample marker error or a sample
     slip — the same step-discriminator as the corruption test;
   - **a slow phase walk** → a real frequency offset between the GPSDO's PPS and
     its 27 MHz output (a property of the GPSDO), which the slot-crossing rate
     measures to sub-ppb over a long run.

Resolution honesty: a *single* marker is sample-quantized (7.716 ns); you cannot
beat one sample per edge from the data. But the per-edge step test catches any
1-sample error exactly, and long-term phase tracking exposes sub-sample
*systematic* drift (the GPSDO PPS-vs-tone coherence) far below what Tier 1 can
see. So Tier 2 measures the marker where Tier 1 only bounds it.

## Why Tier 2 needs the tone-quality PASS first

The tone is only a valid ruler if coherence holds: if there were real sample
slips or non-coherence (fractional Si5351, bad split, drifting reference), the
75°-per-slip phase steps would corrupt both the corruption test and this timing
test indistinguishably. So the order is: confirm `tone_quality.py` reports
CLEAN (no slips, residual carrier ≈ 0) on a bare-stream baseline → *then* the
tone phase at the marker is meaningful as a timing reference.

## What we'd build when the rig is live

- `pps_integrity --ppslog FILE` — per marker: `edge#, sample_index, host_time`
  (also feeds Tier 1's join and `tone_quality.py --markers`).
- A small `ppstest`-style `/dev/pps0` logger (or chrony measurement log) for the
  direct edges (Tier 1).
- An offline timing analyzer (extend `tone_quality.py`): from the marker sample
  indices + the coherent baseband, compute `ΔS_k` (Tier 2 step 1), the
  marker-slot tone phase (steps 2–3), and the Tier-1 residual vs `/dev/pps0`;
  emit CSVs and gnuplot figures (inter-marker count, slot phase vs time,
  Tier-1 residual + histogram).
- Optional elegant cross-check: feed the extracted PPS to chrony as a
  `noselect` SOCK refclock so chrony logs its offset directly — convenient, but
  the tone-referenced Tier-2 numbers are authoritative.
