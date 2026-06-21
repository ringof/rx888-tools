#!/usr/bin/env python3
# pps_log_stats.py — correlate dip / MISS / spurious events in pps_integrity logs.
#
# pps_integrity (run with -v) prints a per-second line:
#   time  stat  edges  marks  spur  miss  minxfer  [note]
# where note may be "dip ~N (continuity)" (a continuity deficit) and stat is
# ok / MISS / BLIND. This tool parses that stream and reports the correlations
# that distinguish the failure modes — most importantly the buffer-BOUNDARY
# enrichment of dip (loss) seconds, which is the signature of a commit-vs-
# buffer-completion race. Run it on each experiment log (_baseline, _lowR,
# _sync, ...) and compare: a working fix should drop the dip count AND collapse
# the boundary-enrichment toward 1x.
#
# Usage:  tests/pps_log_stats.py <log> [<log2> ...]
#   one log  -> full per-run report
#   many     -> full report each, then a comparison table
#
# Stdlib only; needs a -v log (the minxfer column). Not a hardware test.

import re, sys, math, statistics as st, collections

TIME_RE = re.compile(r'^\d\d:\d\d:\d\d')
DIP_RE  = re.compile(r'dip ~(\d+)')

def parse(path):
    rows, summary = [], {}
    full = 524288
    for ln in open(path, errors='replace'):
        m = re.search(r'Transfer size:\s+(\d+) samples', ln)
        if m: full = int(m.group(1))
        m = re.search(r'=\s*([\d.]+)\s*ppm\b.*never delivered', ln) \
            or re.search(r'never delivered\s*=\s*([\d.]+)\s*ppm', ln)
        if m: summary['loss_ppm'] = float(m.group(1))
        m = re.search(r'undelivered\s*([+-][\d.]+)\s*MB', ln)
        if m: summary['undeliv_mb'] = float(m.group(1))
        m = re.search(r'Result:\s*(\w+)', ln)
        if m: summary['result'] = m.group(1)
        f = ln.split()
        if len(f) < 7 or not TIME_RE.match(f[0]):
            continue
        try:
            r = dict(stat=f[1], edges=int(f[2]), marks=int(f[3]),
                     spur=int(f[4]), miss=int(f[5]), mx=int(f[6]))
        except ValueError:
            continue                       # not a -v data line
        d = DIP_RE.search(ln)
        r['dip'] = int(d.group(1)) if d else None
        rows.append(r)
    return rows, summary, full

def frac_bucket(x, full):
    f = x / full
    if f < 0.05:  return 0
    if f < 0.25:  return 1
    if f < 0.75:  return 2
    if f < 0.95:  return 3
    return 4
BUCKETS = ['<0.05 (near-empty)', '0.05-0.25', '0.25-0.75',
           '0.75-0.95', '>=0.95 (near-full)']

def describe(v, label):
    v = [x for x in v if x is not None]
    if not v:
        print(f"  {label}: (none)"); return
    s = sorted(v)
    print(f"  {label}: n={len(s)} min={s[0]} med={s[len(s)//2]} "
          f"mean={int(st.mean(s))} max={s[-1]}")

def analyze(path):
    rows, summary, full = parse(path)
    n = len(rows)
    print(f"\n===== {path} =====")
    if n == 0:
        print("  no per-second -v data lines found "
              "(was the run done with -v?)"); return None

    miss = [i for i, r in enumerate(rows) if r['stat'] == 'MISS']
    blind = [i for i, r in enumerate(rows) if r['stat'] == 'BLIND']
    dip = [i for i, r in enumerate(rows) if r['dip'] is not None]
    spur_inc = [rows[i]['spur'] - (rows[i-1]['spur'] if i else 0) for i in range(n)]
    spur_sec = [i for i in range(n) if spur_inc[i] > 0]
    hrs = n / 3600.0

    print(f"  seconds={n} ({hrs:.2f} h)  edges={rows[-1]['edges']}  "
          f"marks={rows[-1]['marks']}")
    print(f"  MISS(displaced)={len(miss)}  dip(loss)={len(dip)}  "
          f"BLIND={len(blind)}  spur++ seconds={len(spur_sec)} "
          f"(total spur={rows[-1]['spur']})")
    if summary:
        print(f"  summary: result={summary.get('result','?')} "
              f"loss={summary.get('loss_ppm','?')} ppm "
              f"undelivered={summary.get('undeliv_mb','?')} MB")

    # --- boundary enrichment: the key discriminator ---
    okmx = [r['mx'] for r in rows if r['stat'] == 'ok']
    def buck(idxs_or_vals, vals=False):
        v = idxs_or_vals if vals else [rows[i]['mx'] for i in idxs_or_vals]
        c = [0]*5
        for x in v: c[frac_bucket(x, full)] += 1
        return c, len(v) or 1
    ca, ta = buck(okmx, vals=True)
    cd, td = buck(dip)
    print("\n  marker position (minxfer / full buffer):")
    print(f"    {'bucket':22s} {'all ok %':>9s} {'dip %':>8s}")
    for k in range(5):
        print(f"    {BUCKETS[k]:22s} {100*ca[k]/ta:8.1f}% {100*cd[k]/td:7.1f}%")
    base_bnd = (ca[0]+ca[4]) / ta            # near-empty + near-full, baseline
    dip_bnd  = (cd[0]+cd[4]) / td
    enr = dip_bnd / base_bnd if base_bnd else float('inf')
    print(f"  >>> boundary-adjacent: {100*dip_bnd:.1f}% of dips vs "
          f"{100*base_bnd:.1f}% baseline  ->  ENRICHMENT {enr:.1f}x")
    print("      (high enrichment = loss is a commit-vs-buffer-boundary race)")
    describe([rows[i]['dip'] for i in dip], "dip magnitude (samples)")
    describe(okmx, "all ok-second marker size")

    # --- MISS = clean merge? ---
    if miss:
        after = [rows[i+1]['mx'] for i in miss if i+1 < n]
        typ = st.median(okmx) if okmx else 0
        describe(after, "marker size on second AFTER a MISS")
        if after and typ:
            print(f"      median post-MISS / typical marker = "
                  f"{st.median(after)/typ:.2f}x  (~2x => clean displaced merge)")

    # --- co-occurrence ---
    ds, ms, ss = set(dip), set(miss), set(spur_sec)
    print(f"  co-occur: MISS&dip={len(ms&ds)}/{len(ms) or 0}  "
          f"spur&dip={len(ss&ds)}/{len(ss) or 0}  "
          f"spur&MISS={len(ss&ms)}/{len(ss) or 0}")

    # --- temporal ---
    if len(dip) > 1:
        gaps = [dip[i]-dip[i-1] for i in range(1, len(dip))]
        describe(gaps, "inter-dip gap (s)")
    return dict(path=path, secs=n, hrs=hrs, dips=len(dip), miss=len(miss),
                spur=rows[-1]['spur'], enr=enr,
                loss_ppm=summary.get('loss_ppm'),
                undeliv_mb=summary.get('undeliv_mb'),
                result=summary.get('result'))

def compare(ms):
    ms = [m for m in ms if m]
    if len(ms) < 2: return
    print("\n===== COMPARISON =====")
    h = f"{'run':40s} {'dips/hr':>8s} {'bnd-enr':>8s} {'spur':>6s} " \
        f"{'MISS':>5s} {'loss_ppm':>9s} {'undeliv_MB':>11s} {'result':>7s}"
    print(h); print('-'*len(h))
    for m in ms:
        name = m['path'].split('/')[-1]
        print(f"{name[:40]:40s} {m['dips']/m['hrs']:8.1f} {m['enr']:7.1f}x "
              f"{m['spur']:6d} {m['miss']:5d} "
              f"{('%.1f'%m['loss_ppm']) if m['loss_ppm'] is not None else '?':>9s} "
              f"{('%+.1f'%m['undeliv_mb']) if m['undeliv_mb'] is not None else '?':>11s} "
              f"{(m['result'] or '?'):>7s}")
    print("\nA working edge/synchronizer fix should drop dips/hr AND collapse")
    print("bnd-enr toward ~1.0 (loss no longer concentrated at buffer boundaries).")

def main(argv):
    if len(argv) < 2:
        print(__doc__ or "usage: pps_log_stats.py <log> [<log2> ...]")
        return 2
    metrics = [analyze(p) for p in argv[1:]]
    compare(metrics)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
