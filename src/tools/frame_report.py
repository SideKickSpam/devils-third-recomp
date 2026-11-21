# frame_report.py CSV...: frame-time distribution and slow-frame causes from --frame-csv files, over gameplay
# (frames >= --first, default 2400). Leaves out Bink movie spans (runs of 33.37 ms frames: swap interval 2).
import csv, sys, statistics as st
from collections import Counter, defaultdict
FIRST = 2400
args = sys.argv[1:]
if args[:1] == ['--first']:
    FIRST, args = int(args[1]), args[2:]
def pct(v, p): v = sorted(v); return v[min(len(v) - 1, int(p / 100 * len(v)))]
allrows = []
for path in args:
    rows = [r for r in csv.DictReader(open(path))]
    by = {int(r['frame']): r for r in rows}
    gpu = {}
    for r in rows:
        g = float(r.get('gpu_ms_2_frames_back') or 0)
        if g: gpu[int(r['frame']) - 2] = g
    movie = set(); i = 0; fs = sorted(by)
    while i < len(fs):
        j = i
        while j < len(fs) and 33.2 < float(by[fs[j]]['ms']) < 33.6: j += 1
        if j - i >= 10: movie.update(range(fs[i] - 2, fs[j - 1] + 3))
        i = max(j, i + 1)
    for f in fs:
        if f >= FIRST and f not in movie:
            r = dict(by[f]); r['gpu'] = gpu.get(f); r['run'] = path; allrows.append(r)
def f(r, k): return float(r[k])
def cause(r):
    if f(r, 'compile_ms') >= 5: return 'compile'
    if f(r, 'game_fs') + f(r, 'render_fs') >= 5: return 'file system'
    if f(r, 'create_ms') + f(r, 'detile_ms') >= 5: return 'surfaces/uploads'
    if f(r, 'fence_ms') >= 5: return 'GPU (CPU waits for an earlier frame)'
    if f(r, 'game_code') >= 25: return 'game thread code'
    if f(r, 'render_code') + f(r, 'draw_ms') >= 20: return 'render thread'
    return 'mixed / small'
ms = [f(r, 'ms') for r in allrows]
print(f"{len(args)} runs, {len(ms)} gameplay frames (movies excluded): mean {st.mean(ms):.2f} ms, stdev {st.pstdev(ms):.2f}, "
      f"p1 {pct(ms,1):.2f}, p50 {pct(ms,50):.2f}, p99 {pct(ms,99):.2f}, p99.9 {pct(ms,99.9):.1f}, max {max(ms):.1f}")
print(f"mean |frame-to-frame change| {st.mean(abs(a-b) for a,b in zip(ms, ms[1:])):.2f} ms; exactly 16.68 ms (early, held by the 59.94 Hz pacing): {sum(16.6 < m < 16.75 for m in ms)}")
for lim in (22, 25, 33, 50): print(f"  frames over {lim} ms: {sum(m > lim for m in ms)} ({100*sum(m > lim for m in ms)/len(ms):.2f}%)")
hist = Counter(min(int(m), 40) for m in ms)
print("histogram:", ", ".join(f"{k}{'+' if k == 40 else ''}:{hist[k]}" for k in sorted(hist)))
slow = [r for r in allrows if f(r, 'ms') > 25]
c = Counter(cause(r) for r in slow); ex = defaultdict(float)
for r in slow: ex[cause(r)] += f(r, 'ms') - 20
print(f"\nframes over 25 ms: {len(slow)}, by cause (frames, ms over a 20 ms frame):")
for k, n in c.most_common(): print(f"  {k}: {n}, {ex[k]:.0f} ms")
g = [r['gpu'] for r in allrows if r['gpu']]
if g: print(f"\nGPU ms per frame (runs with it): mean {st.mean(g):.2f}, p99 {pct(g,99):.1f}, max {max(g):.1f}; frames over 20 ms: {sum(x > 20 for x in g)}")
for k in ('GPU (CPU waits for an earlier frame)', 'game thread code', 'compile', 'render thread', 'mixed / small'):
    rs = sorted([r for r in slow if cause(r) == k], key=lambda r: -f(r, 'ms'))[:6]
    if rs: print(f"\n{k}:")
    for r in rs:
        print(f"  {f(r,'ms'):6.1f} ms {r['run']} frame {r['frame']}: gpu {r['gpu']}, fence {r['fence_ms']}, compile {r['compile_ms']} ({r['pipelines']}p/{r['shaders']}s), draws {r['draws']}, "
              f"game code {r['game_code']} wait {r['game_wait']} other {r['game_other']}, render code {r['render_code']} draw {r['draw_ms']} gx2 {r['render_gx2']} swap {r['render_swap']} wait {r['render_wait']}")
