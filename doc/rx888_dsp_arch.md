# rx888_dsp Architecture

## Overview

`rx888_dsp` is a fixed-function, real-time DSP decimation pipeline designed to
process very high sample-rate SDR data on commodity x86-64 CPUs. The design
prioritizes deterministic performance, low memory traffic, and clean
integration into Unix pipelines.

The codebase intentionally trades generality for predictability and
measurability.

---

## Functional Goal

Input:
- 135 MS/s real-valued int16 samples (RX888-class front ends)

Output:
- 33.75 MS/s complex float32 IQ (interleaved I/Q)

Net effect:
- Real -> complex conversion
- Two-stage decimation by 2 (overall /4)
- Frequency translation to complex baseband

---

## Conceptual DSP Pipeline

Conceptually, the signal processing consists of four stages:

1. **Stage 1**
   Convert int16 input samples to float and apply a -Fs/4 frequency shift
   (real -> complex).

2. **Stage 2**
   19-tap halfband FIR filter with decimation-by-2.

3. **Stage 3**
   Apply a +Fs/4 frequency shift to re-center the spectrum.

4. **Stage 4**
   235-tap halfband FIR filter (folded) with decimation-by-2.

---

## Stage Fusion Strategy

For performance reasons, the conceptual stages are *not* implemented as four
separate passes.

### Stage 1 + Stage 2 Fusion

Stage 1 is fused into Stage 2 by performing the int16->float conversion and
-Fs/4 frequency shift directly while filling the Stage 2 extended FIR window.
This eliminates:
- One full buffer write
- One full buffer read
- An extra traversal of the data

### Stage 3 + Stage 4 Fusion

Stage 3 is fused into Stage 4 by applying the +Fs/4 frequency shift while
filling the Stage 4 extended FIR window. This avoids an additional standalone
frequency-shift pass and improves cache locality.

The resulting execution order is:

```
(Stage 1 + Stage 2) -> (Stage 3 + Stage 4)
```

---

## FIR and Halfband Design

### Halfband Filters

Both decimation stages use halfband FIR filters:

- Stage 2: 19 taps
- Stage 4: 235 taps

Halfband filters have every other coefficient equal to zero, allowing
**folding** of symmetric coefficient pairs. This reduces the effective number
of multiplications:

- Stage 2: 6 MACs per output sample
- Stage 4: 59 MAC pairs + center tap per output sample

This reduction is essential to achieving real-time performance at 135 MS/s.

---

## SIMD Strategy

The DSP pipeline is vectorized using AVX2 and FMA:

- Structure-of-Arrays (SoA) layout for I and Q data
- AVX2 vectors process 8 float samples at a time
- FMA instructions used in FIR inner loops
- Explicit handling of AVX2 lane semantics using `permute2f128` where required

Scalar fallback code exists only for reference; the program requires AVX2.

---

## Threading Model

The application uses three threads:

1. **Input Thread (main thread)**
   - Reads from stdin
   - Fills buffers from a fixed pool
   - Pushes filled buffers to the processing queue

2. **Processing Thread**
   - Executes the fused DSP pipeline
   - Pushes completed buffers to the output queue

3. **Output Thread**
   - Writes IQ data to stdout or a FIFO
   - Handles downstream disconnect/reconnect gracefully (drops blocks
     while disconnected; see FIFO behavior in rx888_dsp.md)

Threads communicate using lock-free single-producer/single-consumer (SPSC)
queues implemented with C11 atomics.

### Shutdown and Drain Protocol

When `stop_flag` is set (via signal or EOF), all three threads exit their
main loops. However, since the threads see `stop_flag` concurrently, buffers
can be stranded in the queues between stages.

To prevent data loss on shutdown:

1. The **processing thread** drains `filled_queue` after its main loop,
   processing any remaining buffers and pushing results to `ready_queue`.
   It then sets `proc_drain_done` to signal completion.

2. The **output thread** continues draining `ready_queue` until
   `proc_drain_done` is set and the queue is empty, then performs one final
   sweep to handle any push that raced with the flag.

This ensures every block submitted by the input thread is processed and
written before the process exits.

Note: the current implementation uses spin-wait polling with `nanosleep` for
queue consumption. Replacing this with condvar-based wakeup would eliminate
the polling overhead but adds complexity. The current approach is adequate
at 135 MS/s (block budget is ~1.94 ms, poll interval is 100 us).

---

## Buffering and Memory Management

- Fixed-size buffer pool allocated at startup
- No dynamic allocation on the hot path
- Explicit control of backpressure vs. drop behavior
- Memory alignment chosen to support efficient SIMD loads/stores
- SPSC queue indices (`write_idx`, `read_idx`) are placed on separate
  cache lines to prevent false sharing between producer and consumer threads

This ensures predictable latency and avoids allocator-induced jitter.

---

## Error Handling and Robustness

- Fail-fast behavior for invalid usage or unsupported CPUs
- Short reads and broken pipes handled explicitly
- SIGPIPE ignored; reconnect logic handled in output thread
- Optional SIGUSR1 statistics reporting for live inspection

---

## Design Constraints and Non-Goals

This architecture intentionally does **not** support:

- Multiple concurrent pipelines
- Runtime reconfiguration of DSP stages
- Non-x86 platforms
- GPU or accelerator offload

These constraints simplify reasoning about performance and correctness.

---

## Design Philosophy

`rx888_dsp` is designed to behave like a classic Unix utility:
- Simple inputs and outputs
- Explicit failure modes
- Composable with other tools

Performance optimizations were pursued until the pipeline achieved stable
real-time operation with headroom. Further complexity is deferred until
driven by concrete requirements.
