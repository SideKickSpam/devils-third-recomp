#!/usr/bin/env python3
"""Instruction census of dt.elf's .text, compared against the opcodes XenonRecomp's recompiler handles.

Decodes .text with Capstone in 32-bit big-endian PowerPC + paired-single mode and counts mnemonics.
If a XenonRecomp checkout is given, every mnemonic is matched against the `case PPC_INST_*` labels in
its recompiler.cpp (uppercased, with '.' and branch hints stripped), and the unhandled ones are listed.

Requires capstone (pip install capstone==5.0.3).
Usage: insn_census.py [elf_path] [xenonrecomp_dir]
"""

import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capstone
from rpx_report import REPO_ROOT, parse_sections

DEFAULT_ELF = REPO_ROOT / "game" / "dt.elf"
DEFAULT_XENON = REPO_ROOT / "ref" / "XenonRecomp"


def census(data, text):
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
    code = data[text.offset:text.offset + text.size]
    counts = Counter()
    invalid = Counter()
    off = 0
    while off < len(code):
        for _addr, size, mnemonic, _ops in md.disasm_lite(code[off:], text.addr + off):
            counts[mnemonic] += 1
            off += size
        if off < len(code):
            # Capstone stops at the first word it cannot decode.
            (word,) = struct.unpack_from(">I", code, off)
            invalid[word >> 26] += 1
            off += 4
    return counts, invalid


# Capstone prints these rlwinm forms as aliases; binutils (XenonRecomp's disassembler) prints rlwinm.
ALIASES = {"SLWI": "RLWINM", "SRWI": "RLWINM"}


def normalize(mnemonic):
    m = mnemonic.rstrip("+-").rstrip(".").upper()
    return ALIASES.get(m, m)


def main():
    elf_path = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_ELF
    xenon = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else DEFAULT_XENON
    data = elf_path.read_bytes()
    text = next(s for s in parse_sections(data) if s.name == ".text")
    counts, invalid = census(data, text)
    total = sum(counts.values()) + sum(invalid.values())

    handled = set()
    recompiler = xenon / "XenonRecomp" / "recompiler.cpp"
    if recompiler.exists():
        handled = set(re.findall(r"case PPC_INST_([A-Z0-9_]+):", recompiler.read_text()))

    print(f".text words: {total:,}  decoded: {sum(counts.values()):,}  undecodable: {sum(invalid.values()):,}")
    if invalid:
        print("undecodable by primary opcode: " + ", ".join(f"{op}:{n}" for op, n in invalid.most_common()))
    print(f"distinct mnemonics: {len(counts)}")
    if handled:
        missing = Counter()
        for m, n in counts.items():
            if normalize(m) not in handled:
                missing[normalize(m)] += n
        covered = total - sum(missing.values()) - sum(invalid.values())
        print(f"XenonRecomp handles {len(handled)} opcodes; {len(counts) - len(missing)} of this game's mnemonics match, "
              f"covering {covered:,} words ({100.0 * covered / total:.2f}%)")
        print(f"unhandled mnemonics: {len(missing)}, {sum(missing.values()):,} words")
        for m, n in missing.most_common():
            print(f"  {m:12} {n:>9,}")
    print("top 40 mnemonics:")
    for m, n in counts.most_common(40):
        print(f"  {m:12} {n:>9,}")


if __name__ == "__main__":
    main()
