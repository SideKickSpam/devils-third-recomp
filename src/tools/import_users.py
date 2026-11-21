#!/usr/bin/env python3
"""Which game functions use which imports, from the RPX relocations (imports are usually called indirectly,
so the relocation table is the reliable source).

Usage:
  import_users.py --lib snd_core [--lib snd_user ...]   functions using these libraries, with the imports used
  import_users.py --at 0x0465C7F0 [...]                 imports referenced by the function containing an address
Optional: --elf PATH (default game/dt.elf)
"""

import argparse
import bisect
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rpx_report import REPO_ROOT, parse_sections, parse_symbols

R_PPC_ADDR16_HA, R_PPC_REL24 = 6, 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", action="append", default=[])
    ap.add_argument("--at", action="append", default=[])
    ap.add_argument("--elf", default=str(REPO_ROOT / "game" / "dt.elf"))
    args = ap.parse_args()

    d = Path(args.elf).read_bytes()
    secs = parse_sections(d)
    symtab = next(s for s in secs if s.type == 2)
    syms = parse_symbols(d, symtab, secs[symtab.link])
    text = next(s for s in secs if s.name == ".text")
    fns = sorted((s["value"], s["size"], s["name"]) for s in syms
                 if s["type"] == 2 and s["shndx"] == text.index and s["size"])
    starts = [f[0] for f in fns]
    libs = {s.index: s.name.split("_", 1)[1] for s in secs if s.name.startswith((".fimport_", ".dimport_"))}

    refs = []  # (instruction address, "lib::name")
    for rel in (s for s in secs if s.name == ".rela.text"):
        for i in range(rel.size // 12):
            off, info, _ = struct.unpack_from(">IIi", d, rel.offset + i * 12)
            sym = syms[info >> 8]
            if sym["shndx"] in libs and (info & 0xFF) in (R_PPC_ADDR16_HA, R_PPC_REL24):
                refs.append((off, libs[sym["shndx"]], sym["name"]))
    refs.sort()

    def containing(addr):
        return fns[bisect.bisect_right(starts, addr) - 1]

    if args.lib:
        users = defaultdict(list)
        for off, lib, name in refs:
            if lib in args.lib:
                f = containing(off)
                if name not in users[f]:
                    users[f].append(name)
        for f, names in sorted(users.items()):
            print(f"{f[0]:08X} {f[2]}: {' '.join(names)}")
    for a in args.at:
        f = containing(int(a, 16))
        print(f"{f[0]:08X} {f[2]} ({f[1]} bytes)")
        for off, lib, name in refs:
            if f[0] <= off < f[0] + f[1]:
                print(f"   {off:08X} {lib}::{name}")


if __name__ == "__main__":
    main()
