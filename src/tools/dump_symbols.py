#!/usr/bin/env python3
"""Dump dt.elf's defined FUNC/OBJECT symbols with demangled names to a CSV.

Columns: address, size, mangled, demangled, section, status
  status = ok (demangled), plain (not a mangled name; demangled == mangled), failed (demangled empty)

Undefined symbols (the value-0 WEAK leftovers) have no address and are not written, but they are
still run through the demangler and counted in the summary.

Usage: dump_symbols.py [elf_path] [csv_path]
Defaults are game/dt.elf and src/notes/symbols.csv relative to the repo root.
"""

import csv
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # also works under python -I

import ghs_demangle
from rpx_report import REPO_ROOT, SHN_UNDEF, parse_sections, parse_symbols

DEFAULT_ELF = REPO_ROOT / "game" / "dt.elf"
DEFAULT_CSV = REPO_ROOT / "src" / "notes" / "symbols.csv"
STT_OBJECT, STT_FUNC = 1, 2


def classify(name):
    try:
        out = ghs_demangle.demangle(name)
    except ghs_demangle.DemangleError as e:
        return "failed", "", str(e)
    if out is None:
        return "plain", name, None
    return "ok", out, None


def main():
    elf_path = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_ELF
    csv_path = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else DEFAULT_CSV
    data = elf_path.read_bytes()
    sections = parse_sections(data)
    symtab = next(s for s in sections if s.type == 2)
    syms = [s for s in parse_symbols(data, symtab, sections[symtab.link])
            if s["type"] in (STT_FUNC, STT_OBJECT) and s["name"]]

    defined = sorted((s for s in syms if s["shndx"] != SHN_UNDEF), key=lambda s: (s["value"], s["name"]))
    undefined = [s for s in syms if s["shndx"] == SHN_UNDEF]

    stats = Counter()
    failures = []
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "size", "mangled", "demangled", "section", "status"])
        for s in defined:
            status, out, err = classify(s["name"])
            stats[status] += 1
            if err:
                failures.append((s["name"], err))
            w.writerow([f"0x{s['value']:08x}", s["size"], s["name"], out, sections[s["shndx"]].name, status])

    und_stats = Counter()
    for s in undefined:
        status, _, err = classify(s["name"])
        und_stats[status] += 1
        if err:
            failures.append((s["name"], err))

    mangled = stats["ok"] + stats["failed"]
    print(f"wrote {csv_path} ({len(defined):,} defined symbols)")
    print(f"defined:   {stats['ok']:,} demangled, {stats['plain']:,} plain C names, "
          f"{stats['failed']:,} failed ({100.0 * stats['failed'] / max(mangled, 1):.3f}% of mangled names)")
    print(f"undefined: {und_stats['ok']:,} demangled, {und_stats['plain']:,} plain, {und_stats['failed']:,} failed "
          "(not written to the CSV)")
    for name, err in failures:
        print(f"FAILED: {name}  ({err})")


if __name__ == "__main__":
    main()
