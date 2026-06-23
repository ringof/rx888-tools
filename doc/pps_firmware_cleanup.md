# Firmware cleanup handoff — GETSTATS dead-counter cleanup

For the firmware side (rx888-firmware / SDDC_FX3). Host-side companion to the
PPS-integrity investigation in `doc/pps_integrity.md`. This is a request to
remove/relabel diagnostics that turned out to be inaccurate or dead, **without
shifting the byte offsets the host decoder depends on**.

## Last-known-working pairing (pin this)

The host tools in THIS repo and the firmware were verified working together at:

| Component | Repo | Commit |
|---|---|---|
| Host tools (this repo) | `ringof/rx888-tools` | `bc68cbf3a18576067a0a8a80d7d2776a594e69d7` |
| Firmware | `rx888-firmware` | `09cc99b2ae7be2924edcd7ea3fe5f78454845346` |

At this pairing the 3-hour campaigns ran clean: the in-band PPS marker is
byte-lossless, the ~42 ppm "loss" was identified as a `glDMACount×16384`
partial-buffer over-count (not real loss), and the byte-exact drain backlog
(producer−consumer socket `xferCount`) confirmed net-zero orphaning over 3 h.
Any firmware change below should keep the host decoder reading the same 48-byte
layout so this pairing's tools still parse the response.

## Current GETSTATS layout (48 bytes) — what each region is

```
[0..3]    glDMACount            CY_U3P_DMA_CB_PROD_EVENT count (per buffer,
                                regardless of fill) — OVER-counts partial
                                marker buffers; keep, but it is NOT a loss meter
[4]       gpifState
[5..18]   PIB error/status block
[19]      Si5351 status
[20..23]  boot_count
[24..25]  CLK0 status
[26..35]  glPpsCount / glPpsCommitFailCount / glPpsLastWrapS0 / ...S1   <-- DEAD
[36..39]  apiProd   summed PRODUCER socket xferCount (both PIB sockets)  KEEP
[40..43]  apiCons   consumer xferCount via API                          KEEP
[44..47]  rawCons   consumer xferCount via raw register read            KEEP
```

## Requested changes

1. **Relabel `[26..35]` (the `glPps*` block) as reserved, IN PLACE.**
   These counters are only populated by `synth_pps.c`; in the `PPS_CTL_ENABLE`
   build they are never written and read as stale/zero. They misled the
   investigation. **Do NOT compact the struct** to reclaim the bytes — that
   would shift `[36..47]` and break the host decoder pinned above. Zero-fill
   the region and rename to `reserved[10]` (or similar) so it's explicitly dead
   but offset-stable.

2. **Confirm `glDMAConsCount` / `CONS_EVENT` is fully removed.**
   `CY_U3P_DMA_CB_CONS_EVENT` does not fire on an AUTO (AUTO_MANY_TO_ONE) DMA
   channel, so the old `glDMAConsCount` was always zero — an inaccurate
   "consumed buffers" reading. It was correctly replaced by the socket
   `xferCount` registers at `[36..47]`. Make sure no remnant of the CONS_EVENT
   callback or `glDMAConsCount` remains, and that `[36..39]` is the **summed
   producer** xferCount only (both PIB producer sockets), not a mix.

3. **Keep `rawCons` `[44..47]` and keep the response length at 48 bytes.**
   The host requires the 48-byte length to know the drain block is present;
   older/shorter responses are treated as "drain counters absent." Don't shorten
   the response.

## What stays and why

- `glDMACount` stays — it's a useful producer-event tally, just not a loss
  meter. The host now reports the `glDMACount×16384`-vs-delivered gap as a raw
  measurement and cross-checks it against the byte-exact drain backlog rather
  than treating it as loss.
- The drain block `[36..47]` (producer/consumer `xferCount`) is the authoritative
  orphan/leak check; keep all three fields. The host reads producer−consumer as
  a **signed** backlog (it wraps ~16 s and read-skew can put the consumer up to
  one buffer ahead).

## Net effect

No offset changes; one dead region relabeled reserved; CONS_EVENT-based counter
confirmed gone. Host decoder for the pinned tools commit continues to parse the
48-byte response unchanged.
