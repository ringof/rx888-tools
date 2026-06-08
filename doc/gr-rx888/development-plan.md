# `gr-rx888` Development Plan

## Goals & non-goals

**In scope:** A GNU Radio 3.10 OOT module providing one source block,
`rx888.rx888_source`, that streams `int16` real samples from an RX888 /
RX888 mk2 over libusb-1.0, with runtime control of the HF analog
front-end and live health stats.

**Out of scope:**

- DSP (decimation, real → complex). Users wire stock GR blocks
  (`blocks.short_to_float`, `filter.hilbert_fc`, `filter.freq_xlating_fir_filter_ccf`,
  decimators) downstream.
- AVX2 / FMA dependency. The CLI tool's `rx888_dsp` is not ported; that
  way the OOT module runs on any CPU GR runs on.
- SoapySDR shim. The block links libusb directly. The whole point of
  the module is so a user doesn't have to make GR *and* SoapySDR work
  at the same time.
- VHF / R82XX tuner path. Not present in the current ringof
  ExtIO_sddc firmware.
- Reimplementing `rx888_dsp` or `iqrecord`.

## Target environment

- Primary: Ubuntu 24.04 LTS, GNU Radio 3.10.x from the distro
- Build deps:
  `gnuradio gnuradio-dev libusb-1.0-0-dev cmake pkg-config python3-pybind11`
- Runtime: existing `udev/` rule, existing `firmware/SDDC_FX3.img`,
  `usbfs_memory_mb >= 256` (set via the `tmpfiles.d` snippet documented
  below — no manual `echo … > /sys/...` after `make install`).
- README note: "radioconda support is planned but unverified."
- License: GPL-3.0-or-later (compatible with the GPL-2.0-or-later
  `ezusb.c` already vendored in this repo).

## Repo layout

`gr-rx888/` lives as a sibling to `src/` inside `rx888_tools` — keeps
the FX3 firmware, udev rule, and ezusb sources discoverable in one
place and lets the module reference the existing headers/sources
in-tree (no submodule, no vendoring).

```
gr-rx888/
├── CMakeLists.txt
├── cmake/Modules/FindLIBUSB.cmake
├── include/gnuradio/rx888/
│   ├── api.h
│   └── rx888_source.h
├── lib/
│   ├── rx888_source_impl.{h,cc}     # GR block
│   ├── rx888_device.{h,cc}          # libusb wrapper
│   └── CMakeLists.txt               # references ../../src/ezusb.c
│                                    # and ../../include/{rx888,ezusb}.h
├── python/rx888/
│   ├── __init__.py
│   ├── bindings/rx888_source_python.cc
│   └── qa_rx888_source.py
├── grc/rx888_rx888_source.block.yml
├── examples/
│   ├── rx888_to_file.grc
│   └── rx888_hf_spectrum.grc
└── docs/README.md
```

## Control surface

The current ringof ExtIO_sddc firmware drops the VHF / R82XX path, so
the block exposes only HF-relevant SETARGs and GPIO bits.

**SETARG IDs used (`include/rx888.h:82`):**

- `AD8340_VGA` (11) — HF VGA gain, 0..127
- `DAT31_ATT` (10) — step attenuator, 0..63

**GPIO bits exposed (`include/rx888.h:137`):**

- `DITH`, `RANDO` — ADC dither / output randomizer
- `BIAS_HF` — bias-T for HF antennas
- `LED_BLUE` — runtime status indicator (the only LED the firmware exposes)
- `SHDWN` — managed internally, not user-facing

**Dropped entirely:** all `R82XX_*` SETARGs, `PRESELECTOR`,
`VHF_ATTENUATOR`, `BIAS_VHF`, `VHF_EN`, `PGA_EN`, and the
`TUNERINIT` / `TUNERTUNE` / `TUNERSTDBY` vendor commands (including
the best-effort `TUNERSTDBY` at the end of `rx888_start` in
`src/librx888.c`).

## Phase 1 — Scaffolding (½ day)

1. Branch already exists: `claude/rx888-gnuradio-module-302or`.
2. Run `gr_modtool newmod rx888` in a scratch dir; copy generated tree
   into `gr-rx888/`.
3. `gr_modtool add -t source -l cpp rx888_source`.
4. Wire `find_package(LIBUSB REQUIRED)` into `lib/CMakeLists.txt`; add
   `rx888_device.cc` and `${CMAKE_SOURCE_DIR}/../../src/ezusb.c` to the
   library target. Add `${CMAKE_SOURCE_DIR}/../../include` to
   `target_include_directories`.
5. Smoke-test: empty block compiles, installs to `/usr/local`,
   `import rx888` works in Python, block appears in GRC.

**Done when:** flowgraph with the (empty) source + `null_sink` opens
in GRC and runs without crashing.

## Phase 2 — `rx888_device` core (1 day)

Most of this work is already done — `librx888` (added in this branch)
has the libusb / threading / control-plane logic.  The C++ wrapper is
a thin shim that calls into librx888 and exposes a class-shaped API
to the GR block.

If we choose to build directly against librx888 instead of writing a
C++ shim, the work collapses to "wire the library's callback into a
GR-friendly SPSC ring."  Either way the underlying mechanism is:

- Device open + firmware upload: `librx888.c:open_device` (around
  `src/librx888.c:227`).
- Configure / SETARG / GPIO / STARTADC: inline in `rx888_open()`
  (`src/librx888.c:454`).
- `start_stream` / `stop_stream`: `STARTFX3` / `STOPFX3` issued from
  `rx888_start` / `rx888_stop`.
- Async transfer engine: `librx888.c:writer_main` and
  `librx888.c:event_main` (around `src/librx888.c:373` and `:409`).
  N in-flight transfers, libusb event-pump thread, completed
  transfers pushed onto a thread-safe ring.
- Runtime setters (`set_vga_gain`, `set_attenuation`, `set_dither`,
  `set_randomizer`, `set_bias_hf`, `set_led(color, on)`) — **not yet
  in librx888**; v0.0 of the library is config-at-open only.  Adding
  setters means adding a control-endpoint mutex and the corresponding
  `rx888_set_*` exports; tracked as a librx888 follow-up.
- Atomic counters: `librx888` already exposes `ok_xfers`, `bad_xfers`,
  `bytes_out`, `in_flight`, `last_cb_ms` via `rx888_get_stats()`
  (`include/librx888.h`); the GR block adds `dropped_blocks` on its
  side (count of buffers it had to discard because the GR scheduler
  couldn't absorb them).
- `restart()` (added in Phase 4.5; see `stream-division-of-recovery.md`):
  re-entrant `open` + `configure` + `start_stream` so the block can
  bring a wedged FX3 back without process exit.

**Done when:** a standalone test program in
`lib/test_rx888_device.cc` opens the device, streams for 10 s at
32 MS/s, prints the same stats `rx888_stream -v` does, and exits
cleanly.  (At the C level, `tests/hw_smoke.sh` already does
the equivalent against `rx888_stream`; the new test exercises the
C++ shim path.)

## Phase 3 — `rx888_source` block (1 day)

- `make(...)` parameters:
  - `firmware_path` (`std::string`, default
    `/usr/local/share/rx888/SDDC_FX3.img`)
  - `sample_rate` (uint32, default 32 000 000)
  - `vga_gain` (0..127, default 0)
  - `attenuation` (0..63, default 0)
  - `dither` (bool, false)
  - `randomizer` (bool, false)
  - `bias_hf` (bool, false)
  - `queue_depth` (uint32, default 32)
  - `req_packets` (uint32, default 1024)
- Output signature: `sizeof(int16_t)`, single port. (Float mode
  deferred — users put `blocks.short_to_float` downstream.)
- `start()`: opens device, configures, calls `start_stream()`, kicks
  the libusb event thread.
- `stop()`: signals event thread, calls `stop_stream()`, joins,
  releases USB.
- `work()`: drains the SPSC ring into `output_items[0]`, returns the
  sample count. If GR's output buffer can't absorb the next transfer,
  increment `dropped_blocks` and discard the head buffer (don't
  backpressure libusb — that wedges the FX3).
- Public methods: all the runtime setters plus `get_stats()` returning
  a struct.
- Stream tags on the first sample after start: `rx_rate`, `rx_time`
  (current monotonic), `rx_freq` (0 — at baseband / real samples).

**Done when:** GRC flowgraph
`rx888_source → blocks.short_to_float → qtgui_freq_sink_f` shows the
HF spectrum at 32 MS/s.

## Phase 4 — Message ports & telemetry (½ day)

- Input port `cmd`: accepts PMT dicts like
  `{"vga_gain": 60, "att": 10, "dither": #t}`, dispatches to setters.
  Validate and log on out-of-range.
- Output port `stats`: emits a PMT dict every N seconds (parameter,
  default 1 s) with the atomic counters plus derived `MiB_per_s` and
  `dropped_blocks_per_s`.
- Output port `events`: emits PMT messages for `device_lost`,
  `transfer_error`, `usb_overflow`, `firmware_uploaded`. One-shot, not
  periodic. Recovery events added in Phase 4.5.

**Done when:** a Python QA test sends a `cmd` dict, reads back a
`stats` message showing the change took effect, and triggers an
`events` message by unplugging the device mid-stream.

## Phase 4.5 — Resilience (½ day)

See `stream-division-of-recovery.md` for the full rationale. Summary:

- `rx888_device::restart()` — make the open/configure/start sequence
  re-entrant.
- `rx888_source` recovery thread + state machine driven by parameters
  `recovery_mode`, `recovery_max_attempts`, `recovery_backoff_ms`,
  `recovery_zero_fill`.
- `events` port emits `recovery_started`, `recovery_succeeded`,
  `recovery_failed` PMT messages.
- Test plan: yank the USB cable mid-stream, plug back in within the
  retry window, verify clean resume with `rx_time` tag and a
  `recovery_succeeded` event.

## Phase 5 — GRC integration & examples (½ day)

- Polish `grc/rx888_rx888_source.block.yml`: parameter labels, ranges,
  tooltips matching the CLI help in
  `src/rx888_stream.c:usage()` and the field comments in
  `include/librx888.h`.
- `examples/rx888_to_file.grc`: source → head(32M samples) →
  file_sink. Equivalent to `rx888_stream … > out.s16`.
- `examples/rx888_hf_spectrum.grc`: source → short_to_float →
  freq_xlating_fir_filter → qtgui_freq_sink. Demonstrates getting to
  a complex view without the AVX2 DSP.
- One-page `docs/README.md`: build, install, `usbfs_memory_mb` note,
  link back to root `README.md` for hardware/firmware context.

## Phase 6 — Quality & CI (½ day, optional but recommended)

- `qa_rx888_source.py`: skips when no device present (env var
  `RX888_QA_HARDWARE=1` to opt in); otherwise tests the
  construct/destroy path with a mock libusb device handle injected via
  a test-only constructor.
- GitHub Actions: build on Ubuntu 24.04 with
  `apt install gnuradio gnuradio-dev libusb-1.0-0-dev`, run
  `cmake --build` and the no-hardware QA subset.
- Update root `README.md` with a short "GNU Radio module" section
  pointing at `gr-rx888/`.

## `usbfs_memory_mb` — make it persistent

Manually running
`echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb` after
every boot is the single most-forgotten step in the CLI workflow.
The OOT module fixes this two ways:

1. Ship `/usr/lib/tmpfiles.d/rx888.conf` from the root `Makefile`
   install target:

   ```
   w /sys/module/usbcore/parameters/usbfs_memory_mb - - - - 256
   ```

   `modprobe.d` is unreliable here because `usbcore` is built into the
   Ubuntu kernel, so its module options are ignored.
   `systemd-tmpfiles` writes `/sys` paths at every boot. Apply
   immediately with
   `sudo systemd-tmpfiles --create /usr/lib/tmpfiles.d/rx888.conf` —
   no reboot required. 256 MiB covers the GR block's 32 MS/s target;
   the CLI tool's documented 1000 value is for 135 MS/s.

2. In `rx888_device::open()`, compute the required minimum
   (`queue_depth × req_packets × pktsize`) and read the current
   `usbfs_memory_mb`.  If too low, throw a runtime error quoting the
   exact `systemd-tmpfiles --create` and `echo` commands needed to
   fix it.  The arithmetic is the same one librx888 uses to size the
   transfer ring (`src/librx888.c:rx888_start`); we just turn it into
   a hard precondition that fails fast with an actionable message
   instead of letting `LIBUSB_ERROR_NO_MEM` surface mid-stream.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| GR output buffer fills at 32 MS/s under load | `set_min_output_buffer(8 MiB)` in ctor; document `max_noutput_items` tuning; `dropped_blocks` counter makes it visible |
| Control transfers race with event thread | Single mutex on the device handle, taken by every setter and every control call in `configure()` |
| Firmware upload + re-enumeration timing | Bounded poll loop (≤5 s) in `open()`; clear error if device never reappears |
| `usbfs_memory_mb` too low | Ship `tmpfiles.d` snippet via `make install` + fail-fast precondition with actionable message |
| Block left holding the USB device after a Python exception | RAII in `rx888_device` dtor + GR `stop()` always called by the scheduler |
| FX3 wedges silently mid-stream | Recovery layer in Phase 4.5 (see `stream-division-of-recovery.md`) |

## Total effort: ~4½ days for v1

End state: `apt install gnuradio gnuradio-dev`,
`cmake .. && sudo make install` inside `gr-rx888/`, drag
**RX888 Source** into a flowgraph, hit play.
