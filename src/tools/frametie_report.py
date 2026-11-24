#!/usr/bin/env python3
"""Compares two frametie profiles (DT_FRAMETIE_PROFILE, one per fps cap) and lists the script sites that look frame-tied:
they run about once per engine tick at both rates, and add the same amount per call at both (correct code adds an
amount scaled by the frame time, so less per call at the higher rate).

    python3 -I src/tools/frametie_report.py prof.50.csv prof.0.csv
"""
import csv
import math
import sys


def load(path):
    with open(path, newline="") as f:
        head = f.readline().split()
        ticks, seconds = int(head[2]), float(head[4])
        rows = {}
        for r in csv.DictReader(f):
            rows[(r["function"], r["offset"], r["op"])] = (int(r["calls"]), float(r["sum"]), float(r["sum_abs"]), int(r.get("tick_checks") or 0), int(r.get("continuous") or 0))
    return ticks, seconds, rows


def main():
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    lo_t, lo_s, lo = load(files[0])
    hi_t, hi_s, hi = load(files[1])
    print(f"low: {lo_t} ticks {lo_s:.0f} s ({lo_t / lo_s:.1f} fps); high: {hi_t} ticks {hi_s:.0f} s ({hi_t / hi_s:.1f} fps)")
    out = []
    for key in lo.keys() & hi.keys():
        lc, lsum, labs, lchk, lcont = lo[key]
        hc, hsum, habs, hchk, hcont = hi[key]
        if lc < 200 or hc < 200 or labs == 0 or habs == 0:
            continue
        lpt, hpt = lc / lo_t, hc / hi_t  # calls per tick
        lmean, hmean = labs / lc, habs / hc
        ratio = lmean / hmean  # ~1: frame-tied; ~ the fps ratio: scaled by the frame time
        # per second of wall time: what the player sees
        lps, hps = labs / lo_s, habs / hi_s
        # the variable carries its value from one tick to the next: an accumulator, not a loop counter or a value
        # rebuilt every tick
        cont = min(lcont / lchk if lchk else 0, hcont / hchk if hchk else 0)
        out.append((hps / lps if lps else 0, key, lpt, hpt, lmean, hmean, ratio, cont))
    out.sort(key=lambda x: -x[0])
    tied_only = "--tied" in sys.argv
    print(f"{'per-sec hi/lo':>13} {'calls/tick lo':>13} {'hi':>6} {'mean/call lo':>13} {'hi':>11} {'lo/hi':>6} {'cont':>5}  site")
    for growth, (fn, off, op), lpt, hpt, lmean, hmean, ratio, cont in out:
        flag = "TIED" if 0.8 < ratio < 1.25 and min(lpt, hpt) > 0.3 and cont > 0.5 else ""
        if tied_only and not flag:
            continue
        print(f"{growth:13.2f} {lpt:13.2f} {hpt:6.2f} {lmean:13.5g} {hmean:11.5g} {ratio:6.2f} {cont:5.2f}  {fn} {off} {op} {flag}")


if __name__ == "__main__":
    main()
