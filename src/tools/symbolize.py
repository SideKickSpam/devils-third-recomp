# symbolize.py LOG [--map build/game/dt_port.map] [--symbols src/notes/symbols.csv]
#
# Names the native stacks in a dt_port log ("native stack, innermost first: e+..." lines in hang and crash reports):
# executable offsets through the linker map, recompiled functions (sub_XXXXXXXX) also by the game's symbol.
import bisect, csv, re, sys

args = sys.argv[1:]
opts = {'--map': 'build/game/dt_port.map', '--symbols': 'src/notes/symbols.csv'}
paths = []
while args:
    a = args.pop(0)
    if a in opts:
        opts[a] = args.pop(0)
    else:
        paths.append(a)
if len(paths) != 1:
    sys.exit("usage: symbolize.py LOG [--map MAP] [--symbols CSV]")

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

def name_of(offset):
    va = 0x140000000 + offset
    i = bisect.bisect_right(starts, va) - 1
    n = names[i] if i >= 0 else hex(offset)
    if n.startswith('?'):
        n = '::'.join(reversed(n[1:].split('@@')[0].split('@')))
    m = re.match(r'sub_([0-9A-F]{8})$', n)
    if m and int(m.group(1), 16) in guest:
        n += '  ' + guest[int(m.group(1), 16)][:110]
    return n

thread = None
for line in open(paths[0], errors='replace'):
    if line.startswith('--- thread'):
        thread = line.strip()
    m = re.match(r'\s*native stack, innermost first:(.*)', line)
    if m:
        print(thread)
        for tok in m.group(1).split():
            print('    ' + (name_of(int(tok[2:], 16)) if tok.startswith('e+') else tok))
