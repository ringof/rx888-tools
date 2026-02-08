# Cross-Project Style Harmonization Plan

## Scope

Three single-file C11 programs that form a pipeline:

```
rx888_stream (USB3 -> stdout) | rx888_dsp (stdin -> stdout) | iqrecord (stdin -> files)
```

All three are maintained by the same developer. The goal is to minimize
stylistic context-switching when moving between files. Changes are
cosmetic and structural only -- no functional/algorithmic changes.

## Files

- `rx888_stream.c` -- ~1263 lines, USB3 bulk streaming driver (libusb, pthreads)
- `rx888_dsp.c` -- ~1370 lines, AVX2/FMA DSP decimation pipeline (3 threads, SPSC queues)
- `iqrecord.c` -- ~656 lines, SigMF-compatible IQ recorder (single-threaded)

All build on Linux x86-64 with gcc or clang, C11 standard.

---

## Change 1: Stop flag type and name

**Problem:** The global stop flag used by signal handlers has three
different declarations across the three files.

| File | Current declaration | Name |
|------|-------------------|------|
| iqrecord.c:31 | `static volatile sig_atomic_t g_stop = 0;` | `g_stop` |
| rx888_dsp.c:146 | `static _Atomic int stop_flag = 0;` | `stop_flag` |
| rx888_stream.c:58 | `static atomic_int g_stop = 0;` | `g_stop` |

`volatile sig_atomic_t` only guarantees atomicity between a signal handler
and the interrupted thread. It does not provide cross-thread visibility.
iqrecord is single-threaded today so this works, but it is the weakest of
the three and inconsistent with the other files.

`atomic_int` (rx888_stream) and `_Atomic int` (rx888_dsp) are equivalent
C11 spellings. Both files already include `<stdatomic.h>`.

**Target state (all three files):**

```c
#include <stdatomic.h>
static _Atomic int g_stop = 0;
```

**Changes required:**

- iqrecord.c:31 -- Change `volatile sig_atomic_t` to `_Atomic int`.
  Rename from `g_stop` is not needed (already named `g_stop`). Add
  `#include <stdatomic.h>` if not already present.
- rx888_dsp.c:146 -- Rename `stop_flag` to `g_stop`. Update all
  references: lines 846, 856, 916, 920, 1122 (signal handler),
  and the processing/output thread loops. Also update
  `proc_drain_done` references that test `stop_flag` in drain loops.
- rx888_stream.c:58 -- Change `atomic_int` to `_Atomic int` for
  spelling consistency. Name is already `g_stop`.

**Verification:** grep for any remaining `stop_flag` or
`volatile sig_atomic_t` across all three files. Build clean with
`-Wall -Wextra -Wpedantic`. For rx888_dsp, run TSan test to confirm
no regressions.

---

## Change 2: Indentation normalization

**Problem:** Three different indent styles.

| File | Current style |
|------|--------------|
| iqrecord.c | 2-space throughout |
| rx888_dsp.c | 4-space throughout |
| rx888_stream.c | Mixed tabs and 4-space |

**Target state:** 4-space indent in all three files. No tabs.

**Changes required:**

- iqrecord.c -- Convert all 2-space indent to 4-space. This is a
  whole-file mechanical change (~162 indented lines). Can be done
  with `sed` or editor reindent, but must be verified manually for
  multi-line strings and alignment that should not change.
- rx888_dsp.c -- Already 4-space. No change.
- rx888_stream.c -- Convert tabs to 4-space. The tab-indented lines
  are concentrated in `main()` option parsing (lines ~860-1010) and
  the post-parse validation block (lines ~1010-1035). Approximately
  151 tab-indented lines. The rest of the file is already 4-space.

**Verification:** `grep -P '\t' *.c` should return zero matches.
Visual inspection of multi-line string literals and alignment columns
(e.g., `fprintf` format strings, struct initializers) to confirm they
were not incorrectly reindented.

---

## Change 3: Signal setup pattern

**Problem:** Three different signal setup approaches.

| File | sigemptyset | sa_flags = 0 | Error check | SIGPIPE method |
|------|------------|--------------|-------------|----------------|
| iqrecord.c:447-453 | yes | yes | yes (die) | none (writes to files) |
| rx888_dsp.c:1235-1259 | yes | yes | no | sigaction(SIG_IGN) |
| rx888_stream.c:851-856 | no | no | no | signal(SIG_IGN) |

rx888_stream relies on `memset(&sa, 0, sizeof(sa))` to zero the mask
and flags, which works but is an implementation detail. It also uses
the deprecated `signal()` function for SIGPIPE.

**Target state (all three files):**

```c
struct sigaction sa;
memset(&sa, 0, sizeof(sa));
sa.sa_handler = on_signal;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, NULL);
sigaction(SIGTERM, &sa, NULL);
```

For SIGPIPE (rx888_dsp, rx888_stream only -- iqrecord writes to files):

```c
sa.sa_handler = SIG_IGN;
sigaction(SIGPIPE, &sa, NULL);
```

No error check on sigaction (cannot fail with valid signal numbers and
a valid sa pointer). Drop iqrecord's `die()` on sigaction failure.

**Changes required:**

- iqrecord.c:447-453 -- Remove die() checks on sigaction calls. Keep
  sigemptyset and sa_flags.
- rx888_dsp.c:1235-1259 -- Rename `signal_handler` to `on_signal`.
  Keep the existing SIGUSR1 handler (`sigusr1_handler`) and its
  separate `struct sigaction su` -- that is rx888_dsp-specific and
  should not be harmonized away. Keep SIGPIPE via sigaction (already
  correct).
- rx888_stream.c:851-856 -- Add `sigemptyset(&sa.sa_mask)` and
  `sa.sa_flags = 0` after memset. Replace `signal(SIGPIPE, SIG_IGN)`
  with `sa.sa_handler = SIG_IGN; sigaction(SIGPIPE, &sa, NULL);`.

**Verification:** Build clean. Test SIGINT/SIGTERM shutdown on all
three programs. For rx888_stream, test broken-pipe handling still
works (`./rx888_stream ... | head -c 1 > /dev/null`).

---

## Change 4: Write helper name and convention

**Problem:** Two different names and return conventions.

| File | Function | Signature | Returns |
|------|----------|-----------|---------|
| iqrecord.c:185 | `write_all` | `(int fd, const void *buf, size_t len)` | 0 / -1 |
| rx888_stream.c:147 | `write_full` | `(int fd, const uint8_t *buf, size_t len)` | 0 / -errno |
| rx888_dsp.c | (inline write loops) | -- | -- |

iqrecord's `write_all` also checks `g_stop` in the loop (early exit on
signal). rx888_stream's `write_full` checks for `n == 0` (returns -EIO).

**Target state:** All files that have a write helper name it `write_all`
with the signature `(int fd, const void *buf, size_t len)` returning
0 on success, -1 on error (caller uses `errno`).

```c
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}
```

The `g_stop` check in iqrecord's version is retained for iqrecord only
(the outer loop already checks `g_stop`, but the write helper provides
an early-exit path mid-write). rx888_stream's writer thread checks
`g_stop` externally after each transfer, so the helper does not need it.

**Changes required:**

- rx888_stream.c:147 -- Rename `write_full` to `write_all`. Change
  return from `-errno` to `-1`. Change `const uint8_t *buf` to
  `const void *buf`. Add cast inside. Update the one call site
  (line ~614) and the SIGPIPE comment (line ~856).
- iqrecord.c:185 -- Add `n == 0` -> EIO check for robustness (minor;
  `write()` returning 0 on a regular fd is pathological but defensive
  coding is consistent). Keep `g_stop` check.
- rx888_dsp.c -- No change now. The inline write loops are embedded
  in thread functions with EPIPE-specific handling (sets stop_flag on
  broken pipe). Extracting them into `write_all` is a future cleanup
  that touches the threading model and is out of scope for this
  cosmetic pass.

**Verification:** Build clean. For rx888_stream, test broken-pipe
exit (`| head -c 1`), normal streaming to `/dev/null`, and SIGINT
mid-stream. For iqrecord, test disk-full (write error) path.

---

## Change 5: Error output functions

**Problem:** Three completely different error output patterns.

| File | Fatal error | Non-fatal warning | Verbose info |
|------|------------|-------------------|--------------|
| iqrecord.c | `die(fmt, ...)` | `warn_msg(fmt, ...)` | none |
| rx888_dsp.c | `LOG_ERR()` macro | `LOG_ERR()` | `LOG_INFO()` macro |
| rx888_stream.c | raw `fprintf(stderr, ...)` | raw `fprintf(stderr, ...)` | `vlogf(level, fmt, ...)` |

Messages are inconsistently prefixed with the program name. iqrecord
hardcodes `"iqrecord: "` in two places but not in `die()` or `warn_msg()`.
rx888_dsp uses a `PROGRAM_NAME` macro. rx888_stream has no prefix in
most messages.

**Target state:** A common set of helpers in all three files:

```c
#define PROG_NAME "iqrecord"  /* per file */

/* Fatal error: print message and exit(1). */
static void die(const char *fmt, ...) {
    fprintf(stderr, PROG_NAME ": ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Non-fatal warning. */
static void warn_msg(const char *fmt, ...) {
    fprintf(stderr, PROG_NAME ": ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
```

For programs with verbose mode (rx888_dsp, rx888_stream), add:

```c
static void info_msg(const char *fmt, ...) {
    if (!g_verbose) return;
    fprintf(stderr, PROG_NAME ": ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
```

rx888_dsp's `LOG_SYSERR(ctx)` becomes `die("%s: %s", ctx, strerror(errno))`
or `warn_msg(...)` depending on whether it's fatal.

**Changes required:**

- iqrecord.c -- Add `PROG_NAME` define. Add prefix to `die()` and
  `warn_msg()`. Remove hardcoded `"iqrecord: "` from the two TTY/write
  error messages (they'll get it from the helper now).
- rx888_dsp.c -- Replace `PROGRAM_NAME` with `PROG_NAME`. Replace
  `LOG_ERR(fmt, ...)` calls with `warn_msg(fmt, ...)` or `die(fmt, ...)`.
  Replace `LOG_INFO(fmt, ...)` calls with `info_msg(fmt, ...)`. Remove
  the macro definitions. Keep `LOG_SYSERR` as an optional convenience
  macro or inline it.
- rx888_stream.c -- Add `PROG_NAME`, `die()`, `warn_msg()`, `info_msg()`.
  Replace raw `fprintf(stderr, ...)` calls. Replace `vlogf(level, ...)`
  with `info_msg()` (most calls are level 0 or 1; the few level-2+
  calls can be guarded with `if (g_verbose >= 2)`). Remove `vlogf()`.

**Verification:** Build clean. Run `--help` and invalid-arg paths on
all three. Confirm all stderr output is prefixed with the program name.
For rx888_stream, confirm verbose output (`-v`) still works at all
verbosity levels.

**Note:** rx888_stream's `vlogf()` supports multi-level verbosity
(level 0/1/2/3). The simple `info_msg()` pattern is a single on/off.
If multi-level verbosity is worth keeping, `info_msg()` can take a
level parameter:

```c
static void info_msg(int level, const char *fmt, ...) {
    if (g_verbose < level) return;
    ...
}
```

This is a design choice. The simpler version (boolean verbose) is
sufficient for iqrecord and rx888_dsp. rx888_stream may want the
leveled version. Either way the name and prefix convention match.

---

## Change 6: Section header comments

**Problem:** Inconsistent section divider style.

| File | Style | Example |
|------|-------|---------|
| iqrecord.c | `// ---- text ----` | Line 20 |
| rx888_dsp.c | None | -- |
| rx888_stream.c | `/* --- text --- */` | Lines 52, 98, 162, 288, etc. |

**Target state:** C-style block comment dividers everywhere:

```c
/* ----------------------------- Section name ------------------------------ */
```

rx888_stream already uses this style extensively and provides a good
template for the section names.

**Changes required:**

- iqrecord.c -- Replace `// ---- text ----` with `/* --- text --- */`.
  Add section headers for major groups: Defaults, Helpers, I/O,
  Metadata, Main.
- rx888_dsp.c -- Add section headers at major boundaries: Coefficients,
  Queue, Buffer pool, DSP kernels, Processing thread, Output thread,
  CLI/main. Currently the file has no visual section breaks.
- rx888_stream.c -- Already done. No change.

**Verification:** Visual inspection only.

---

## Change 7: File header comment

**Problem:** iqrecord has no file header comment. The other two files
have detailed block comments at the top explaining purpose, I/O, and
design.

**Target state:** All three files have a header comment in the same
style:

```c
/*
 * <program_name> (<stdin/device> -> <stdout/files>)
 *
 * Purpose
 *   <one-paragraph description>
 *
 * I/O
 *   Input:  <format and source>
 *   Output: <format and destination>
 *
 * <optional: copyright / license line>
 */
```

**Changes required:**

- iqrecord.c -- Add header comment before `#define _GNU_SOURCE`:

```c
/*
 * iqrecord (stdin -> SigMF files)
 *
 * Purpose
 *   High-throughput IQ recorder for continuous capture at >=135 MS/s.
 *   Reads cf32_le IQ samples from stdin, splits into fixed-size files
 *   with SigMF-compatible metadata, and writes a session-level run.json.
 *
 * I/O
 *   Input:  complex float32 IQ (cf32_le) on stdin.
 *   Output: cap_NNNNNN.sigmf-data + .sigmf-meta files, plus run.json.
 */
```

- rx888_dsp.c -- Already has a detailed header. No change (or minor
  formatting to match the template above if desired).
- rx888_stream.c -- Already has a detailed header. Update the copyright
  line if needed (currently says "OpenAI assistant" for the rewrite).

**Verification:** Visual inspection only.

---

## Change 8: iqrecord `aligned_alloc` sizing (iqrecord only)

**Problem:** C11 §7.22.3.1 requires the `size` argument to
`aligned_alloc` to be a multiple of `alignment`. The current code
(iqrecord.c:465-467):

```c
const size_t buf_bytes = block_bytes_want + (size_t)BYTES_PER_SAMPLE - 1;
uint8_t *buf = (uint8_t *)aligned_alloc(4096, buf_bytes);
```

`block_bytes_want` is 32 MiB (33,554,432). `buf_bytes` is
33,554,439 (32 MiB + 7), which is not a multiple of 4096. Glibc
happens to tolerate this, but musl and other conforming
implementations will return NULL.

**Target state:**

```c
const size_t buf_bytes_raw = block_bytes_want + (size_t)BYTES_PER_SAMPLE - 1;
const size_t buf_bytes = (buf_bytes_raw + 4095) & ~(size_t)4095;
uint8_t *buf = (uint8_t *)aligned_alloc(4096, buf_bytes);
```

Alternatively, replace with `posix_memalign`:

```c
uint8_t *buf = NULL;
if (posix_memalign((void **)&buf, 4096, buf_bytes_raw) != 0 || !buf)
    die("posix_memalign failed");
```

`posix_memalign` requires alignment to be a power of two and a
multiple of `sizeof(void *)`, but does not require `size` to be a
multiple of alignment. It is the more portable choice.

**Changes required:**

- iqrecord.c:465-468 -- Replace `aligned_alloc` with
  `posix_memalign`, or round `buf_bytes` up to 4096 before calling
  `aligned_alloc`.

**Verification:** Build clean. Run under valgrind. Test on musl-based
system (e.g., Alpine container) if available. Confirm no change in
behavior on glibc.

---

## Change 9: iqrecord `write_all` stop-flag semantics (iqrecord only)

**Problem:** `write_all` (iqrecord.c:185) returns 0 (success) when
`g_stop` is set mid-write:

```c
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        if (g_stop) return 0;   /* <-- reports success, bytes not written */
        ssize_t n = write(fd, p, len);
        ...
    }
    return 0;
}
```

The caller (line 545) treats return 0 as "all bytes written" and
advances `samples_total` and `samples_in_file` by the full
`to_write_samples`. If `g_stop` fires between two `write()` calls
inside `write_all`, the counters overcount. The resulting
`.sigmf-meta` and `run.json` will report more samples than were
actually written to disk.

In practice the error is bounded by one partial `write()` call
(a few hundred KB in a multi-GB capture), so this is an imprecision
rather than a catastrophic corruption. But it is fixable.

**Target state:** Remove the `g_stop` check from `write_all`. The
helper becomes a pure I/O function:

```c
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}
```

Stop checking moves to the outer loop, which already has
`if (g_stop) break;` at its top (line 495). When SIGINT arrives,
`write()` inside `write_all` will either complete the current
syscall or return -1 with `errno == EINTR` (which the helper
retries). The outer loop breaks on the next iteration. This means
any write that starts will finish atomically, so counters stay
accurate.

**Changes required:**

- iqrecord.c:185-199 -- Remove `if (g_stop) return 0;` from
  `write_all`. Add `n == 0` check.

**Verification:** Build clean. Test Ctrl-C during a long capture
(counter_cf32 | iqrecord). Verify `run.json` `bytes_written` matches
sum of `.sigmf-data` file sizes. Run the existing
`kill_iqrecord_test.sh` with `sigint` and `sigterm`.

---

## Change 10: iqrecord `run.json` structure cleanup (iqrecord only)

**Problem:** The `run.json` contains a top-level `"accounting"` object
with zeroed counters written at file open time (iqrecord.c:334-338),
followed by `"final_accounting"` with real totals appended at close
(line 396). The zero block is never updated and serves no purpose.
It reads like a workaround for streaming JSON without buffering.

Current output:
```json
{
  "schema": "iqrecord.run.v1",
  ...
  "accounting": {
    "files_written": 0,
    "samples_written": 0,
    "bytes_written": 0
  },
  "files": [ ... ],
  "final_accounting": {
    "files_written": 3,
    "samples_written": 1012500000,
    "bytes_written": 8100000000
  }
}
```

**Target state:** Remove the zero `"accounting"` block. Rename
`"final_accounting"` to `"accounting"`:

```json
{
  "schema": "iqrecord.run.v1",
  ...
  "files": [ ... ],
  "accounting": {
    "files_written": 3,
    "samples_written": 1012500000,
    "bytes_written": 8100000000
  }
}
```

**Changes required:**

- iqrecord.c:334-338 (`open_run_json`) -- Remove the six lines that
  write the zero `"accounting"` block and its trailing comma.
- iqrecord.c:396 (`finalize_run_json`) -- Change
  `"final_accounting"` to `"accounting"`.

**Verification:** Build clean. Run a short capture, confirm `run.json`
parses with `jq`, confirm there is exactly one `"accounting"` key
with correct totals. Run `kill_iqrecord_test.sh` with `sigint` to
confirm finalization still works.

---

## Change 11: iqrecord atomic metadata writes (iqrecord only)

**Problem:** A crash (SIGKILL, power loss) during `write_sigmf_meta`
or `finalize_run_json` can leave truncated `.sigmf-meta` or
`run.json` files on disk. These are indistinguishable from valid
files without parsing them.

This cannot be prevented for SIGKILL, but the window of corruption
can be narrowed to nearly zero by using atomic write-then-rename.

**Target state:** Metadata files are written to a `.tmp` suffix and
renamed on completion:

```c
/* In write_sigmf_meta: */
snprintf(tmp_path, ..., "%s/cap_%06" PRIu64 ".sigmf-meta.tmp", ...);
FILE *f = fopen(tmp_path, "wb");
/* ... write contents ... */
fclose(f);
rename(tmp_path, final_path);

/* In open_run_json / finalize_run_json: */
/* Write to run.json.tmp throughout, rename in finalize_run_json. */
```

`rename()` is atomic on POSIX filesystems (ext4, xfs, btrfs). After
rename, the file is either the complete new version or the old
version (or absent, if no prior version existed). A truncated `.tmp`
file is clearly identifiable as incomplete.

**Changes required:**

- iqrecord.c `write_sigmf_meta` (line 219) -- Write to
  `.sigmf-meta.tmp`, then `rename()` to `.sigmf-meta`.
- iqrecord.c `open_run_json` (line 293) -- Open `run.json.tmp`
  instead of `run.json`.
- iqrecord.c `finalize_run_json` (line 389) -- After `fclose()`,
  `rename("run.json.tmp", "run.json")`.

**Verification:** Build clean. Run a capture, confirm final files
have no `.tmp` suffix. `ls` the output directory during a capture
to confirm `.tmp` files exist transiently. Run SIGKILL test and
confirm no truncated `.sigmf-meta` or `run.json` (only `.tmp`
artifacts if anything).

---

## Change 12: iqrecord usage string (iqrecord only)

**Problem:** The `--help` / usage string (iqrecord.c:411-413) does
not mention `--fsync`, which is a supported option parsed at
line 432:

```c
fprintf(stderr,
    "usage: %s OUTDIR [--freq HZ] [--desc TEXT]\n"
    "MVP defaults: ...\n",
    argv[0]);
```

**Target state:**

```c
fprintf(stderr,
    "usage: %s OUTDIR [--freq HZ] [--desc TEXT] [--fsync]\n"
    "defaults: datatype=cf32_le Fs=33750000 samples_per_file=337500000 block_samples=4194304\n",
    argv[0]);
```

Also replace "MVP defaults" with "defaults" (the "MVP" qualifier is
development-internal language that doesn't belong in user-facing
output).

**Changes required:**

- iqrecord.c:411-413 -- Add `[--fsync]` to usage line. Change
  "MVP defaults" to "defaults".

**Verification:** `./iqrecord` with no args shows updated usage.
`./iqrecord --fsync` with no OUTDIR still shows usage (not a crash).

---

## Change 13: rx888_stream backpressure documentation (rx888_stream only)

**Problem:** When the internal transfer queue fills (downstream stalled
long enough that all completed USB transfers are queued waiting for the
writer thread), the callback sets `g_stop` and exits immediately. This
is the correct behavior for a real-time USB driver — blocking the
callback would wedge libusb's event loop, and dropping samples silently
would be worse. But this policy is not documented in the README.

The code comments (lines 571-578 of rx888_stream.c) explain it:

```c
/* Downstream stalled enough to fill the queue: fail-fast.
   This prevents blocking libusb event handling (which is worse).
   Wake the writer thread so it can exit promptly. */
```

**Target state:** Add a note to the README Notes section:

```
- If downstream stalls long enough to fill the internal transfer queue
  (queue depth x transfer size), rx888_stream exits rather than blocking
  the USB event loop or silently dropping samples. Increase -q or fix
  the downstream bottleneck.
```

**Changes required:**

- rx888_stream README.md Notes section -- Add the backpressure note.

**Verification:** Review only.

---

## Change 14: rx888_stream verbose memory budget (rx888_stream only)

**Problem:** Debugging USB buffer issues requires knowing the total
in-flight memory and how it relates to `usbfs_memory_mb`. Currently
the user must compute this manually from `-p` and `-q`. With `-v`,
the program should print this once at startup.

**Target state:** After option parsing and before `libusb_init`, when
`g_verbose >= 1`, print a one-time summary:

```
rx888_stream: samplerate=135000000 Hz
rx888_stream: transfer_size=1048576 bytes (1024 packets x 1024 bytes)
rx888_stream: queue_depth=32, total_inflight=33554432 bytes (32.00 MiB)
rx888_stream: minimum usbfs_memory_mb=32
```

Implementation sketch:

```c
if (g_verbose) {
    unsigned int pktsize = 1024; /* USB3 bulk max; known after endpoint pick */
    uint64_t xfer_bytes = (uint64_t)g_req_packets * pktsize;
    uint64_t total_bytes = xfer_bytes * g_queue_depth;
    info_msg("samplerate=%u Hz", g_samplerate);
    info_msg("transfer_size=%" PRIu64 " bytes (%u packets x %u bytes)",
             xfer_bytes, g_req_packets, pktsize);
    info_msg("queue_depth=%u, total_inflight=%" PRIu64 " bytes (%.2f MiB)",
             g_queue_depth, total_bytes, (double)total_bytes / (1024.0 * 1024.0));
    info_msg("minimum usbfs_memory_mb=%" PRIu64, (total_bytes + (1<<20) - 1) >> 20);
}
```

Note: the exact packet size (1024 for USB3, 512 for USB2) is only known
after endpoint discovery. The printout can be split: print samplerate
and queue depth early, print transfer sizing after `pick_bulk_in_endpoint`
returns the actual max packet size.

**Changes required:**

- rx888_stream.c -- Add verbose startup printout after endpoint
  discovery, using the actual packet size from the endpoint descriptor.

**Verification:** Run `./rx888_stream -v -s 135000000 -q 32 -p 1024 > /dev/null`
(will fail at device open, but verbose output should appear before that
if placed after option parsing). With hardware: confirm values match
expected sizing.

---

## Change 15: rx888_dsp AVX2/FMA runtime check (rx888_dsp only)

**Problem:** The README states the program "requires AVX2 + FMA and
will not run otherwise" and ARCHITECTURE.md lists "fail-fast behavior
for unsupported CPUs." But there is no runtime check. If the binary
(compiled with `-mavx2 -mfma`) runs on a non-AVX2 CPU, it crashes
with `SIGILL` — an opaque illegal instruction fault, not a clear
diagnostic.

**Target state:** Add a check early in `main()`, before any DSP code
runs:

```c
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) {
        LOG_ERR("this CPU lacks AVX2+FMA support (required)\n");
        return EXIT_USAGE;
    }
#endif
```

This uses `__builtin_cpu_supports`, which is available in both gcc
(>= 4.8) and clang (>= 3.7). The `#if` guard ensures the code
compiles on other compilers (it just skips the check, which is the
same as today's behavior).

Place this immediately after argument parsing and TTY checks, before
`init_hb2_coeffs()` (line 1262) — this is the first point where AVX2
instructions could execute.

**Changes required:**

- rx888_dsp.c -- Add the 5-line check after TTY checks (around
  line 1233), before `init_hb2_coeffs()`.

**Verification:** Build clean on an AVX2 system. Test the check by
temporarily changing "avx2" to "avx512f" (or similar unsupported
feature) — should print the error and exit 2. Revert and confirm
normal operation.

---

## Change 16: rx888_dsp FIFO disconnect and loss-policy docs (rx888_dsp only)

**Problem:** The README and ARCHITECTURE.md describe FIFO handling
and shutdown drain, but do not spell out the actual loss policy:

- What happens when the FIFO has no reader at startup? (Blocks are
  dropped and counted.)
- What happens when the reader disconnects mid-stream? (EPIPE closes
  the fd; subsequent blocks are dropped until a new reader connects.)
- What happens during shutdown drain with no reader? (Blocks are
  dropped — line 970: "No FIFO reader — drop during drain.")
- Does `--block-on-full` affect FIFO writer availability? (No — it
  controls input→processing backpressure only.)

The code handles all of these correctly. The documentation doesn't
explain them.

**Target state:** Add a "FIFO behavior" subsection to the README
Notes section:

```
- When using -o PATH (FIFO output), blocks are dropped if no reader
  is connected. Drop counts are reported via SIGUSR1 and at shutdown.
  To avoid startup drops, start the FIFO consumer before the producer.
- If the reader disconnects mid-stream (EPIPE), rx888_dsp closes the
  FIFO and attempts to reconnect. Blocks are dropped while
  disconnected.
- On shutdown (signal or EOF), queued blocks are drained through the
  pipeline. If the FIFO has no reader during drain, remaining blocks
  are dropped rather than blocking the exit.
- --block-on-full controls backpressure between the input reader and
  the processing thread. It does not affect FIFO writer behavior.
```

Also tighten the "no silent data loss" claim in the Signals section
to scope it to "shutdown while output is writable."

**Changes required:**

- rx888_dsp README.md Notes section -- Add FIFO behavior notes.
- rx888_dsp README.md Signals section -- Change "no silent data loss"
  to "no silent data loss while output is connected."
- rx888_dsp ARCHITECTURE.md -- Update the "gracefully" description
  in the output thread section to reference the drop-while-disconnected
  policy.

**Verification:** Review only. Confirm notes match observed behavior
by running with `-v -o /tmp/test.fifo` with no reader, then starting
a reader mid-stream, then killing the reader.

---

## Change order

Recommended execution order to minimize merge conflicts and allow
incremental testing:

1. **Change 8** (aligned_alloc) -- real portability bug, one-site fix
2. **Change 9** (write_all g_stop) -- pairs with Change 8 since both
   touch iqrecord I/O helpers
3. **Change 1** (stop flag type) -- smallest cross-project change,
   high correctness value
4. **Change 3** (signal setup) -- pairs naturally with Change 1
5. **Change 4** (write helper name) -- isolated rename + convention
6. **Change 12** (usage string) -- trivial, no risk
7. **Change 10** (run.json structure) -- small, isolated to one function
8. **Change 11** (atomic metadata) -- pairs with Change 10 since both
   touch the same metadata write paths
9. **Change 15** (AVX2 runtime check) -- small, isolated, high value
10. **Change 16** (FIFO disconnect docs) -- README/ARCHITECTURE only
11. **Change 13** (backpressure doc) -- README-only, no code
12. **Change 14** (memory budget) -- isolated addition, no refactoring
13. **Change 5** (error output) -- moderate effort, touches many lines
14. **Change 7** (file header) -- trivial, no code impact
15. **Change 6** (section headers) -- trivial, no code impact
16. **Change 2** (indentation) -- do last because it touches every line
    and creates the largest diff; easier to review if all semantic
    changes are already committed

Grouping suggestions:
- Changes 8, 9, 12 as an "iqrecord correctness" commit.
- Changes 10, 11 as an "iqrecord metadata cleanup" commit.
- Changes 1, 3, 4 as a "cross-project signal and I/O" commit.
- Changes 15, 16 as an "rx888_dsp runtime check and docs" commit.
- Changes 13, 14 as an "rx888_stream docs and diagnostics" commit.
- Changes 5, 6, 7 as a "cross-project comments and diagnostics" commit.
- Change 2 as a standalone "indent normalization" commit.

---

## Out of scope

The following differences were observed but are intentionally not
harmonized:

- **rx888_dsp's SPSC queue vs. rx888_stream's mutex+condvar queue.**
  These reflect genuinely different threading models (lock-free vs.
  blocking). Not a style issue.
- **rx888_dsp's inline write loops.** Extracting to `write_all` would
  change EPIPE handling semantics in the output thread. Deferred.
- **rx888_stream's `vlogf` multi-level verbosity.** If multi-level
  is worth keeping, the leveled `info_msg(level, ...)` variant is
  the harmonized form. Decision deferred to implementation.
- **iqrecord's `die()` call in `write_all` removal.** Already done in
  a prior fix (Fix 5: graceful write error handling). The current
  `write_all` returns -1 and caller handles it. No further change.
- **Include order.** All three files use alphabetical system includes.
  Minor differences in which headers are included reflect actual
  dependencies. Not worth normalizing.
- **iqrecord rotation loop refactor.** A reviewer suggested reshaping
  the main write/rotate loop into a single pattern. The current
  structure was deliberately established in prior fixes (Fix 1:
  carry-over logic, Fix 5: write error handling). The "spillover vs
  exact-boundary vs deferred-open" branches represent genuinely
  different states. The reviewer's proposed pseudocode is essentially
  what the code already does. No change unless a concrete bug or
  readability failure is identified.
- **iqrecord partial-write accounting.** If `write()` partially
  succeeds then fails, counters could be slightly off. In practice
  `write()` on a regular file either completes fully or fails on the
  first call (disk full). Partial writes on regular fds are extremely
  rare outside NFS. Change 9 (removing g_stop from write_all) is the
  more impactful fix for accounting accuracy. Byte-level tracking
  inside write_all is deferred as over-engineering for the current
  use case.
- **rx888_stream teardown leak fallback.** A reviewer proposed
  per-transfer state tracking to replace the "leak to avoid UAF"
  path in `stream_teardown`. The leak exists because after
  `libusb_cancel_transfer`, libusb owns the transfer struct until
  it fires the completion callback. If the callback never fires
  (device yanked, host controller wedged), there is no safe way to
  free that memory. The current approach (wait 2s, then leak and
  let process exit reclaim) is correct. Per-transfer state tracking
  would add complexity without changing this fundamental constraint.
- **rx888_stream cross-thread libusb resubmission.** A reviewer
  proposed moving `libusb_submit_transfer` from the writer thread
  to the event thread. The code already documents (lines 625-632)
  why cross-thread submission is safe on Linux's default backend
  and would not be safe on other platforms. This is a Linux-only
  USB3 SDR driver. Adding a second queue for resubmission would
  increase latency in the hot path for no practical benefit.
- **rx888_stream `g_stop` replacement with stop-reason bitmask.**
  A reviewer proposed an atomic bitmask (`STOP_SIG | STOP_PIPE |
  STOP_WATCHDOG | ...`). Each stop site already prints a specific
  diagnostic before setting `g_stop`, so the reason is already
  visible in stderr output. The bitmask would allow cleaner exit
  reporting but adds indirection at every `g_stop` test site.
  Optional enhancement during Change 1 if desired, but not required.
- **rx888_stream backpressure policy options.** A reviewer suggested
  `--queue-policy=drop|stop` or `--max-stall-ms`. The current
  fail-fast policy (exit on queue full) is the only correct behavior
  for a real-time USB driver: blocking the callback wedges libusb,
  and silent drops corrupt the stream. No configuration needed.
- **ezusb.c hardening and license separation.** A reviewer suggested
  splitting ezusb into a separate executable for MIT/GPL isolation
  and hardening firmware parsing. ezusb is vendor-inherited code
  that works. Splitting it worsens the user workflow (two-step
  instead of one). Firmware parsing hardening is high risk, low
  reward for code that processes a single known-good firmware image.
- **rx888_dsp sentinel-based shutdown.** A reviewer proposed replacing
  the `proc_drain_done` flag with NULL sentinel buffers pushed through
  the queues. This is a valid alternative but not obviously simpler.
  The sentinel check would be spread across every `spsc_pop` site.
  The current flag is set in one place and checked in one place. The
  "final sweep" in the output thread exists because of acquire/release
  vs seq_cst ordering — a sentinel does not eliminate this race, it
  just moves it. The current code works, passes TSan, and has a
  comment explaining the ordering concern.
- **rx888_dsp nanosleep polling replacement.** A reviewer proposed
  replacing nanosleep-based SPSC polling with eventfd or condvar
  wakeups. This is a real improvement for latency and CPU efficiency,
  but it is a significant refactor touching both the SPSC queue
  implementation and all three threads. The current polling works at
  proven throughput (135 MS/s). ARCHITECTURE.md already contains a
  breadcrumb noting this as a future improvement.
- **rx888_dsp output state machine.** A reviewer proposed explicit
  states (`OUTPUT_STDOUT`, `OUTPUT_FIFO_WAIT_READER`,
  `OUTPUT_FIFO_CONNECTED`, `OUTPUT_ERROR_SHUTDOWN`). The current code
  handles these states via conditions (`output_fd < 0 && output_path`,
  etc.) in a ~60-line loop. An enum adds indirection without changing
  behavior.
- **rx888_dsp struct config / centralized stats / buffer state enum.**
  A reviewer proposed consolidating globals into structs and adding
  a per-buffer state enum. These are style preferences that would
  touch many function signatures without reducing bug surface. The
  current globals work for single-instance programs, stats are already
  in atomics, and buffer ownership is tracked implicitly by queue
  membership (which is the standard SPSC pattern).
- **rx888_dsp partial-read / sample-alignment.** A reviewer flagged
  potential mid-sample short reads. The `read_full` helper already
  loops until it gets exactly `INPUT_SAMPLES * sizeof(int16_t)` bytes.
  Mid-block EOF is detected and reported ("Input stream ended
  mid-block"). Already handled correctly.

---

## Appendix A: File inventory

### Source files (all single-file C11 programs)

| Program | Source | Headers | Build command |
|---------|--------|---------|---------------|
| rx888_stream | `rx888_stream.c` | `ezusb.c`, `ezusb.h`, `rx888.h` | `gcc -O2 -Wall -Wextra -std=c11 -pthread rx888_stream.c ezusb.c -lusb-1.0 -o rx888_stream` |
| rx888_dsp | `rx888_dsp.c` | (none) | `gcc -O3 -mavx2 -mfma -Wall -Wextra -std=c11 rx888_dsp.c -lm -lpthread -o rx888_dsp` |
| iqrecord | `iqrecord.c` | (none) | `gcc -O3 -Wall -Wextra -std=c11 iqrecord.c -o iqrecord` |

The `Makefile` builds all three. `99-rx888.rules` is a udev rules
file for USB device permissions. `rx888_dsp_to_gqrx.sh` is a helper
script for piping rx888_dsp output to GQRX via FIFO.

### Documentation files

Output filenames use a prefix to avoid collisions since all three
projects share one output directory:

| Program | README | Companion doc |
|---------|--------|---------------|
| rx888_stream | `rx888_stream_README.md` | `rx888_stream_TEST_PLAN.md` |
| rx888_dsp | `README.md` | `ARCHITECTURE.md` |
| iqrecord | `iqrecord_README.md` | `iqrecord_TEST_PLAN.md` |

rx888_dsp's files are unprefixed because they were the first to be
consolidated (before the naming convention was established). This is
cosmetic and does not affect content.

### Fix history

Each program went through a prior fix round documented in:

| Program | Fix log | Fixes applied |
|---------|---------|---------------|
| rx888_stream | `rx888_stream_fixes.md` | 15 fixes (build, races, teardown, robustness) |
| rx888_dsp | `rx888_dsp_fixes.md` | 6 fixes (help text, data races, false sharing, SIGUSR1) |
| iqrecord | `iqrecord_fixes.md` | 7 fixes (data corruption, naming, TTY, write errors, indentation) |

The harmonization plan references specific prior fixes in the
Out of Scope section (e.g., "Fix 1: carry-over logic" and "Fix 5:
graceful write error handling" for iqrecord). The fix logs contain
full details.

### Test scripts (iqrecord)

| File | Purpose |
|------|---------|
| `counter_cf32.py` | Generates cf32_le IQ with incrementing I counter (test data source) |
| `verify_cf32.py` | Verifies file sizes, per-file counters, boundary continuity |
| `kill_generator_test.sh` | Tests iqrecord finalization when producer dies mid-stream |
| `kill_iqrecord_test.sh` | Tests iqrecord finalization under SIGINT/SIGTERM/SIGKILL |

### External review documents

Three external code reviews were triaged during planning. The
triage decisions are captured in the change list and out-of-scope
section, but the original reviews provide additional context:

| Review | Covers | Actionable items taken |
|--------|--------|-----------------------|
| (iqrecord review, provided in-chat) | iqrecord.c correctness and structure | Changes 8, 9, 10, 11, 12 |
| `CHANGES_REVIEW_rx888_stream_ezusb.md` | rx888_stream + ezusb | Changes 13, 14 |
| `CHANGES_REVIEW_rx888_dsp.md` | rx888_dsp | Changes 15, 16 |

---

## Appendix B: Applied vs pending changes

Changes already applied to files in the output directory (do not
re-apply):

| Change | File modified | What was done |
|--------|---------------|---------------|
| 13 (backpressure doc) | `rx888_stream_README.md` | Added backpressure policy note to Notes section |

All other changes (1–12, 14–16) are pending.

When implementing changes, update this table so a new chat knows
what has been done.

---

## Appendix C: Resuming in a new chat

To resume this work in a new chat, upload:

1. This plan (`STYLE_HARMONIZATION_PLAN.md`)
2. The three C source files: `rx888_stream.c`, `rx888_dsp.c`,
   `iqrecord.c`
3. The six documentation files: `rx888_stream_README.md`,
   `rx888_stream_TEST_PLAN.md`, `README.md`, `ARCHITECTURE.md`,
   `iqrecord_README.md`, `iqrecord_TEST_PLAN.md`

Optional (for reference if questions arise about prior fixes):
4. The three fix logs: `rx888_stream_fixes.md`, `rx888_dsp_fixes.md`,
   `iqrecord_fixes.md`

The plan is self-contained: each change specifies the problem,
target state, file locations with line numbers, implementation
details, and verification steps. Start from the change order
section and work through pending changes in sequence.
