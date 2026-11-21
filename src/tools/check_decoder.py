#!/usr/bin/env python3
"""Cross-check dt_recomp's Espresso decoder against Capstone over every word of .text.

Capstone prints simplified mnemonics (mr, blr, slwi, ...); these are mapped back to base instructions
before comparing with `dt_recomp <elf> --decode-dump <file>` output.

Usage: tools/venv/bin/python -I src/tools/check_decoder.py <decode_dump.txt> [elf_path]
"""

import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capstone
from rpx_report import REPO_ROOT, parse_sections

BASE = {
    "mr": "or", "li": "addi", "lis": "addis", "la": "addi", "nop": "ori", "not": "nor", "subi": "addi",
    "subis": "addis", "subic": "addic", "subic.": "addic.", "sub": "subf", "subc": "subfc", "cmpwi": "cmpi",
    "cmpw": "cmp", "cmplwi": "cmpli", "cmplw": "cmpl", "slwi": "rlwinm", "srwi": "rlwinm", "clrlwi": "rlwinm",
    "clrrwi": "rlwinm", "rotlwi": "rlwinm", "rotrwi": "rlwinm", "extlwi": "rlwinm", "extrwi": "rlwinm",
    "clrlslwi": "rlwinm", "rotlw": "rlwnm", "inslwi": "rlwimi", "insrwi": "rlwimi", "crclr": "crxor",
    "crset": "creqv", "crmove": "cror", "crnot": "crnor", "mtcr": "mtcrf", "trap": "tw", "mftbu": "mftb",
    "mtctr": "mtspr", "mtlr": "mtspr", "mtxer": "mtspr", "mfctr": "mfspr", "mflr": "mfspr", "mfxer": "mfspr",
    "neg": "neg", "mullw": "mullw", "xnop": "xori", "nand": "nand", "eieio": "eieio",
}


def base_mnemonic(m):
    m = m.rstrip("+-")
    rec = m.endswith(".") and m not in ("stwcx.", "addic.", "andi.", "andis.")
    if rec:
        m = m[:-1]
    if m in BASE:
        return BASE[m]
    if m in ("b", "ba", "bl", "bla"):
        return "b"
    if m.startswith("b"):
        if re.search(r"lrl?$", m):
            return "bclr"
        if re.search(r"ctrl?$", m):
            return "bcctr"
        return "bc"
    if re.fullmatch(r"tw(i|[a-z]{1,3}i)", m) or (m.startswith("tw") and m.endswith("i")):
        return "twi"
    if m.startswith("tw"):
        return "tw"
    if m.endswith("o") and m[:-1] in ("add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe", "subfze",
                                      "subfme", "neg", "mullw", "divw", "divwu"):
        return m[:-1]
    return m


def main():
    dump = Path(sys.argv[1])
    elf = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO_ROOT / "game" / "dt.elf"
    data = elf.read_bytes()
    text = next(s for s in parse_sections(data) if s.name == ".text")
    ours = {}
    for line in dump.read_text().splitlines():
        addr, mnem = line.split(" ", 1)
        ours[int(addr, 16)] = mnem.rstrip(".") if mnem not in ("stwcx.", "addic.", "andi.", "andis.") else mnem

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
    code = data[text.offset:text.offset + text.size]
    mismatches = Counter()
    examples = {}
    checked = 0
    off = 0
    while off < len(code):
        for addr, size, mnem, ops in md.disasm_lite(code[off:], text.addr + off):
            mine = ours.get(addr)
            theirs = base_mnemonic(mnem)
            checked += 1
            if mine != theirs:
                key = (theirs, mine)
                mismatches[key] += 1
                examples.setdefault(key, f"{addr:#010x}: {mnem} {ops}")
            off += size
        if off < len(code):
            addr = text.addr + off
            if ours.get(addr) != "(invalid)":
                mismatches[("<capstone invalid>", ours.get(addr))] += 1
            off += 4
    print(f"checked {checked:,} words; mismatching words: {sum(mismatches.values()):,}")
    for (theirs, mine), n in mismatches.most_common(40):
        print(f"  capstone {theirs!s:12} ours {mine!s:12} x{n:<8,} e.g. {examples.get((theirs, mine), '')}")


if __name__ == "__main__":
    main()
