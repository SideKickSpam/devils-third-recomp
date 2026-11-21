#!/usr/bin/env python3
"""Analyze a decompressed Wii U RPX (ELF32 big-endian PPC) and write a Markdown report.

Reports:
  * imported functions/data grouped by library (.fimport_<lib> / .dimport_<lib>)
  * FUNC symbols in .symtab that are neither imports nor exports
  * paired-single instruction counts in .text vs. total instruction count

Usage: rpx_report.py [elf_path] [report_path]
Defaults are game/dt.elf and src/notes/rpx_report.md relative to the repo root.
Standard library only.
"""

import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ELF = REPO_ROOT / "game" / "dt.elf"
DEFAULT_REPORT = REPO_ROOT / "src" / "notes" / "rpx_report.md"

SHT_SYMTAB = 2
SHT_RPL_EXPORTS = 0x80000001
SHT_RPL_IMPORTS = 0x80000002
SHF_RPL_ZLIB = 0x08000000
SHN_UNDEF = 0

STT_NAMES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE"}
STB_NAMES = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}
STT_FUNC = 2

# Primary opcodes for Espresso paired-single loads/stores (D-form).
PSQ_DFORM = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}
# Opcode-4 indexed paired-single loads/stores, 6-bit XO at bits 25-30.
PSQ_XFORM = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}
# Opcode-4 A-form ps math, 5-bit XO at bits 26-30.
PS_AFORM = {
    10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1",
    14: "ps_madds0", 15: "ps_madds1", 18: "ps_div", 20: "ps_sub",
    21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul",
    26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
    31: "ps_nmadd",
}
# Opcode-4 X-form ops, 10-bit XO at bits 21-30.
PS_XFORM = {
    0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1",
    40: "ps_neg", 72: "ps_mr", 136: "ps_nabs", 264: "ps_abs",
    528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
    624: "ps_merge11",
}
DCBZ_L_XO = 1014


class Section:
    def __init__(self, index, name_off, sh_type, flags, addr, offset, size, link, info, entsize):
        self.index = index
        self.name_off = name_off
        self.name = ""
        self.type = sh_type
        self.flags = flags
        self.addr = addr
        self.offset = offset
        self.size = size
        self.link = link
        self.info = info
        self.entsize = entsize


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("utf-8", errors="replace")


def parse_sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        sys.exit("not an ELF32 big-endian file")
    (e_shoff,) = struct.unpack_from(">I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", data, 0x2E)
    sections = []
    for i in range(e_shnum):
        fields = struct.unpack_from(">10I", data, e_shoff + i * e_shentsize)
        name_off, sh_type, flags, addr, offset, size, link, info, _align, entsize = fields
        sections.append(Section(i, name_off, sh_type, flags, addr, offset, size, link, info, entsize))
    shstr = sections[e_shstrndx]
    for s in sections:
        s.name = cstr(data, shstr.offset + s.name_off)
    compressed = [s.name for s in sections if s.flags & SHF_RPL_ZLIB]
    if compressed:
        sys.exit(f"sections still zlib-compressed (run wiiurpxtool -d first): {compressed}")
    return sections


def parse_symbols(data, symtab, strtab):
    syms = []
    for i in range(symtab.size // 16):
        name_off, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", data, symtab.offset + i * 16)
        syms.append({
            "index": i,
            "name": cstr(data, strtab.offset + name_off),
            "value": value,
            "size": size,
            "type": info & 0xF,
            "bind": info >> 4,
            "shndx": shndx,
        })
    return syms


def parse_exports(data, sec):
    """RPL export table: u32 count, u32 signature, then {u32 value, u32 name_off} entries.
    name_off is relative to the section start; the high bit flags a TLS export."""
    count, _sig = struct.unpack_from(">II", data, sec.offset)
    exports = []
    for i in range(count):
        value, name_off = struct.unpack_from(">II", data, sec.offset + 8 + i * 8)
        exports.append((value, cstr(data, sec.offset + (name_off & 0x7FFFFFFF))))
    return exports


def classify_op4(insn):
    xo6 = (insn >> 1) & 0x3F
    if xo6 in PSQ_XFORM:
        return "psq_x", PSQ_XFORM[xo6]
    xo10 = (insn >> 1) & 0x3FF
    if xo10 == DCBZ_L_XO:
        return "dcbz_l", "dcbz_l"
    if xo10 in PS_XFORM:
        return "ps_math", PS_XFORM[xo10]
    xo5 = (insn >> 1) & 0x1F
    if xo5 in PS_AFORM:
        return "ps_math", PS_AFORM[xo5]
    return "unknown", f"op4 xo10={xo10}"


def scan_text(data, text):
    total = text.size // 4
    words = struct.unpack_from(f">{total}I", data, text.offset)
    primary = Counter(w >> 26 for w in words)
    op4_class = Counter()
    mnemonics = Counter()
    for w in words:
        op = w >> 26
        if op == 4:
            cls, mn = classify_op4(w)
            op4_class[cls] += 1
            mnemonics[mn] += 1
        elif op in PSQ_DFORM:
            mnemonics[PSQ_DFORM[op]] += 1
    zero_words = sum(1 for w in words if w == 0)
    # ps_merge10 fN,fN,fN swaps a register's halves in place; the compiler emits it on nonvolatile FPRs.
    self_swaps = sum(1 for w in words if w >> 26 == 4 and (w >> 1) & 0x3FF == 592
                     and (w >> 21) & 31 == (w >> 16) & 31 == (w >> 11) & 31)
    return total, primary, op4_class, mnemonics, zero_words, self_swaps


def pct(n, d):
    return f"{100.0 * n / d:.2f}%" if d else "n/a"


def build_report(elf_path):
    data = elf_path.read_bytes()
    sections = parse_sections(data)
    by_name = {s.name: s for s in sections}

    symtab = next(s for s in sections if s.type == SHT_SYMTAB)
    syms = parse_symbols(data, symtab, sections[symtab.link])

    import_secs = {}  # shndx -> (lib, kind)
    for s in sections:
        for prefix, kind in ((".fimport_", "func"), (".dimport_", "data")):
            if s.name.startswith(prefix):
                import_secs[s.index] = (s.name[len(prefix):], kind)

    export_entries = []
    for s in sections:
        if s.type == SHT_RPL_EXPORTS:
            kind = "func" if s.name == ".fexports" else "data"
            export_entries += [(kind, v, n) for v, n in parse_exports(data, s)]
    export_names = {n for _, _, n in export_entries}

    # --- imports ---
    imports = defaultdict(lambda: {"func": [], "data": []})
    for sym in syms:
        if sym["shndx"] in import_secs and sym["type"] != 3:  # skip the SECTION symbols
            lib, kind = import_secs[sym["shndx"]]
            imports[lib][kind].append(sym["name"])

    # --- FUNC symbols that are not imports or exports ---
    funcs = [s for s in syms if s["type"] == STT_FUNC]
    non_import = [s for s in funcs if s["shndx"] not in import_secs]
    exported_funcs = [s for s in non_import if s["name"] in export_names]
    internal = [s for s in non_import if s["name"] not in export_names]
    internal_def = [s for s in internal if s["shndx"] != SHN_UNDEF]
    internal_und = [s for s in internal if s["shndx"] == SHN_UNDEF]

    text = by_name[".text"]
    text_end = text.addr + text.size
    in_text = [s for s in internal_def if text.addr <= s["value"] < text_end]
    unique_addrs = {s["value"] for s in in_text}
    # Coverage of .text by sized function symbols (aliases at the same address counted once).
    covered = 0
    cursor = text.addr
    for start, size in sorted({(s["value"], s["size"]) for s in in_text}):
        end = min(start + size, text_end)
        if end > cursor:
            covered += end - max(start, cursor)
            cursor = end
    def_bind = Counter(STB_NAMES.get(s["bind"], str(s["bind"])) for s in internal_def)
    def_sec = Counter(sections[s["shndx"]].name for s in internal_def)
    und_bind = Counter(STB_NAMES.get(s["bind"], str(s["bind"])) for s in internal_und)
    und_names_also_defined = len({s["name"] for s in internal_und} & {s["name"] for s in internal_def})

    # --- paired singles ---
    total, primary, op4_class, mnemonics, zero_words, self_swaps = scan_text(data, text)
    ps_math = op4_class["ps_math"]
    psq_x = op4_class["psq_x"]
    psq_d = sum(primary[op] for op in PSQ_DFORM)
    ps_total = ps_math + psq_x + psq_d

    out = []
    w = out.append
    w("# RPX analysis report\n")
    w(f"Generated by `src/tools/rpx_report.py` from `{elf_path.relative_to(REPO_ROOT) if elf_path.is_relative_to(REPO_ROOT) else elf_path}` "
      f"({len(data):,} bytes, {len(sections)} sections, {len(syms):,} symbols).\n")

    w("## Summary\n")
    total_fi = sum(len(v["func"]) for v in imports.values())
    total_di = sum(len(v["data"]) for v in imports.values())
    w(f"- **Imports:** {total_fi:,} functions + {total_di:,} data symbols from {len(imports)} libraries.")
    w(f"- **Exports:** {len(export_entries)} ({', '.join(n for _, _, n in export_entries)}).")
    w(f"- **Internal FUNC symbols (not imports or exports): {len(internal):,}**, made up of")
    w(f"  - {len(internal_def):,} defined in a section ({len(unique_addrs):,} distinct addresses in `.text`),")
    w(f"  - {len(internal_und):,} undefined (`UND`), which are leftover names of discarded weak copies with no code (see below).")
    w(f"- Named functions cover **{pct(covered, text.size)}** of `.text`, so **the game shipped with internal function names** "
      "(mangled C++ names that look like Unreal Engine 3).")
    w(f"- **Paired-single instructions:** {ps_total:,} of {total:,} `.text` words ({pct(ps_total, total)}).\n")

    w("## Imports by library\n")
    w("| Library | Functions | Data | Total |")
    w("|---|---:|---:|---:|")
    for lib in sorted(imports, key=lambda l: (-len(imports[l]["func"]) - len(imports[l]["data"]), l)):
        f, d = len(imports[lib]["func"]), len(imports[lib]["data"])
        w(f"| {lib} | {f} | {d} | {f + d} |")
    w(f"| **Total** | **{total_fi}** | **{total_di}** | **{total_fi + total_di}** |\n")
    w("These counts are named symbols in each import section. The sections are much larger than that: "
      "`.fimport_coreinit` has room for about 1,100 eight-byte stubs, and its header count is 0x44F. Only the "
      "slots with a symbol are counted here.\n")

    w("## Internal function symbols\n")
    w("A FUNC symbol counts as internal when it is not in a `.fimport_*`/`.dimport_*` section and its name "
      "is not in `.fexports`/`.dexports`.\n")
    w("| Category | Count |")
    w("|---|---:|")
    w(f"| All FUNC symbols | {len(funcs):,} |")
    w(f"| Imports | {len(funcs) - len(non_import):,} |")
    w(f"| Exports | {len(exported_funcs):,} |")
    w(f"| **Internal** | **{len(internal):,}** |")
    w(f"| of which defined | {len(internal_def):,} |")
    w(f"| of which undefined (UND) | {len(internal_und):,} |\n")
    w("Defined internal functions by section: " + ", ".join(f"`{k}` {v:,}" for k, v in def_sec.most_common()) + ".  ")
    w("By binding: " + ", ".join(f"{k} {v:,}" for k, v in def_bind.most_common()) + ".  ")
    w(f"Distinct start addresses in `.text`: {len(unique_addrs):,}. Named functions cover {covered:,} of {text.size:,} "
      f"`.text` bytes ({pct(covered, text.size)}); only {text.size - covered:,} bytes are outside any named function.\n")
    w(f"The {len(internal_und):,} undefined FUNC symbols are all value 0 with non-zero sizes "
      f"(binding: {', '.join(f'{k} {v:,}' for k, v in und_bind.most_common())}). Names shared with a defined "
      f"function: {und_names_also_defined}. They look like leftover names of weak or inline copies "
      "that the linker dropped, so they don't point at any code.\n")
    w("Sample of defined internal names (first 15 in `.text` by address):\n")
    w("```")
    seen = set()
    for s in sorted(in_text, key=lambda s: s["value"]):
        if s["value"] in seen:
            continue
        seen.add(s["value"])
        w(f"0x{s['value']:08x}  {s['size']:>6}  {s['name']}")
        if len(seen) == 15:
            break
    w("```\n")

    w("## Paired-single instructions in `.text`\n")
    w(f"`.text` is {text.size:,} bytes = **{total:,} instruction words**. This counts every 4-byte word, so any "
      f"data or padding inside `.text` is included (there are {zero_words:,} all-zero words).\n")
    w("| Group | Count | % of .text |")
    w("|---|---:|---:|")
    w(f"| Opcode 4: ps_* math/move/compare/merge | {ps_math:,} | {pct(ps_math, total)} |")
    w(f"| Opcode 4: indexed psq_lx/stx/lux/stux | {psq_x:,} | {pct(psq_x, total)} |")
    for op, mn in PSQ_DFORM.items():
        w(f"| Opcode {op}: {mn} | {primary[op]:,} | {pct(primary[op], total)} |")
    w(f"| **Paired-single total** | **{ps_total:,}** | **{pct(ps_total, total)}** |")
    w(f"| Opcode 4: dcbz_l (cache op, not paired-single) | {op4_class['dcbz_l']:,} | {pct(op4_class['dcbz_l'], total)} |")
    w(f"| Opcode 4: unrecognized encodings | {op4_class['unknown']:,} | {pct(op4_class['unknown'], total)} |")
    w(f"| All primary opcode 4 | {primary[4]:,} | {pct(primary[4], total)} |\n")
    w("Primary opcode 4 isn't only `ps_*` math. On Espresso it also holds the indexed `psq_*x` loads/stores and "
      "`dcbz_l`, so those are split out above. The \"paired-single total\" is all opcode-4 paired-single "
      "encodings plus opcodes 56/57/60/61.\n")
    w(f"Of the {mnemonics['ps_merge10']:,} `ps_merge10`, **{self_swaps:,}** have the form `ps_merge10 fN,fN,fN` "
      "(mostly f26-f31, the callee-saved registers). That swaps the register's two halves in place and is "
      "probably compiler save/restore code, not vector math. Leaving those out gives "
      f"**{ps_total - self_swaps:,}** paired-single instructions ({pct(ps_total - self_swaps, total)}).\n")
    w("Paired-single mnemonics, most common first:\n")
    w("| Mnemonic | Count |")
    w("|---|---:|")
    for mn, c in mnemonics.most_common():
        if mn != "dcbz_l" and not mn.startswith("op4 "):
            w(f"| `{mn}` | {c:,} |")
    w("")

    w("## Imported symbols\n")
    for lib in sorted(imports):
        for kind in ("func", "data"):
            names = sorted(imports[lib][kind])
            if not names:
                continue
            w(f"### {lib} ({'functions' if kind == 'func' else 'data'}: {len(names)})\n")
            w("```")
            w("\n".join(names))
            w("```\n")

    summary = {
        "imports_func": total_fi, "imports_data": total_di, "libs": len(imports),
        "internal": len(internal), "internal_def": len(internal_def), "internal_und": len(internal_und),
        "unique_addrs": len(unique_addrs), "coverage": pct(covered, text.size),
        "ps_total": ps_total, "ps_math": ps_math, "psq_x": psq_x, "psq_d": psq_d,
        "dcbz_l": op4_class["dcbz_l"], "ps_merge10_self_swaps": self_swaps, "op4_unknown": op4_class["unknown"], "total_insns": total,
        "per_lib": {lib: (len(v["func"]), len(v["data"])) for lib, v in imports.items()},
        "psq_by_op": {PSQ_DFORM[op]: primary[op] for op in PSQ_DFORM},
        "exports": [n for _, _, n in export_entries],
    }
    return "\n".join(out), summary


def main():
    elf_path = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_ELF
    report_path = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else DEFAULT_REPORT
    report, summary = build_report(elf_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(report)
    print(f"wrote {report_path}")
    for k, v in summary.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()
