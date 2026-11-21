"""Scaleform GFx movies (the game's menus and HUD) inside UE3 packages.

    python3 -I src/tools/gfx_movies.py list [PACKAGE...]          SwfMovie objects in packages (default: every package)
    python3 -I src/tools/gfx_movies.py extract PACKAGE NAME... [--out DIR]
                                                                   save movies as .gfx files (default DIR: build/gfx)
    python3 -I src/tools/gfx_movies.py info MOVIE.gfx [--strings]  tags, exported symbols, named placements per sprite,
                                                                   fonts, and (--strings) the ActionScript 2 constants

A SwfMovie's serialized data holds the movie (a SWF with a "GFX" signature, uncompressed here). Extracted movies are game
content: they go to build/ (untracked) and *.gfx / *.swf are gitignored.
"""
import argparse
import collections
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # also works under python -I
import ue3_package

REPO_ROOT = Path(__file__).resolve().parents[2]
COOKED = REPO_ROOT / 'game/0005000010177600_v16/content/DevilsThirdGame/CookedWiiU'

TAG_NAMES = {0: 'End', 1: 'ShowFrame', 12: 'DoAction', 26: 'PlaceObject2', 37: 'DefineEditText', 39: 'DefineSprite',
             56: 'ExportAssets', 59: 'DoInitAction', 70: 'PlaceObject3', 71: 'ImportAssets2', 75: 'DefineFont3',
             82: 'DoABC', 1000: 'GFx:ExporterInfo', 1005: 'GFx:DefineCompactedFont'}


def movie_bytes(serialized):
    """The movie inside a SwfMovie's serialized data."""
    for sig in (b'GFX', b'FWS'):
        k = serialized.find(sig)
        if k >= 0:
            return serialized[k:k + struct.unpack_from('<I', serialized, k + 4)[0]]
    raise ValueError('no uncompressed movie found')


def tags(data, nested=False):
    """(code, body, offset) for each tag: a movie's top level, or a sprite's tags when nested."""
    if nested:
        o = 0
    else:
        nbits = data[8] >> 3
        o = 8 + (5 + 4 * nbits + 7) // 8 + 4  # header, stage RECT, frame rate and count
    while o + 2 <= len(data):
        code_len = struct.unpack_from('<H', data, o)[0]
        o += 2
        code, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            length = struct.unpack_from('<I', data, o)[0]
            o += 4
        yield code, data[o:o + length], o
        o += length
        if code == 0:
            break


def as2_strings(actions):
    """ActionScript 2 strings: constant pools and pushed strings."""
    out, o = [], 0
    while o < len(actions):
        a = actions[o]
        o += 1
        if a < 0x80:
            continue
        length = struct.unpack_from('<H', actions, o)[0]
        payload = actions[o + 2:o + 2 + length]
        o += 2 + length
        if a == 0x88:  # ActionConstantPool
            p = 2
            for _ in range(struct.unpack_from('<H', payload, 0)[0]):
                e = payload.index(b'\0', p)
                out.append(payload[p:e].decode('utf-8', 'replace'))
                p = e + 1
        elif a == 0x96:  # ActionPush
            p = 0
            while p < len(payload):
                t = payload[p]
                p += 1
                if t == 0:
                    e = payload.index(b'\0', p)
                    out.append(payload[p:e].decode('utf-8', 'replace'))
                    p = e + 1
                elif t in (1, 7):
                    p += 4
                elif t == 6:
                    p += 8
                elif t in (4, 5, 8):
                    p += 1
                elif t == 9:
                    p += 2
                else:
                    break
    return out


def placement_name(body):
    """The instance name of a PlaceObject2/3 tag, if it has one."""
    m = re.search(rb'([A-Za-z_][A-Za-z0-9_]{2,60})\x00', body)
    return m.group(1).decode() if m else None


class Movie:
    def __init__(self, data):
        self.data = data
        self.histogram = collections.Counter()
        self.exports = {}  # character id -> exported symbol name
        self.sprites = {}  # character id -> nested tag data
        self.fonts = []
        self.strings = []
        self.main_placements = []
        self._walk(data, False)

    def _walk(self, data, nested):
        for code, body, _ in tags(data, nested):
            self.histogram[TAG_NAMES.get(code, code)] += 1
            if code in (12, 59):
                self.strings += as2_strings(body if code == 12 else body[2:])
            elif code == 56:
                p = 2
                for _ in range(struct.unpack_from('<H', body, 0)[0]):
                    cid = struct.unpack_from('<H', body, p)[0]
                    e = body.index(b'\0', p + 2)
                    self.exports[cid] = body[p + 2:e].decode('latin1')
                    p = e + 1
            elif code == 39:
                self.sprites[struct.unpack_from('<H', body, 0)[0]] = body[4:]
                self._walk(body[4:], True)
            elif code == 75:
                flags, name_len = body[2], body[4]
                name = body[5:5 + name_len].rstrip(b'\0').decode('utf-8', 'replace')
                glyphs = struct.unpack_from('<H', body, 5 + name_len)[0]
                self.fonts.append((name, glyphs, 'bold' if flags & 1 else '', 'italic' if flags & 2 else ''))
            elif code in (26, 70) and not nested:
                self.main_placements.append(placement_name(body))

    def sprite_placements(self, cid):
        return [placement_name(b) for c, b, _ in tags(self.sprites[cid], True) if c in (26, 70)]


def swf_movies(package):
    return [(i, e) for i, e in enumerate(package.exports) if package.class_of(e) == 'SwfMovie']


def cmd_list(args):
    paths = args.packages or sorted(str(COOKED / f) for f in os.listdir(COOKED) if f.endswith('.xxx'))
    for path in paths:
        try:
            p = ue3_package.load(path)
        except Exception as e:  # noqa: BLE001 (report and go on)
            print(f'{os.path.basename(path)}: {e}', file=sys.stderr)
            continue
        for i, e in swf_movies(p):
            print(f'{os.path.basename(path):40s} {p.path(i + 1):55s} {e["size"]}')


def cmd_extract(args):
    p = ue3_package.load(args.package)
    os.makedirs(args.out, exist_ok=True)
    wanted = set(args.names)
    for i, e in swf_movies(p):
        if e['name'] in wanted or p.path(i + 1) in wanted:
            data = movie_bytes(p.export_data(e))
            out = os.path.join(args.out, e['name'] + '.gfx')
            open(out, 'wb').write(data)
            print(f'{p.path(i + 1)} -> {out} ({len(data)} bytes, SWF version {data[3]})')


def cmd_info(args):
    m = Movie(open(args.movie, 'rb').read())
    print('tags:', dict(m.histogram.most_common()))
    print('fonts:', m.fonts)
    print('main timeline places:', [n for n in m.main_placements if n])
    for cid, name in sorted(m.exports.items(), key=lambda kv: kv[1]):
        placed = [n for n in m.sprite_placements(cid) if n] if cid in m.sprites else []
        print(f'  {name}' + (f': {placed}' if placed else ''))
    if args.strings:
        seen = list(dict.fromkeys(m.strings))
        print(f'{len(seen)} ActionScript strings:', seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('list')
    a.add_argument('packages', nargs='*')
    a.set_defaults(fn=cmd_list)
    a = sub.add_parser('extract')
    a.add_argument('package')
    a.add_argument('names', nargs='+')
    a.add_argument('--out', default=str(REPO_ROOT / 'build/gfx'))
    a.set_defaults(fn=cmd_extract)
    a = sub.add_parser('info')
    a.add_argument('movie')
    a.add_argument('--strings', action='store_true')
    a.set_defaults(fn=cmd_info)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
