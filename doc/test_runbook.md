# Runbook: acquisition / data-quality test

Operational procedure for running a coherent-tone capture and confirming the
RX888 acquires cleanly (no skips, jumps, or dropouts). The *method* is in
`doc/tone_quality.md`; host bring-up (power, udev, SSD, usbfs, GPSDO config) is
in `REPRODUCE.md` §3a. This is the step-by-step for a session.

Rig: GPSDO OUT2 (27 MHz square) split → RX888 reference-clock input **and** →
attenuator → RX888 HF input; GPSDO OUT1 (1PPS) and NMEA for the timing tier.
Run native on the Pi for bring-up/dialing; the container is for keep/publish
runs and analysis (it bundles numpy/gnuplot).

## 0. Pre-session gates (every time)

```sh
./scripts/ssd-preflight.sh                                    # SSD mounted, writable, room
LBE142X=~/gps_pps/lbe-142x/build/bin/lbe-142x ./scripts/gpsdo-preflight.sh
```
Both must read **PREFLIGHT PASS** (GPS+PLL lock, antenna OK, OUT1=1PPS,
OUT2=27 MHz, NMEA + fix; SSD on the NVMe with space). A misconfigured or
unlocked GPSDO silently poisons the data — don't skip these.

## 1. Set the front-end level (DO NOT trust the defaults)

The library default leaves the AD8370 VGA at the **bottom** of its range, which
buries a padded tone under the ADC's fs/4 spur. Always set `--gain`. Probe ~10 s
and read the live `-v` columns:

```sh
./tone_monitor 0.01 -f firmware/SDDC_FX3.img --gain 127 --att 0 \
    --iqlog /mnt/ssd/rx888/probe.iq --statslog /mnt/ssd/rx888/probe.csv -v
```
- **`amp_mean`** should be a few thousand LSB (≈ −18 dBFS at gain 127 / att 0 with
  ~60 dB external pad). If it's ~0, the tone isn't landing — check level, then
  that the tone is on the right bin (next line).
- **`residHz`** small/stable and **`maxstep`** tiny (after row 1) ⇒ coherent.
- Sanity that the tone is on the expected bin (catches a wrong sample rate):
  capture a short raw grab and look at the spectrum — the peak's `frac` is
  fs-independent (27 MHz at fs 129.6 ⇒ `frac ≈ 0.2083 = 5/24`):
  ```sh
  # match the level you capture at — rx888_stream's default gain is buried too:
  timeout 1 ./rx888_stream -s 129600000 -g 127 -a 0 > /mnt/ssd/rx888/raw.s16
  python3 tests/tone_quality.py /mnt/ssd/rx888/raw.s16 --powers --fs 129600000 --max-samples 8000000
  ```
  The tone should be the dominant peak at `frac ≈ 0.2083` (5/24). If the fs/4 spur
  (`frac 0.25`) dominates instead, that's buried gain, not a rate problem.
Adjust `--gain` / `--att` (or the external pad) until `amp_mean` is comfortable
(a few thousand LSB; ~−10 to −20 dBFS). Do **not** start the long run until the
probe is clean.

## 2. Capture

```sh
./tone_monitor <hours> -f firmware/SDDC_FX3.img --gain 127 --att 0 \
    --iqlog /mnt/ssd/rx888/run.iq --statslog /mnt/ssd/rx888/run.csv
```
- `<hours>` is fractional; the iqlog is **~1.5 GB/hr** (decim 2400). Confirm SSD
  space for the duration (`ssd-preflight.sh` already did, for ≥ MIN_FREE_GB).
- The `--statslog` CSV is the per-second telemetry; it is the drift-robust record
  even if the iqlog is later too big to one-shot.

## 3. Analyze

**(a) Quick verdict from the statslog** (no numpy, no memory, drift-robust —
this alone answers the jump question):
```sh
grep -v '^#' /mnt/ssd/rx888/run.csv | \
  awk -F, 'NR>1{n++; if($8+0>m)m=$8; if($8+0>40)c++} END{printf "%d sec | max_step peak %.1f deg | %d sec >40deg\n",n,m,c}'
# which seconds (startup vs mid-run?):
grep -v '^#' /mnt/ssd/rx888/run.csv | awk -F, 'NR>1 && $8+0>40{print "sec",$2,"step",$8,"sample",$9}'
# picture in the SSH terminal (headless: dumb terminal, no X needed):
grep -v '^#' /mnt/ssd/rx888/run.csv | awk -F, 'NR>1{print $2,$8}' > /tmp/ms.dat
gnuplot -e "set term dumb 120 30; set xlabel 'sec'; set ylabel 'maxstep'; plot '/tmp/ms.dat' w l"
```
A real grid slip is a ~75° spike in one second. Expect a flat plot with at most
one sub-75° blip at **sec 0** (the startup transient at ~0.5 ms — benign).

**(b) Full streaming analysis of the iqlog** (constant memory, drift-robust —
required for long / undisciplined-oscillator captures; the whole-file path OOMs
on multi-GB and mis-counts drift as slips):
```sh
docker run --rm -v /mnt/ssd/rx888:/data -v "$PWD/tests:/opt/rx888-tools/tests:ro" \
    rx888-ppskit python3 tests/tone_quality.py /data/run.iq --window 60
```
Read: `[1]` amplitude mean/min/max (flat = no dropouts; min is usually startup),
`[2]` residual-carrier range = oscillator drift over the run, phase jitter (deg
RMS), and the slip count (startup-only = clean).

**(c) Transport verdict** — from the `tone_monitor` run summary (clock-
independent, the authoritative no-lost-data line):
```
USB transfers:   bad=0 zero-length=0
Xfer status:     ERROR=0 TIMED_OUT=0 CANCELLED=0 STALL=0 NO_DEVICE=0 OVERFLOW=0
PIB errors:      0    Stream faults: 0    Boot count: unchanged
iqlog:           ... 0 dropped
```

## 4. Pass criteria (clean acquisition)

| axis | source | pass |
|---|---|---|
| skips / lost data | run summary counters (3c) | bad/zero/OVERFLOW/PIB/faults/dropped all 0; boot unchanged |
| jumps / slips | statslog max_step (3a) + windowed slips (3b) | no slip > startup; max_step flat except sec 0 |
| dropouts | windowed amplitude (3b) | mean stable, min = startup only |
| coherence | windowed phase jitter (3b) | jitter small (sub-degree) |
| record count | summary / file size | ≈ duration × bb_rate (no large gaps) |

Carrier drift (residHz over the run) is **reported, not graded** — with an
undisciplined oscillator it's expected; it matters for the timing tier, not for
acquisition.

## 5. Gotchas (hard-won)

- **Set `--gain`.** Default VGA = bottom of range → padded tone buried under the
  fs/4 spur, reads as "no tone." Confirm `amp_mean` on the probe.
- **Use `--window` for anything long or with an undisciplined oscillator.** The
  whole-file analyzer OOMs on multi-GB and counts slow carrier drift as slips.
- **Startup transient** at ~0.5 ms (sample ~64800) every capture — sub-75°,
  non-integer, benign. For spotless reports, discard the first ~1 s.
- **Headless gnuplot:** `set term dumb` (in-terminal ASCII) or `set term pngcairo;
  set output FILE` (scp the PNG). The default qt/x11 terminal fails over SSH.
- **Wrong-rate check:** a tone at the unexpected `frac` (e.g. 0.25 instead of
  0.2083) means the ADC isn't at the assumed rate — chase the clock, not the
  data. `--powers` reports `frac` (fs-independent).
- **Native vs container:** native for dialing/first-light; container for the
  real runs + analysis (it has numpy/gnuplot). Firmware load stays a host step.
- **Trust the raw measurement over the theory.** If a peak or count disagrees
  with what you expect, widen the view (all peaks via `--powers`, the statslog
  max_step) before changing anything.
