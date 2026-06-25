# Next steps: absolute timing & GPSDO characterization (forward work)

Status: FORWARD-LOOKING. **Do not start this until the µs-level marker proof
lands** — i.e. until `doc/pps_timing.md` Tier 1 (Pi + pps-gpio + chrony) has
shown the extracted PPS is true to the GPSDO edge at the microsecond level. The
Pi is sufficient for that first proof; everything here is the *next* tier and is
not needed to establish that short-commit PPS works.

This doc holds the absolute-time path, kept separate from `pps_timing.md` (which
is the relative / OS-level proof) on purpose.

## Why an absolute tier at all

Two different array topologies need two different things:
- **Co-located arrays** share one GPSDO; alignment is *relative* and the tone
  (Tier 2) measures it sub-sample. Absolute time is irrelevant there.
- **Distributed citizen-science arrays** do NOT share a clock across sites, so
  every node needs *absolute* UTC to correlate across the array, and the science
  is stated in absolute time. "Trust the cross-correlation" neither lands with
  that audience nor satisfies the requirement.

So the absolute tier is for the distributed case and for **complete coverage** —
a result legible to people who won't follow a common-mode-cancellation argument:
a number in nanoseconds against GNSS/UTC, with the GPSDO's own drift broken out
from everything else.

## Tier 0 — absolute reference (independent of the GPSDO)

Place this on a proper host with a hardware-timestamping NIC, **not the Pi**
(the Pi's NIC is software-timestamping only, ~µs, useless for ns work).

Rig:
- **u-blox ZED-F9T** — timing-grade GNSS (~5 ns RMS time pulse with the
  quantization-error/sawtooth correction it reports; multi-band; emits raw
  observables for common-view).
- **HW-timestamping NIC with an accessible SDP/PPS pin** (Intel i210 or i226 —
  see the caveat below). Feed the F9T PPS into an SDP pin and timestamp it
  against the NIC PHC in hardware, escaping the software-IRQ floor.
- The GPSDO PPS is captured the same way (a second SDP pin) so you measure
  **GPSDO PPS vs GNSS PPS directly** — that difference, over time, is the GPSDO
  drift, separated from everything else.

This sits above Tier 1 (OS pps-gpio, ~µs) and Tier 2 (tone, relative
sub-sample) in `doc/pps_timing.md`.

## NIC choice and the SDP-exposure caveat (the make-or-break detail)

Both the Intel **i210** (1 GbE, `igb` driver) and **i226** (2.5 GbE, `igc`)
fully support, in silicon + driver, hardware timestamping of an external PPS on
their SDP pins (`PTP_PF_EXTTS`), captured via `testptp`/`ts2phc`. The i210 is the
more trodden path in the time-nuts community for exactly this; the i226's `igc`
PTP/extts support matured later, so use a recent kernel (6.x) and confirm extts
actually arms (`testptp -d /dev/ptpN`) before relying on it. Also check the i226
SDP I/O voltage against the F9T's 3.3 V TIMEPULSE — level-translate if it isn't
3.3 V-tolerant (a non-issue on the i210). For this one job — HW-timestamping an
external PPS — the i210 is the lower-risk pick even at 1 GbE; the i226 buys
2.5 GbE for the data network at the cost of the newer `igc` path.

**The catch is physical access to an SDP pin.** Retail single-port cards —
including the Intel **I210-T1** and **I226-T1** — generally do NOT break the SDP
pins out to a header or SMA; the pins exist on the chip but aren't wired to a
connector. So:
- *Electrically / driver-wise:* an i210-T1 is sufficient — it's a classic choice
  and its extts is ns-class, far finer than needed.
- *Physically:* expect to **solder-tap an SDP pad** on the card to get the F9T
  PPS in; the retail I210-T1 typically has no dedicated PPS header. Verify your
  specific board — some OEM/industrial i210 boards expose SDP on a header/SMA,
  which avoids the mod.
- *Levels:* F9T TIMEPULSE is ~3.3 V CMOS and the i210 SDP is 3.3 V CMOS —
  compatible; add a series resistor and a clean common ground, don't overdrive.

Confirm the capability on the host with `ethtool -T <iface>` (look for
hardware-tx/rx + PHC) and `testptp -d /dev/ptpN` for extts.

Bottom line: yes, the original i210-T1 is sufficient for direct hardware PPS from
the F9T — through an SDP pad, most likely by a solder tap rather than an existing
header.

## Logging (same stream → CSV → gnuplot pattern as the rest)

- `ts2phc` (linuxptp) captures the external PPS into the PHC and/or logs the
  offset; or a small logger reads `PTP_EXTTS_REQUEST` events from
  `/dev/ptpN`.
- Emit per-second `gpsdo_pps_minus_gnss_pps` (ns) → CSV → gnuplot: the GPSDO
  drift/walk vs GNSS over hours. That panel *is* the "GPSDO drift vs anything
  else" proof.

## Error budget (per-sample absolute UTC, once composed)

| contributor | measured by | scale |
|---|---|---|
| marker quantization (1 ADC sample) | by design; stability via tone | 7.7 ns |
| marker ↔ GPSDO edge faithfulness | tone (Tier 2, relative) | sub-sample |
| GPSDO ↔ GNSS (drift/offset) | i210/i226 PHC + F9T (Tier 0) | ns — the attribution |
| GNSS absolute (F9T) | qErr-corrected; antenna/cable cal | ~few–20 ns |

Compose Tier 2 (sample-index ↔ GPSDO edge) with Tier 0 (GPSDO ↔ GNSS) and every
RX888 sample carries an absolute UTC timestamp with an *itemized* budget — and
you can point at which term moved when something drifts.

## Practical limits for citizen nodes

- **Antenna siting + cable-delay calibration**, not the electronics, sets the
  absolute accuracy at each node.
- For inter-site alignment **better than each receiver's standalone absolute**
  (relevant at VHF, where a period is ~3 ns), use **common-view GNSS** from the
  F9T raw observables — it cancels common GNSS errors site-to-site, sub-ns.

## Sequencing

1. (now) Prove marker PPS true to ~µs on the Pi — `doc/pps_timing.md` Tier 1.
2. Tone-quality PASS → Tier 2 relative sub-sample marker quality.
3. (this doc) Add Tier 0: i210/i226 + F9T absolute reference; GPSDO-vs-GNSS
   logging; compose into absolute per-sample UTC + budget.
4. Distributed arrays: per-node Tier 0 + common-view for inter-site.
