# Hardware Integration Tests

Quick tests to run on a machine with an RX888 connected. These
exercise the code paths that cannot be validated without hardware:
USB streaming, DSP under real data rates, full pipeline integration,
and signal handling under load.

**Prerequisites:**
- RX888 / RX888mk2 connected via USB3
- `usbfs_memory_mb` set to at least 256 (see README)
- All three binaries built (`make`)
- `mbuffer` installed
- Optional: 50-ohm terminator on RF input (for consistent noise floor)

**Note:** The FX3 boots in DFU mode (PID `0x00f3`) after every
power-cycle. All commands below include `-f firmware/SDDC_FX3.img` to
upload firmware on first use. After a successful upload the device
re-enumerates to app mode (PID `0x00f1`) and subsequent runs in the
same power cycle do not strictly require `-f`, but including it is
harmless and keeps the commands copy-paste safe.

---

## 1. rx888_stream: device discovery and streaming

```sh
./rx888_stream -f firmware/SDDC_FX3.img -v -s 135000000 -q 32 -p 1024 \
  | head -c $((64*1024*1024)) > /tmp/rx888_smoke.raw
echo "exit=$?"
ls -la /tmp/rx888_smoke.raw
```

**Expected:**
- 64 MiB file
- Exit 0
- Verbose output shows config block (Ref. Clock, Sample Rate, Gain, etc.)
  and memory budget (transfer_size, queue_depth, total_inflight,
  minimum usbfs_memory_mb)
- All lines prefixed with `rx888_stream:`

**Validates:** Device open, USB streaming, verbose memory budget
(Change 14), error message prefixes (Change 5).

---

## 2. rx888_stream: broken pipe handling

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 | head -c 1 > /dev/null
echo "broken pipe exit=$?"
```

**Expected:**
- Clean exit, no crash, no segfault
- Exit 0

**Validates:** write_all EPIPE detection (Change 4), SIGPIPE handling
(Change 3).

---

## 3. rx888_stream: sample sanity

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 \
  | head -c $((64*1024*1024)) \
  | python3 -c "
import sys, numpy as np
x = np.frombuffer(sys.stdin.buffer.read(), dtype='<i2')
print(f'samples: {len(x)}, min: {x.min()}, max: {x.max()}, std: {x.std():.1f}')
"
```

**Expected:** With a 50-ohm terminator or no antenna, values should
cluster near zero with std in the low hundreds. Non-zero std confirms
real ADC data (not stuck at zero or railed).

**Validates:** Sample framing, byte order, data integrity through USB
and write_all.

---

## 4. rx888_dsp: DSP chain under real data

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 \
  | ./rx888_dsp -v \
  | head -c $((270*1024*1024)) > /tmp/dsp_test.cf32
echo "exit=$?"
```

**Expected:**
- ~270 MiB file (approximately 1 second of cf32 at 33.75 MS/s)
- Exit 0
- Verbose output on stderr prefixed with `rx888_dsp:`

**Validates:** Full DSP pipeline at real-time rates, AVX2/FMA runtime
check (Change 15) passes on a real AVX2 system, error prefixes
(Change 5).

---

## 5. Full pipeline: 30-second capture

```sh
rm -rf /tmp/pipeline_test
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 \
  | ./rx888_dsp --block-on-full -v \
  | mbuffer -m 2G -q \
  | timeout --signal=INT 30 ./iqrecord /tmp/pipeline_test \
      --freq 7100000 --desc "pipeline smoke test"

# Verify
python3 -m json.tool /tmp/pipeline_test/run.json
ls -lh /tmp/pipeline_test/

# Check accounting
python3 -c "
import json
d = json.load(open('/tmp/pipeline_test/run.json'))
a = d['accounting']
print(f\"files: {a['files_written']}, samples: {a['samples_written']}, bytes: {a['bytes_written']}\")
assert 'final_accounting' not in d, 'ERROR: final_accounting still present'
print('accounting key: OK')
"

# Check no .tmp files left behind
ls /tmp/pipeline_test/*.tmp 2>/dev/null && echo "FAIL: .tmp files remain" || echo "No .tmp files: OK"

# Check all .sigmf-data have matching .sigmf-meta
missing=0
for d in /tmp/pipeline_test/cap_*.sigmf-data; do
  m="${d%.sigmf-data}.sigmf-meta"
  [ -f "$m" ] || { echo "MISSING meta: $(basename $d)"; missing=$((missing+1)); }
done
echo "Missing meta count: $missing"
```

**Expected:**
- 3 files (two full 10-second + one partial) at ~2.7 GB each
- Valid `run.json` with single `"accounting"` key (not `"final_accounting"`)
- No `.tmp` files remaining
- All `.sigmf-data` files have matching `.sigmf-meta`
- `rx888_dsp` verbose stats on stderr

**Validates:** End-to-end pipeline, atomic metadata writes (Change 11),
run.json structure (Change 10), file rotation, SIGINT finalization.

---

## 6. SIGINT mid-capture

```sh
rm -rf /tmp/sigint_hw_test
bash tests/kill_iqrecord_test.sh /tmp/sigint_hw_test sigint 5
```

**Expected:**
- `PASS: graceful finalization under sigint`
- `run.json` present and valid JSON
- No missing `.sigmf-meta` files
- No `.tmp` files

**Validates:** Signal handling (Changes 1, 3), atomic metadata writes
(Change 11), g_stop propagation.

---

## 7. SIGUSR1 stats (rx888_dsp)

```sh
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 \
  | ./rx888_dsp --block-on-full -v > /dev/null &
PIPELINE_PID=$!
sleep 3
kill -USR1 $(pgrep -x rx888_dsp)
sleep 1
kill -INT $(pgrep -x rx888_dsp)
wait $PIPELINE_PID 2>/dev/null
```

**Expected:** Stats block on stderr. Without `-v`, a compact one-liner:

```
rx888_dsp: SIGUSR1: blocks=1547 dropped=0(0.00%) avg=0.82ms headroom=57.7% out=...
```

With `-v` (already set above), a detailed multi-line block:

```
rx888_dsp: === Statistics (SIGUSR1) ===
rx888_dsp: Blocks processed: 1547
rx888_dsp: Blocks dropped:   0 (0.00%)
rx888_dsp: Time/block: avg 0.82 ms (min 0.61, max 1.23)
rx888_dsp: Samples output: ...
```

**Validates:** SIGUSR1 handler, stats formatting, `rx888_dsp:` prefix
(Change 5), warn_msg output path.

---

## 8. Sustained streaming (soak test)

```sh
rm -rf /tmp/soak_test
./rx888_stream -f firmware/SDDC_FX3.img -s 135000000 -q 32 -p 1024 \
  | ./rx888_dsp --block-on-full -v \
  | mbuffer -m 4G -q \
  | ./iqrecord /tmp/soak_test --freq 7100000 --desc "30-minute soak test" &
PIPELINE_PID=$!

# Let it run, periodically check stats
for i in 1 2 3; do
  sleep 600
  kill -USR1 $(pgrep -x rx888_dsp) 2>/dev/null
done

# Stop after ~30 minutes
kill -INT $(pgrep -x iqrecord) 2>/dev/null
wait $PIPELINE_PID 2>/dev/null

# Verify
python3 -m json.tool /tmp/soak_test/run.json | tail -10
echo "Files:"
ls -lh /tmp/soak_test/cap_*.sigmf-data | wc -l
```

**Expected:**
- ~180 files (30 minutes / 10 seconds per file)
- Zero dropped blocks in SIGUSR1 stats
- Valid `run.json` with correct accounting
- No USB errors in `dmesg`

**Validates:** Long-duration stability across all changes. Any
regression in signal handling, memory management, or I/O paths will
surface here.

---

## What to watch for

| Symptom | Likely cause |
|---------|-------------|
| Missing `rx888_stream:` / `rx888_dsp:` / `iqrecord:` prefix on any stderr message | Change 5 regression |
| `"final_accounting"` key in run.json | Change 10 regression |
| `.sigmf-meta.tmp` or `run.json.tmp` left after clean shutdown | Change 11 regression |
| SIGILL crash in rx888_dsp | Change 15 (AVX2 check) not triggering |
| No memory budget in `rx888_stream -v` output | Change 14 regression |
| `write_full` in any error message | Change 4 (rename to write_all) missed a site |
| `signal_handler` or `stop_flag` in rx888_dsp output | Change 1/3 naming regression |
