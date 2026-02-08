#!/usr/bin/env bash
set -euo pipefail

# Create GitHub issues for each item in the style harmonization plan.
# Run from anywhere with gh authenticated:
#   ./scripts/create_style_issues.sh
#
# Prerequisites:
#   - gh CLI installed and authenticated (gh auth status)
#   - Write access to the target repo
#
# Issues are created in the recommended execution order from
# doc/style_harmonization.md (bugs first, then cross-project,
# then cosmetic).

REPO="ringof/rx888_tools"
DELAY=2  # seconds between API calls to avoid rate limiting

created=0
failed=0

create_issue() {
    local title="$1"
    local labels="$2"
    local body="$3"

    echo "Creating: $title"
    if gh issue create --repo "$REPO" --title "$title" --label "$labels" --body "$body"; then
        created=$((created + 1))
    else
        echo "  FAILED: $title" >&2
        failed=$((failed + 1))
    fi
    sleep "$DELAY"
}

# Ensure labels exist (gh will error if they don't; create them first)
echo "=== Ensuring labels exist ==="
gh label create "bug"           --repo "$REPO" --color "d73a4a" --description "Something isn't working"                  2>/dev/null || true
gh label create "style"         --repo "$REPO" --color "0e8a16" --description "Code style harmonization"                 2>/dev/null || true
gh label create "documentation" --repo "$REPO" --color "0075ca" --description "Documentation improvements"               2>/dev/null || true
gh label create "iqrecord"      --repo "$REPO" --color "c5def5" --description "Affects iqrecord"                         2>/dev/null || true
gh label create "rx888_stream"  --repo "$REPO" --color "bfdadc" --description "Affects rx888_stream"                     2>/dev/null || true
gh label create "rx888_dsp"     --repo "$REPO" --color "d4c5f9" --description "Affects rx888_dsp"                        2>/dev/null || true
gh label create "cross-project" --repo "$REPO" --color "fbca04" --description "Affects multiple files"                   2>/dev/null || true

echo ""
echo "=== Creating issues (recommended execution order) ==="
echo ""

# ── 1. Change 8: aligned_alloc portability bug ──────────────────────────

create_issue \
  "iqrecord: aligned_alloc size not a multiple of alignment (portability bug)" \
  "bug,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 8

**Priority:** High — real portability bug
**Scope:** `iqrecord.c` only
**Type:** Bug fix

### Problem

C11 §7.22.3.1 requires the `size` argument to `aligned_alloc` to be a multiple of `alignment`. The current code (`iqrecord.c:465-467`):

```c
const size_t buf_bytes = block_bytes_want + (size_t)BYTES_PER_SAMPLE - 1;
uint8_t *buf = (uint8_t *)aligned_alloc(4096, buf_bytes);
```

`block_bytes_want` is 32 MiB (33,554,432). `buf_bytes` is 33,554,439 (32 MiB + 7), which is **not** a multiple of 4096. Glibc happens to tolerate this, but musl and other conforming implementations will return NULL.

### Target State

Either round up to the next 4096 multiple:

```c
const size_t buf_bytes_raw = block_bytes_want + (size_t)BYTES_PER_SAMPLE - 1;
const size_t buf_bytes = (buf_bytes_raw + 4095) & ~(size_t)4095;
uint8_t *buf = (uint8_t *)aligned_alloc(4096, buf_bytes);
```

Or replace with `posix_memalign` (doesn't require size to be a multiple of alignment):

```c
uint8_t *buf = NULL;
if (posix_memalign((void **)&buf, 4096, buf_bytes_raw) != 0 || !buf)
    die("posix_memalign failed");
```

### Verification

- Build clean with `-Wall -Wextra -Wpedantic`
- Run under valgrind
- Test on musl-based system (e.g., Alpine container) if available
- `python3 tests/counter_cf32.py --limit-samples 10000000 | ./iqrecord /tmp/test_ch8`
- `python3 tests/verify_cf32.py "/tmp/test_ch8/*.sigmf-data"`

### Reference

`doc/style_harmonization.md`, Change 8
EOF
)"

# ── 2. Change 9: write_all stop-flag semantics ──────────────────────────

create_issue \
  "iqrecord: write_all returns success when g_stop set mid-write (counter overcount)" \
  "bug,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 9

**Priority:** High — data accuracy bug
**Scope:** `iqrecord.c` only
**Type:** Bug fix

### Problem

`write_all` (`iqrecord.c:185`) returns 0 (success) when `g_stop` is set mid-write:

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

The caller treats return 0 as "all bytes written" and advances `samples_total` and `samples_in_file` by the full `to_write_samples`. If `g_stop` fires between two `write()` calls, counters overcount. The resulting `.sigmf-meta` and `run.json` will report more samples than were actually written to disk.

### Target State

Remove the `g_stop` check from `write_all`. The helper becomes a pure I/O function:

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

Stop checking moves to the outer loop (which already has `if (g_stop) break;`).

### Verification

- Build clean
- Ctrl-C during a long capture: `python3 tests/counter_cf32.py | ./iqrecord /tmp/test_ch9`
- Verify `run.json` `bytes_written` matches sum of `.sigmf-data` file sizes
- Run `tests/kill_iqrecord_test.sh` with `sigint` and `sigterm`

### Reference

`doc/style_harmonization.md`, Change 9
EOF
)"

# ── 3. Change 1: Stop flag type and name ────────────────────────────────

create_issue \
  "Harmonize stop flag type and name across all three programs" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 1

**Priority:** Medium — correctness and consistency
**Scope:** All three files
**Type:** Style harmonization

### Problem

The global stop flag has three different declarations:

| File | Current declaration | Name |
|------|-------------------|------|
| `iqrecord.c:31` | `static volatile sig_atomic_t g_stop = 0;` | `g_stop` |
| `rx888_dsp.c:146` | `static _Atomic int stop_flag = 0;` | `stop_flag` |
| `rx888_stream.c:58` | `static atomic_int g_stop = 0;` | `g_stop` |

`volatile sig_atomic_t` only guarantees atomicity between a signal handler and the interrupted thread — not cross-thread visibility. `atomic_int` and `_Atomic int` are equivalent C11 spellings.

### Target State (all three files)

```c
#include <stdatomic.h>
static _Atomic int g_stop = 0;
```

### Changes Required

- `iqrecord.c:31` — Change `volatile sig_atomic_t` to `_Atomic int`. Add `#include <stdatomic.h>`.
- `rx888_dsp.c:146` — Rename `stop_flag` to `g_stop`. Update all references (lines 846, 856, 916, 920, 1122, and thread loops).
- `rx888_stream.c:58` — Change `atomic_int` to `_Atomic int`.

### Verification

- `grep` for any remaining `stop_flag` or `volatile sig_atomic_t`
- Build clean with `-Wall -Wextra -Wpedantic`
- For `rx888_dsp`, run TSan test to confirm no regressions

### Reference

`doc/style_harmonization.md`, Change 1
EOF
)"

# ── 4. Change 3: Signal setup pattern ───────────────────────────────────

create_issue \
  "Harmonize signal setup pattern across all three programs" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 3

**Priority:** Medium — correctness
**Scope:** All three files
**Type:** Style harmonization

### Problem

Three different signal setup approaches:

| File | sigemptyset | sa_flags = 0 | Error check | SIGPIPE method |
|------|------------|--------------|-------------|----------------|
| `iqrecord.c:447-453` | yes | yes | yes (die) | none |
| `rx888_dsp.c:1235-1259` | yes | yes | no | sigaction(SIG_IGN) |
| `rx888_stream.c:851-856` | no | no | no | signal(SIG_IGN) |

`rx888_stream` relies on `memset` to zero the mask and flags (works but is an implementation detail) and uses the deprecated `signal()` for SIGPIPE.

### Target State (all three files)

```c
struct sigaction sa;
memset(&sa, 0, sizeof(sa));
sa.sa_handler = on_signal;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, NULL);
sigaction(SIGTERM, &sa, NULL);
```

For SIGPIPE (`rx888_dsp`, `rx888_stream` only):

```c
sa.sa_handler = SIG_IGN;
sigaction(SIGPIPE, &sa, NULL);
```

### Changes Required

- `iqrecord.c:447-453` — Remove `die()` checks on sigaction calls.
- `rx888_dsp.c:1235-1259` — Rename `signal_handler` to `on_signal`. Keep the SIGUSR1 handler and its separate `struct sigaction su`.
- `rx888_stream.c:851-856` — Add `sigemptyset(&sa.sa_mask)` and `sa.sa_flags = 0`. Replace `signal(SIGPIPE, SIG_IGN)` with sigaction equivalent.

### Verification

- Build clean
- Test SIGINT/SIGTERM shutdown on all three programs
- For `rx888_stream`, test broken-pipe: `./rx888_stream ... | head -c 1 > /dev/null`

### Reference

`doc/style_harmonization.md`, Change 3
EOF
)"

# ── 5. Change 4: Write helper name and convention ───────────────────────

create_issue \
  "Harmonize write helper name and return convention" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 4

**Priority:** Low-medium
**Scope:** `iqrecord.c`, `rx888_stream.c`
**Type:** Style harmonization

### Problem

Two different names and return conventions:

| File | Function | Signature | Returns |
|------|----------|-----------|---------|
| `iqrecord.c:185` | `write_all` | `(int fd, const void *buf, size_t len)` | 0 / -1 |
| `rx888_stream.c:147` | `write_full` | `(int fd, const uint8_t *buf, size_t len)` | 0 / -errno |
| `rx888_dsp.c` | (inline write loops) | — | — |

### Target State

All files with a write helper name it `write_all` with signature `(int fd, const void *buf, size_t len)` returning 0 on success, -1 on error (caller uses `errno`).

### Changes Required

- `rx888_stream.c:147` — Rename `write_full` to `write_all`. Change return from `-errno` to `-1`. Change `const uint8_t *buf` to `const void *buf`. Update call site (~line 614).
- `iqrecord.c:185` — Add `n == 0` -> EIO check for robustness. Keep `g_stop` check (or remove per Change 9 if applied first).
- `rx888_dsp.c` — No change (inline write loops are out of scope).

### Verification

- Build clean
- `rx888_stream`: test broken-pipe (`| head -c 1`), normal streaming to `/dev/null`, SIGINT mid-stream
- `iqrecord`: test disk-full (write error) path

### Reference

`doc/style_harmonization.md`, Change 4
EOF
)"

# ── 6. Change 12: iqrecord usage string ──────────────────────────────────

create_issue \
  "iqrecord: usage string missing --fsync and says 'MVP defaults'" \
  "bug,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 12

**Priority:** Low — cosmetic/usability
**Scope:** `iqrecord.c` only
**Type:** Bug fix (missing help text)

### Problem

The `--help` / usage string (`iqrecord.c:411-413`) does not mention `--fsync`, which is a supported option parsed at line 432. Also says "MVP defaults" in user-facing text.

Current:
```
usage: ./iqrecord OUTDIR [--freq HZ] [--desc TEXT]
MVP defaults: datatype=cf32_le Fs=33750000 ...
```

### Target State

```
usage: ./iqrecord OUTDIR [--freq HZ] [--desc TEXT] [--fsync]
defaults: datatype=cf32_le Fs=33750000 samples_per_file=337500000 block_samples=4194304
```

### Changes Required

- `iqrecord.c:411-413` — Add `[--fsync]` to usage line. Change "MVP defaults" to "defaults".

### Verification

- `./iqrecord` with no args shows updated usage
- `./iqrecord --fsync` with no OUTDIR still shows usage (not a crash)

### Reference

`doc/style_harmonization.md`, Change 12
EOF
)"

# ── 7. Change 10: run.json structure cleanup ─────────────────────────────

create_issue \
  "iqrecord: remove zero-valued accounting block from run.json" \
  "style,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 10

**Priority:** Low
**Scope:** `iqrecord.c` only
**Type:** Cleanup

### Problem

`run.json` contains a top-level `"accounting"` object with zeroed counters written at file open time, followed by `"final_accounting"` with real totals at close. The zero block is never updated and serves no purpose.

### Target State

Remove the zero `"accounting"` block. Rename `"final_accounting"` to `"accounting"`:

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

### Changes Required

- `iqrecord.c:334-338` (`open_run_json`) — Remove the six lines that write the zero `"accounting"` block.
- `iqrecord.c:396` (`finalize_run_json`) — Change `"final_accounting"` to `"accounting"`.

### Verification

- Build clean
- Short capture, confirm `run.json` parses with `jq`, single `"accounting"` key with correct totals
- Run `tests/kill_iqrecord_test.sh` with `sigint`

### Reference

`doc/style_harmonization.md`, Change 10
EOF
)"

# ── 8. Change 11: Atomic metadata writes ─────────────────────────────────

create_issue \
  "iqrecord: use atomic write-then-rename for metadata files" \
  "style,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 11

**Priority:** Medium — data integrity improvement
**Scope:** `iqrecord.c` only
**Type:** Robustness improvement

### Problem

A crash (SIGKILL, power loss) during `write_sigmf_meta` or `finalize_run_json` can leave truncated `.sigmf-meta` or `run.json` files on disk, indistinguishable from valid files without parsing.

### Target State

Write metadata to a `.tmp` suffix, then `rename()` on completion:

```c
/* In write_sigmf_meta: */
snprintf(tmp_path, ..., "%s/cap_%06" PRIu64 ".sigmf-meta.tmp", ...);
FILE *f = fopen(tmp_path, "wb");
/* ... write contents ... */
fclose(f);
rename(tmp_path, final_path);
```

`rename()` is atomic on POSIX filesystems. After rename, the file is either complete or absent.

### Changes Required

- `iqrecord.c` `write_sigmf_meta` (line 219) — Write to `.sigmf-meta.tmp`, then `rename()`.
- `iqrecord.c` `open_run_json` (line 293) — Open `run.json.tmp` instead of `run.json`.
- `iqrecord.c` `finalize_run_json` (line 389) — After `fclose()`, `rename("run.json.tmp", "run.json")`.

### Verification

- Build clean
- Confirm final files have no `.tmp` suffix after normal capture
- `ls` output directory during capture to confirm `.tmp` files exist transiently
- SIGKILL test: confirm no truncated `.sigmf-meta` or `run.json` (only `.tmp` artifacts)

### Reference

`doc/style_harmonization.md`, Change 11
EOF
)"

# ── 9. Change 15: AVX2/FMA runtime check ─────────────────────────────────

create_issue \
  "rx888_dsp: add AVX2/FMA runtime check instead of SIGILL crash" \
  "bug,rx888_dsp" \
  "$(cat <<'EOF'
## Style Harmonization: Change 15

**Priority:** Medium — usability improvement
**Scope:** `rx888_dsp.c` only
**Type:** Robustness improvement

### Problem

The binary is compiled with `-mavx2 -mfma`. On a non-AVX2 CPU it crashes with `SIGILL` — an opaque illegal instruction fault, not a clear diagnostic. The README claims "fail-fast behavior for unsupported CPUs" but there is no actual check.

### Target State

Add a check early in `main()`, before any DSP code runs:

```c
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) {
        fprintf(stderr, "rx888_dsp: this CPU lacks AVX2+FMA support (required)\n");
        return EXIT_FAILURE;
    }
#endif
```

Place after argument parsing and TTY checks, before `init_hb2_coeffs()` (~line 1262).

### Verification

- Build clean on an AVX2 system
- Test by temporarily changing `"avx2"` to `"avx512f"` — should print error and exit
- Revert and confirm normal operation

### Reference

`doc/style_harmonization.md`, Change 15
EOF
)"

# ── 10. Change 16: FIFO disconnect docs ──────────────────────────────────

create_issue \
  "rx888_dsp: document FIFO disconnect and loss policy" \
  "documentation,rx888_dsp" \
  "$(cat <<'EOF'
## Style Harmonization: Change 16

**Priority:** Low
**Scope:** Documentation only (`doc/rx888_dsp.md`, `doc/rx888_dsp_arch.md`)
**Type:** Documentation

### Problem

The README and ARCHITECTURE docs don't explain the actual loss policy for FIFO output:

- No reader at startup → blocks dropped and counted
- Reader disconnects mid-stream → EPIPE closes fd, blocks dropped until reconnect
- Shutdown drain with no reader → blocks dropped
- `--block-on-full` does not affect FIFO writer behavior

The code handles all of these correctly. The documentation doesn't explain them.

### Target State

Add a "FIFO behavior" subsection to the README Notes section covering all four scenarios. Tighten the "no silent data loss" claim to scope it to "while output is connected."

### Changes Required

- `doc/rx888_dsp.md` Notes section — Add FIFO behavior notes
- `doc/rx888_dsp.md` Signals section — Change "no silent data loss" to "no silent data loss while output is connected"
- `doc/rx888_dsp_arch.md` — Update "gracefully" description in output thread section

### Verification

- Review only
- Confirm notes match observed behavior by running with `-v -o /tmp/test.fifo` with no reader, then starting a reader mid-stream, then killing the reader

### Reference

`doc/style_harmonization.md`, Change 16
EOF
)"

# ── 11. Change 13: Backpressure documentation ────────────────────────────

create_issue \
  "rx888_stream: document backpressure exit policy in README" \
  "documentation,rx888_stream" \
  "$(cat <<'EOF'
## Style Harmonization: Change 13

**Priority:** Low
**Scope:** Documentation only (`README.md`)
**Type:** Documentation

**Note:** Per Appendix B, this change may already be applied to `doc/rx888_stream.md`. Verify before implementing.

### Problem

When the internal transfer queue fills (downstream stalled), the callback sets `g_stop` and exits immediately. This is correct behavior for a real-time USB driver, but the policy is not documented in the README.

### Target State

Add to README Notes section:

> If downstream stalls long enough to fill the internal transfer queue (queue depth x transfer size), rx888_stream exits rather than blocking the USB event loop or silently dropping samples. Increase -q or fix the downstream bottleneck.

### Verification

- Review only

### Reference

`doc/style_harmonization.md`, Change 13
EOF
)"

# ── 12. Change 14: Verbose memory budget ─────────────────────────────────

create_issue \
  "rx888_stream: print memory budget summary in verbose mode" \
  "style,rx888_stream" \
  "$(cat <<'EOF'
## Style Harmonization: Change 14

**Priority:** Low
**Scope:** `rx888_stream.c` only
**Type:** Enhancement

### Problem

Debugging USB buffer issues requires knowing total in-flight memory and how it relates to `usbfs_memory_mb`. Currently the user must compute this manually from `-p` and `-q`.

### Target State

With `-v`, print a one-time summary after endpoint discovery:

```
rx888_stream: samplerate=135000000 Hz
rx888_stream: transfer_size=1048576 bytes (1024 packets x 1024 bytes)
rx888_stream: queue_depth=32, total_inflight=33554432 bytes (32.00 MiB)
rx888_stream: minimum usbfs_memory_mb=32
```

Use actual packet size from `pick_bulk_in_endpoint` (1024 for USB3, 512 for USB2).

### Changes Required

- `rx888_stream.c` — Add verbose startup printout after endpoint discovery.

### Verification

- `./rx888_stream -v` (will fail at device open, but verbose output should appear before that if placed after option parsing)
- With hardware: confirm values match expected sizing

### Reference

`doc/style_harmonization.md`, Change 14
EOF
)"

# ── 13. Change 5: Error output functions ──────────────────────────────────

create_issue \
  "Harmonize error output functions (die/warn_msg/info_msg) across all programs" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 5

**Priority:** Low — moderate effort, touches many lines
**Scope:** All three files
**Type:** Style harmonization

### Problem

Three completely different error output patterns:

| File | Fatal error | Non-fatal warning | Verbose info |
|------|------------|-------------------|--------------|
| `iqrecord.c` | `die(fmt, ...)` | `warn_msg(fmt, ...)` | none |
| `rx888_dsp.c` | `LOG_ERR()` macro | `LOG_ERR()` | `LOG_INFO()` macro |
| `rx888_stream.c` | raw `fprintf(stderr, ...)` | raw `fprintf(stderr, ...)` | `vlogf(level, fmt, ...)` |

Messages are inconsistently prefixed with the program name.

### Target State

Common helpers in all three files:

```c
#define PROG_NAME "iqrecord"  /* per file */

static void die(const char *fmt, ...) { /* prefix + print + exit(1) */ }
static void warn_msg(const char *fmt, ...) { /* prefix + print */ }
static void info_msg(const char *fmt, ...) { /* prefix + print, gated on g_verbose */ }
```

### Design Decision Needed

`rx888_stream` has multi-level verbosity (level 0/1/2/3). The simple `info_msg` is boolean. Options:
- Boolean verbose for all (simplest)
- `info_msg(int level, ...)` for `rx888_stream` only

### Changes Required

- `iqrecord.c` — Add `PROG_NAME`, add prefix to `die()`/`warn_msg()`, remove hardcoded `"iqrecord: "`
- `rx888_dsp.c` — Replace `LOG_ERR`/`LOG_INFO` macros with `die`/`warn_msg`/`info_msg` functions
- `rx888_stream.c` — Add helpers, replace raw `fprintf(stderr, ...)`, replace `vlogf()`

### Verification

- Build clean
- `--help` and invalid-arg paths on all three
- All stderr output prefixed with program name
- `rx888_stream -v` verbose output still works at all levels

### Reference

`doc/style_harmonization.md`, Change 5
EOF
)"

# ── 14. Change 7: File header comment ─────────────────────────────────────

create_issue \
  "Add file header comment to iqrecord.c" \
  "style,iqrecord" \
  "$(cat <<'EOF'
## Style Harmonization: Change 7

**Priority:** Low — cosmetic
**Scope:** `iqrecord.c` (primary), minor review of other two files
**Type:** Style harmonization

### Problem

`iqrecord.c` has no file header comment. The other two files have detailed block comments explaining purpose, I/O, and design.

### Target State

All three files have a header comment:

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

### Changes Required

- `iqrecord.c` — Add header comment before `#define _GNU_SOURCE`
- `rx888_dsp.c` — Already has a detailed header, no change
- `rx888_stream.c` — Already has a detailed header, review copyright line

### Verification

- Visual inspection only

### Reference

`doc/style_harmonization.md`, Change 7
EOF
)"

# ── 15. Change 6: Section header comments ─────────────────────────────────

create_issue \
  "Harmonize section header comment style across all programs" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 6

**Priority:** Low — cosmetic
**Scope:** `iqrecord.c`, `rx888_dsp.c`
**Type:** Style harmonization

### Problem

Inconsistent section divider style:

| File | Style |
|------|-------|
| `iqrecord.c` | `// ---- text ----` |
| `rx888_dsp.c` | None |
| `rx888_stream.c` | `/* --- text --- */` (already done) |

### Target State

C-style block comment dividers everywhere:

```c
/* ----------------------------- Section name ------------------------------ */
```

### Changes Required

- `iqrecord.c` — Replace `// ---- text ----` with `/* --- text --- */`. Add section headers for major groups.
- `rx888_dsp.c` — Add section headers at major boundaries: Coefficients, Queue, Buffer pool, DSP kernels, Processing thread, Output thread, CLI/main.
- `rx888_stream.c` — Already done, no change.

### Verification

- Visual inspection only

### Reference

`doc/style_harmonization.md`, Change 6
EOF
)"

# ── 16. Change 2: Indentation normalization ──────────────────────────────

create_issue \
  "Normalize indentation to 4-space, no tabs, across all programs" \
  "style,cross-project" \
  "$(cat <<'EOF'
## Style Harmonization: Change 2

**Priority:** Low — do last (largest diff, easiest to review after all semantic changes)
**Scope:** `iqrecord.c`, `rx888_stream.c`
**Type:** Style harmonization

### Problem

Three different indent styles:

| File | Current style |
|------|--------------|
| `iqrecord.c` | 2-space throughout |
| `rx888_dsp.c` | 4-space throughout (already correct) |
| `rx888_stream.c` | Mixed tabs and 4-space |

### Target State

4-space indent in all three files. No tabs.

### Changes Required

- `iqrecord.c` — Convert all 2-space indent to 4-space (~162 indented lines). Must verify multi-line strings and alignment manually.
- `rx888_dsp.c` — Already 4-space, no change.
- `rx888_stream.c` — Convert tabs to 4-space (~151 tab-indented lines, concentrated in `main()` option parsing).

### Important

Do this change **last** — it touches every line and creates the largest diff. Much easier to review if all semantic changes are already committed.

### Verification

- `grep -P '\t' src/*.c` should return zero matches
- Visual inspection of multi-line string literals and alignment columns

### Reference

`doc/style_harmonization.md`, Change 2
EOF
)"

echo ""
echo "=== Done ==="
echo "Created: $created issues"
echo "Failed:  $failed issues"
