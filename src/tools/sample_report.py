# sample_report.py SAMPLES [--map dt_port.map] [--csv FRAMES.csv] [--slow MS] [--first N] [--symbols symbols.csv]
#                  [--under NAME] [--top N]
#
# Where the sampled threads spend their time, from `dt_port --sample` (native stacks ~1000 times a second, Windows).
# Executable addresses are named through the linker map (build/game/dt_port.map); recompiled functions (sub_XXXXXXXX)
# also get the game's own symbol name from src/notes/symbols.csv (made by dump_symbols.py) when it exists.
# With --csv (the same run's --frame-csv), samples in slow frames (over --slow ms) are compared with the rest; Bink
# movie spans (swap interval 2) are left out. --under NAME keeps only what runs inside functions matching NAME.
import bisect, csv, re, sys
from collections import Counter, defaultdict

args = sys.argv[1:]
opts = {'--map': 'build/game/dt_port.map', '--csv': None, '--slow': '25', '--first': '0',
        '--symbols': 'src/notes/symbols.csv', '--top': '25', '--under': None}
paths = []
while args:
    a = args.pop(0)
    if a in opts:
        opts[a] = args.pop(0)
    else:
        paths.append(a)
if len(paths) != 1:
    sys.exit(__doc__ if __doc__ else "usage: sample_report.py SAMPLES [--map MAP] [--csv CSV] [--slow MS]")
TOP = int(opts['--top'])

# ---- names ----
PREFERRED_BASE = 0x140000000
starts, names = [], []
for line in open(opts['--map'], errors='replace'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
    if m:
        starts.append(int(m.group(2), 16))
        names.append(m.group(1))
order = sorted(range(len(starts)), key=lambda i: starts[i])
starts = [starts[i] for i in order]
names = [names[i] for i in order]

guest = {}
try:
    for r in csv.DictReader(open(opts['--symbols'], errors='replace')):
        guest[int(r['address'], 16)] = r['demangled']
except OSError:
    pass

def undecorate(n):
    if not n.startswith('?'):
        return n
    body = n[1:].split('@@')[0]
    parts = body.split('@')
    return '::'.join(reversed(parts))

cache = {}
def name_of(tok):
    if tok in cache:
        return cache[tok]
    if tok.startswith('e+'):
        va = PREFERRED_BASE + int(tok[2:], 16)
        i = bisect.bisect_right(starts, va) - 1
        n = undecorate(names[i]) if i >= 0 else tok
        m = re.match(r'sub_([0-9A-F]{8})$', n)
        if m and int(m.group(1), 16) in guest:
            g = guest[int(m.group(1), 16)]
            n = f"{n} {g[:90]}"
    elif tok.startswith('m:'):
        n = '[' + tok[2:].split('+')[0] + ']'
    else:
        n = '[unknown]'
    cache[tok] = n
    return n

def kind(n):
    if n.startswith('sub_'):
        return 'game code (recompiled)'
    if n.startswith('imp_') or n.startswith('cafe::'):
        return 'imports (our Wii U libraries)'
    if n.startswith('['):
        return 'system ' + n
    return 'runtime / renderer'

# ---- samples ----
threads = {}
samples = []  # (frame, thread, [names innermost first])
for line in open(paths[0], errors='replace'):
    if line.startswith('#'):
        continue
    if line.startswith('thread '):
        _, idx, tname = line.rstrip('\n').split(' ', 2)
        threads[int(idx)] = tname
        continue
    parts = line.split()
    if len(parts) < 3:
        continue
    frame, t = int(parts[0]), int(parts[1])
    if frame < int(opts['--first']):
        continue
    samples.append((frame, t, [name_of(tok) for tok in parts[2:]]))

slow = set()
frame_ms = {}
if opts['--csv']:
    for r in csv.DictReader(open(opts['--csv'])):
        frame_ms[int(r['frame'])] = float(r['ms'])
    # Bink movies run at swap interval 2 (runs of 33.37 ms frames): neither slow nor normal frames.
    movie, fs = set(), sorted(frame_ms)
    i = 0
    while i < len(fs):
        j = i
        while j < len(fs) and 33.2 < frame_ms[fs[j]] < 33.6:
            j += 1
        if j - i >= 10:
            movie.update(range(fs[i] - 2, fs[j - 1] + 3))
        i = max(j, i + 1)
    samples = [s for s in samples if s[0] not in movie]
    slow = {f for f, ms in frame_ms.items() if ms > float(opts['--slow']) and f not in movie}
    if movie:
        print(f"left out {len(movie)} movie frames")

def report(title, subset, frames):
    print(f"\n=== {title}: {len(subset)} samples over {frames} frames ({len(subset) / max(frames, 1):.1f} per frame) ===")
    if not subset:
        return
    by_kind = Counter(kind(s[2][0]) for s in subset)
    print("leaf by kind: " + ", ".join(f"{k} {100 * n / len(subset):.1f}%" for k, n in by_kind.most_common()))
    self_ = Counter(s[2][0] for s in subset)
    incl = Counter()
    for s in subset:
        incl.update(set(s[2]))
    print(f"self (innermost function), top {TOP}:")
    for n, c in self_.most_common(TOP):
        print(f"  {100 * c / len(subset):5.1f}%  {c / max(frames, 1):6.2f}/frame  {n}")
    print(f"inclusive (on the stack), top {TOP}:")
    for n, c in incl.most_common(TOP):
        print(f"  {100 * c / len(subset):5.1f}%  {c / max(frames, 1):6.2f}/frame  {n}")

all_frames = {s[0] for s in samples}
if opts['--under']:
    # Only the part of each stack below (inside) the first function whose name contains --under.
    cut = []
    for fr, t, st in samples:
        k = next((i for i, n in enumerate(st) if opts['--under'] in n), None)
        if k is not None:
            cut.append((fr, t, st[:k + 1]))
    print(f"--under {opts['--under']}: {len(cut)} of {len(samples)} samples")
    samples = cut
for t, tname in sorted(threads.items()):
    mine = [s for s in samples if s[1] == t]
    if not slow:
        report(tname, mine, len(all_frames))
        continue
    s_slow = [s for s in mine if s[0] in slow]
    s_norm = [s for s in mine if s[0] not in slow]
    n_slow = len(slow & all_frames)
    n_norm = len(all_frames - slow)
    report(f"{tname}, normal frames", s_norm, n_norm)
    report(f"{tname}, slow frames (> {opts['--slow']} ms)", s_slow, n_slow)
    # Where slow frames spend their extra time: samples per frame, slow minus normal, by inclusive function.
    inc_s, inc_n = Counter(), Counter()
    for s in s_slow:
        inc_s.update(set(s[2]))
    for s in s_norm:
        inc_n.update(set(s[2]))
    extra = sorted(((inc_s[n] / max(n_slow, 1) - inc_n[n] / max(n_norm, 1), n) for n in inc_s), reverse=True)
    print(f"--- {tname}: extra samples per slow frame (~ms), by inclusive function ---")
    for d, n in extra[:TOP]:
        print(f"  {d:+7.2f}  {n}")
