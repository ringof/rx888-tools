# Stream Division of Recovery

How `gr-rx888` splits stream-failure recovery between the libusb driver
layer (`rx888_device`) and the GNU Radio block layer
(`rx888_source`). This document is a companion to
`development-plan.md` and the source of truth for Phase 4.5.

## Background

The FX3 firmware (ringof ExtIO_sddc, branch
`claude/fix-fx3stop-issue-c4Gge`) does its best to drop back to an
idle state on any error path. So most "stream broke" situations are
recoverable in principle: the device is still enumerated, the FX3 is
sane, we just need to re-issue the start sequence.

The current `src/rx888_stream.c` only does part of that work. It
absorbs per-transfer transient errors but bails on anything that would
require a fresh streaming session — which is the right call for a
Unix pipeline tool (the user restarts the pipe), but is unfriendly
behaviour inside a GR flowgraph.

## What librx888 already handles

In the writer thread (`src/librx888.c:writer_main`, around
`src/librx888.c:373`) and the event thread
(`src/librx888.c:event_main`, around `src/librx888.c:409`):

| Failure | librx888's behaviour |
|---|---|
| `LIBUSB_TRANSFER_STALL` / `ERROR` / `OVERFLOW` / `TIMED_OUT` | Count as `bad_xfers`, attempt resubmit; stream continues. |
| `LIBUSB_TRANSFER_NO_DEVICE` | Set `stop_flag`; writer thread exits, then `rx888_stop` joins. |
| Watchdog: no callbacks for `cfg.watchdog_ms` | Set `stop_flag`; same exit path. |
| Internal queue full | Set `stop_flag`, shutdown ring; same exit path. |
| Pipe broken (only seen by the CLI) | Caller's `sample_cb` returns; CLI's `g_stop` triggers `rx888_close`. |

So the per-transfer resilience is free.  The "session-level"
resilience (re-open device, re-upload firmware if needed,
re-configure, restart) does not exist yet — librx888 doesn't expose
a `restart()` and `rx888_open` is not currently re-entrant on the
same handle.

## Design principle: mechanism in the driver, policy in the block

Two pieces, two layers, no overlap.

**Mechanism — `rx888_device`.** Knows the FX3 vendor command
sequence, knows the libusb open / claim / configure / submit dance.
Provides one new method:

```cpp
// rx888_device.h
//
// Re-entrant: calls stop_stream() + clean teardown of in-flight
// transfers, then re-runs open() + configure() + start_stream().  If
// the device dropped back to bootloader (PID 0x00f3), re-uploads the
// firmware blob.  Returns true on success.  Idempotent on success
// (calling restart() on a healthy device just stop/start cycles it).
//
bool restart();
```

This lets `restart()` exist without `rx888_device` knowing anything
about GR scheduling, sample tagging, or user retry preferences.

**Policy — `rx888_source`.** Knows about the GR scheduler, the block's
`work()` cadence, and what the user wants to happen when things go
wrong. Decides *whether* to call `restart()`, *how often*, and
*how visibly*.

## Block-level recovery parameters

| Parameter | Default | Meaning |
|---|---|---|
| `recovery_mode` | `"auto"` | `"none"` = legacy fail-fast; `"auto"` = retry up to `recovery_max_attempts`; `"infinite"` = retry forever (until `stop()`) |
| `recovery_max_attempts` | `5` | Only meaningful for `recovery_mode == "auto"` |
| `recovery_backoff_ms` | `500` | Initial backoff. Doubled per attempt, capped at 8000 ms |
| `recovery_zero_fill` | `true` | While recovering, `work()` emits zeros so downstream blocks don't starve. If false, `work()` returns 0 and the scheduler spins |

Use-case mapping:

- 24/7 spectrum / propagation logger: `recovery_mode="infinite"`,
  `recovery_zero_fill=true`. Keep streaming through cable yanks and
  brown-outs.
- Measurement experiment: `recovery_mode="none"`. A glitch must crash
  the flowgraph; silently filling a gap could corrupt results.
- Default GUI use (gqrx-style): `recovery_mode="auto"` with the
  defaults. A momentary USB glitch is hidden, a real failure stops
  the flowgraph after a handful of seconds.

## State machine

```
  STREAMING ──(terminal libusb error)──▶ RECOVERING
      ▲                                       │
      │                                       │ device.restart() OK
      └───────────────────────────────────────┘
                                              │
                                              │ attempts exhausted
                                              ▼
                                            FAILED
                                              │
                                              ▼
                                       work() → WORK_DONE
```

Transitions:

1. **STREAMING → RECOVERING.** The libusb event thread sees a
   terminal error (`NO_DEVICE`, watchdog timeout, repeated
   `submit_transfer` failures). It atomically sets
   `state = RECOVERING`, drops in-flight buffers from the SPSC ring,
   and signals the recovery thread. Emits a `recovery_started` PMT
   on the `events` port:

   ```python
   {"event": "recovery_started",
    "reason": "LIBUSB_ERROR_NO_DEVICE",
    "attempt": 1,
    "max_attempts": 5}
   ```

2. **RECOVERING.** A dedicated recovery thread (kept separate so
   `work()` stays responsive) calls `device.restart()` with backoff
   between attempts. The libusb event thread is paused; `work()`
   either returns 0 or zero-fills depending on `recovery_zero_fill`.

3. **RECOVERING → STREAMING (success).** Recovery thread restarts the
   libusb event pump, sets `state = STREAMING`, marks the *next*
   sample emitted from `work()` with an `rx_time` tag (so downstream
   blocks know there was a discontinuity) and emits a
   `recovery_succeeded` PMT:

   ```python
   {"event": "recovery_succeeded",
    "attempts": 2,
    "gap_ms": 1840}
   ```

4. **RECOVERING → FAILED.** All retries exhausted (only reachable
   when `recovery_mode == "auto"`). Emit `recovery_failed`, set
   `state = FAILED`. Next `work()` returns `WORK_DONE`; the scheduler
   tears the flowgraph down cleanly.

   ```python
   {"event": "recovery_failed",
    "attempts": 5,
    "last_error": "LIBUSB_ERROR_NO_DEVICE"}
   ```

## Why `work()` should zero-fill (default)

Returning 0 from `work()` is correct but causes the scheduler to spin
calling `work()` again immediately, burning CPU until the recovery
thread either wins or gives up.

Zero-filling for the duration of the gap lets downstream blocks keep
their cadence (PLLs don't lose lock from a sample-rate hiccup, AGCs
don't see a sudden re-engage spike when samples resume). The
`rx_time` tag stamped on the *resumption* sample gives any block that
cares the information needed to compensate.

If a user wants to detect the gap explicitly rather than have it
papered over, `recovery_zero_fill=false` plus a `gap_detected`
listener on the `events` port gets that.

## Sample / time tagging during recovery

- No tag during the gap itself. The gap content is either zeros or
  nothing.
- On the first sample emitted after a successful `restart()`:
  - `rx_time` tag with current monotonic time.
  - `rx_rate` tag (in case a future version supports rate change at
    restart — for v1 it's the same value).
  - A `discontinuity` tag with `{"gap_ms": N, "attempts": N}` so
    downstream code can react explicitly without subscribing to the
    `events` message port.

## Test plan

| Scenario | Expected |
|---|---|
| Yank USB, plug back in within 2 s | `recovery_started` + `recovery_succeeded(attempts=1)`, stream resumes, `rx_time` tag visible on next sample |
| Yank USB, plug back in after `recovery_max_attempts × backoff` window | `recovery_started` + `recovery_failed`, flowgraph stops cleanly |
| `recovery_mode="none"`, yank USB | No `recovery_started`, flowgraph stops on first terminal error |
| `recovery_mode="infinite"`, yank USB for 60 s, plug back in | Stream resumes whenever the device reappears |
| Force a watchdog timeout (block libusb event thread) | Same as USB yank: enters RECOVERING |
| Send 100 `cmd` PMTs while RECOVERING | Setters queue or no-op safely; no control transfers attempted on a torn-down handle |
| Multiple successive recoveries within 30 s | Each succeeds; `gap_ms` reflects actual stall, not cumulative |

## What is *not* in scope for v1

- Recovering across a sample-rate change. Restart uses the same
  `sample_rate` the block was constructed with.
- Recovering across a different firmware version showing up after
  re-enumeration. Detect and fail (the firmware blob is a constructor
  parameter, not auto-discovered).
- Cross-device fail-over (e.g. multi-RX888 setups with a hot spare).

These are deliberate v1 omissions; the state machine doesn't preclude
them, but adding them now would expand the test matrix without
benefit.

## Effort

Folded into Phase 4.5 of the development plan: ~½ day. Total project
estimate moves from ~4 days to ~4½.
