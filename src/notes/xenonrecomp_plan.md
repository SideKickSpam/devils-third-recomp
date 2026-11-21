# Adapting XenonRecomp to Wii U RPX

What in hedge-dev/XenonRecomp is tied to the Xbox 360, and what we change so it can statically recompile
`game/dt.elf` (Devil's Third, a decompressed Wii U RPX) into C++.

Source read: `ref/XenonRecomp` at commit `ddd128b` (all line references below are to that checkout).
Facts about dt.elf come from `src/tools/rpx_report.py`, `src/tools/dump_symbols.py`, `src/tools/insn_census.py`
and one-off checks noted inline.

## Summary

XenonRecomp's core is reusable: it translates one PowerPC instruction at a time into C++ that operates on a
`PPCContext` register file and a 4 GB big-endian guest memory block, emitting one C++ function per guest
function. Everything around that core assumes an Xbox 360 executable and the Xenon CPU:

- The loader reads XEX2 (encrypted, LZX-compressed PE images), takes function boundaries from the PE `.pdata`
  exception table, and patches import thunks found through Xbox kernel ordinal tables.
- The register model is 64-bit, with 128 VMX128 vector registers and FPU/VMX flush-mode tracking.
- The disassembler runs in Cell/Xenon mode, which has no paired-single instructions and decodes primary
  opcode 4 as VMX. **On Espresso, opcode 4 is paired-single, so the decoder would silently mistranslate those
  instructions.**
- Jump-table detection matches MSVC code patterns.

dt.elf is unusually friendly to static recompilation:

- **Full symbol table.** 93,054 function symbols cover `.text` with no overlaps; only 896 bytes are unnamed.
- **Relocations already applied** at the default load addresses for everything except imports.
- **No small-data registers.** r2/r13 are not used as SDA bases.
- **Simple switch codegen.** Switches compile to inline branch tables.
- **Coverage.** 99.12% of `.text` words are mnemonics XenonRecomp already translates.

The work is mostly replacing the front end (loader, analysis, decoder) and changing the register model;
most per-instruction emitters carry over with 32-bit fixes.

## How XenonRecomp works

1. `XenonRecomp/main.cpp`: load TOML config, `Recompiler::LoadConfig` → `Image::ParseImage`
   (`XenonUtils/image.cpp`), which dispatches on magic to `Xex2LoadImage` (`XenonUtils/xex.cpp:127`) or a
   minimal `ElfLoadImage` (`XenonUtils/image.cpp:46`).
2. `Recompiler::Analyse` (`XenonRecomp/recompiler.cpp:96`): builds the function list from MSVC save/restore
   helper addresses in the config, explicit `functions` from the config, `.pdata`, `bl` targets, and a linear
   sweep with the `Function::Analyze` block walker (`XenonAnalyse/function.cpp:41`) to fill the rest.
3. `Recompiler::Recompile(fn)`: disassembles each instruction with the bundled binutils decoder
   (`thirdparty/disasm`, wrapped by `XenonUtils/disasm.cpp`) and emits C++ through a ~320-case `switch` on
   `PPC_INST_*` (`recompiler.cpp:257-2270`). Branches inside a function become `goto loc_X`; calls become
   `sub_X(ctx, base)`; `bctr`/`bctrl` go through `PPC_CALL_INDIRECT_FUNC`.
4. `Recompiler::Recompile(header)` (`recompiler.cpp:2503`): writes `ppc_config.h`, `ppc_context.h` (copied from
   `XenonUtils/ppc_context.h`), `ppc_recomp_shared.h` (one extern per symbol), `ppc_func_mapping.cpp`
   (guest address → host function table), and `ppc_recomp.N.cpp` (256 functions per file).
5. `XenonAnalyse` is a separate tool that pattern-matches switch tables in the XEX and writes them to a TOML
   file the recompiler reads.

The host runtime (memory allocation, kernel/import implementations, threads, GPU) is not part of
XenonRecomp; projects such as UnleashedRecomp supply it.

## What is Xbox 360 specific

### 1. XEX loading

| Piece | Where | Wii U status |
|---|---|---|
| XEX2 header parsing, AES-CBC decryption with the retail key, basic/LZX decompression, PE section mapping | `XenonUtils/xex.cpp:127-305`, `xex.h` | Remove. We start from `dt.elf`, already decompressed by wiiurpxtool. |
| XEX delta patches (title updates) | `XenonUtils/xex_patcher.*`, `recompiler.cpp:18-90` | Remove. Wii U updates ship a whole replacement RPX; we already use the update's. |
| XDBF (dashboard resources, achievements) | `XenonUtils/xdbf*` | Remove. |
| PE/Xbox kernel structures, `.pdata` records | `XenonUtils/xbox.h` | Remove. |
| Third-party deps only XEX needs: `tiny-AES-c`, `TinySHA1`, `libmspack` (LZX) | `thirdparty/` | Drop from the build. |

The existing `ElfLoadImage` cannot load an RPX as is: it takes `image.base` from the first `PT_LOAD` program
header (an RPX has none), maps every non-null section including `.rela.*`, `.symtab` and the import sections
(several share address 0, and `Image::sections` is a set keyed by base, so they silently collide), points
`.bss` at file bytes, and never reads `.symtab`.

### 2. Function discovery

`Analyse` dereferences `*image.Find(".pdata")` unconditionally (`recompiler.cpp:177`). That PE exception
table is where Xbox titles get exact function starts and lengths; an RPX has no `.pdata`, so this would
crash. The rest of discovery is heuristic (`bl` targets, block walking). It also synthesises MSVC register
save/restore helpers from `restgprlr_14_address` and similar config keys (`recompiler.cpp:98-169`), which
follow the Xbox MSVC ABI.

### 3. Imports

Xbox imports are resolved by ordinal through generated tables (`XenonUtils/xbox/xboxkrnl_table.inc`,
`xam_table.inc`, 2,685 lines). Each import thunk inside the image is overwritten with
`nop; nop; nop; blr` and registered as a 16-byte function named `__imp__<Name>` (`xex.cpp:335`), which the
host then implements. Imports from any module other than `xboxkrnl.exe`/`xam.xex` get no names.

### 4. 64-bit assumptions

- `PPCRegister` (`XenonUtils/ppc_context.h:126`) is a 64-bit union, and most GPR emitters write `.u64`/`.s64`
  (125 emitter lines), e.g. `addi` → `rD.s64 = rA.s64 + imm`, `rlwinm` rotates a 64-bit value built from the
  low word duplicated (`recompiler.cpp:1392`). Carries and CR compares already use 32-bit views, because
  Xbox code is 32-bit code running on a 64-bit CPU.
- 34 emitters exist only for 64-bit instructions (`ld`/`std`, `rldicl`/`rldicr`/`rldimi`, `cmpd`, `divd`,
  `mulld`, `sradi`, `extsw`, `fcfid`, `fctid`, `mtmsrd`, `td*`). None of them occur in dt.elf.
- `mftb` becomes `__rdtsc()` (`recompiler.cpp:1262`), and `mfmsr`/`mtmsrd` model the Xbox MSR.

### 5. VMX / VMX128

- `PPCContext` carries 128 vector registers (`v0`-`v127`, `ppc_context.h:411`). There are 130 VMX/VMX128
  emitter cases, starting at `recompiler.cpp:1737`.
- VMX runs in non-IEEE (flush-denormals) mode while the FPU does not, so the recompiler tracks a
  `CSRState` and emits `ctx.fpscr.enable/disableFlushMode()` around FPU and VMX instructions
  (`printSetFlushMode`, `recompiler.cpp:423`, 60 call sites).
- Espresso has no VMX. All of this goes, along with `setFromMask` on CR and the `v` local-variable options.

### 6. The decoder

`XenonUtils/disasm.cpp:3` constructs the binutils disassembler with options `"cell 64"`. Its opcode table
(`thirdparty/disasm/ppc-dis.c`, 1,846 `PPC_INST_*` ids) contains no paired-single or `psq_*` entries, and
in that mode primary opcode 4 decodes as VMX. **Without a decoder change, the game's 28,659 opcode-4 words
would turn into wrong AltiVec code instead of failing loudly.** Opcodes 56/57/60/61 (`psq_l`/`psq_lu`/
`psq_st`/`psq_stu`) are similarly claimed by 64-bit instructions.

### 7. Switch tables

`XenonAnalyse/main.cpp` scans for four MSVC code shapes (absolute, computed, byte-offset and short-offset
tables, `main.cpp:299-306`) and reads table data from the image. The recompiler also warns on a `bctr`
preceded by specific Xenon instruction words (`recompiler.cpp:2427`). None of this matches Green Hills output.

### 8. Smaller Xbox assumptions

- **MMIO.** `mmioStore` treats a store followed by `eieio` as a GPU register write (`recompiler.cpp:367`).
  Wii U games reach the GPU through the GX2 library, not MMIO.
- **Indirect calls.** `PPC_LOOKUP_FUNC` (`ppc_context.h:110`) keeps the indirect-call table inside guest
  memory directly after the image, 8 bytes per 4-byte instruction. The layout assumes one contiguous PE image.
- **setjmp/longjmp.** Both are special-cased by configured address.
- **Local-variable options.** These assume the MSVC Xbox ABI's volatile/non-volatile split.

## What dt.elf looks like

| Fact | Value | Source |
|---|---|---|
| Layout | `.text` 0x02000020–0x04A6E118 (44.5 MB); `.rodata` 0x10000000; `.data` 0x104634C0; `.bss` 0x104B7C00 (1.7 MB); import sections at 0xC0A30A00+ | `readelf -S` |
| Entry point | `_start` at 0x0465B944; `main` at 0x03B05A7C; only export `__preinit_user` | symtab |
| Function symbols | 93,054 distinct starts in `.text`, no overlaps, no size conflicts; 8 aliased addresses (`strlen`/`__ghs_strlen`) | one-off check |
| Unnamed `.text` | one orphan `blr` at 0x0465DD34, 568 undecodable bytes at 0x047E5258, three 4-byte zero words | one-off check |
| Name hygiene | 2,742 names reused at different addresses (per-file local copies); 1 name that is not a C identifier (`__ghscallpatch_.text___dotsyscall_0`); internal functions named `malloc`, `free`, `printf`, `memmove`, `sqrt`, `strlen`… | one-off check |
| Imports | 413 functions + 10 data symbols from 18 libraries | `rpx_report.md` |
| Instructions | 11,122,750 words, 220 distinct mnemonics; 99.12% of words are mnemonics XenonRecomp handles | `insn_census.py` |
| Unhandled mnemonics | 63 mnemonics, 97,652 words: `lmw` 25,906, `ps_merge10` 25,427, `stmw` 16,164, `isync` 9,949, `lhzu` 3,385, `crclr` 3,191, `psq_l` 2,859, `psq_st` 2,024, then update-form loads/stores, `addc`, `dcbst`, `crset`, the remaining `ps_*`, `lswi`/`stswi`, `mtspr`/`mfspr`, `eqv`, `rlwnm`, `trap`, conditional calls (`bnel`/`beql`) | `insn_census.py` |
| Paired singles | 33,542 words; 25,253 are `ps_merge10 fN,fN,fN` in prologues/epilogues (saving ps1 of f14–f31) | `rpx_report.md` |
| GQRs | `mtspr`/`mfspr` on SPR 898–903 (UGQR2–UGQR7): the game programs its own quantisation | one-off check |
| Atomics/sync | `lwarx`/`stwcx.` 2,259 each; `isync` 9,949; `sync` 81; `eieio` 49; no `sc` in `.text` | one-off check |
| Calls | `bl` 165,497; `bctrl` 441,291; `bctr` 6,688 | `insn_census.py` |

## Changes for Wii U

### A. ELF/RPX loader with function boundaries from the symbol table

Replace `ElfLoadImage` with an `RpxLoadImage`:

1. Map only allocated sections (`SHF_ALLOC`) at their `sh_addr`: `.syscall`, `.text`, `.rodata`, `.data`,
   `.module_id`, and `.bss` as zero-filled memory. Mark `.text` and `.syscall` as code. Keep `.symtab`,
   `.strtab`, `.rela.*`, `.fexports` and the import sections as metadata, not mapped memory. Set
   `image.base` to 0 and use absolute addresses throughout, because the RPX has no program headers.
2. Refuse input whose sections still have `SHF_RPL_ZLIB` (0x08000000) set, so a compressed RPX fails
   clearly. Decompressing in the loader with zlib is optional later.
3. Read `.symtab`. Every `STT_FUNC` in `.text`/`.syscall` with a non-zero size becomes a `Function`
   and `Symbol_Function`. Skip the 15,931 undefined weak leftovers (value 0). For the 8 aliased addresses,
   pick one canonical symbol (prefer `GLOBAL`, then the mangled name) and emit the others as aliases.
4. Delete the `.pdata` pass, the MSVC helper synthesis and the linear-sweep `Function::Analyze` fallback
   from `Analyse`. Keep a sweep only for the unnamed ranges listed above, and report anything it finds
   instead of guessing.
5. Generate identifiers rather than using raw symbol names. Prefix everything (`g_` or `sub_<addr>_<name>`)
   so that internal `malloc`/`free`/`printf`/`sqrt` cannot collide with the host C library. Make names
   unique, since 2,742 names repeat. Sanitise the one non-identifier. Keep the demangled name from
   `ghs_demangle.py` as a comment above each function.

### B. Relocations at the default load addresses

- **Load at the default addresses.** The game is linked for code at 0x02000020 and data at 0x10000000, and
  the recompiled code bakes in absolute addresses (`lis`/`addi` pairs). The host runtime must place guest
  memory so those addresses are valid. Relocating to anywhere else would mean regenerating code.
- **Apply and verify internal relocations.** Process `.rela.text`, `.rela.rodata`, `.rela.data` and
  `.rela.fexports` (`R_PPC_ADDR32`, `ADDR16_LO/HI/HA`, `REL24`; nothing else occurs). For relocations
  against internal symbols, the result already matches the file contents in every case (1,137,119
  `.text` relocations and all 355,621 data relocations). The loader should still apply them and assert
  they match, which validates the loader.
- **Weak undefined references.** The one `ADDR16_HA` against `$UNDEF` in `_start` resolves to 0.
- **Import relocations are different.** In the file, the 5,438 `REL24` calls and ~27,650 `ADDR16_HA`/`LO`
  pairs that target imports point just past the end of `.text` (0x04A6Fxxx–0x04A70xxx), not at the import symbols'
  addresses (0xC0A3xxxx).
  - Resolve each one to its import symbol.
  - Record `instruction address → import` in a side table.
  - Patch the `HA`/`LO` immediates so that any computed address equals the import's canonical address.
  - Do not trust the `bl` displacement for those 5,438 sites: a 24-bit branch cannot reach 0xC0A3xxxx, so
    the recompiler must take the target from the side table.

### C. 32-bit registers

- Make GPRs 32-bit: `PPCRegister` keeps `u32`/`s32` (plus 8/16-bit views) and drops `u64`/`s64`. Rewrite
  the ~125 emitter lines that use `.u64`/`.s64` to their 32-bit forms, e.g. `addi`, `add`, `subf`, `neg`,
  `mulli`, `extsb`/`extsh`, loads (`rD.u32 = PPC_LOAD_U32(...)`), and `mr`.
- Rewrite `rlwinm`/`rlwimi`/`rlwnm`/`rotlw` with a 32-bit rotate (`__builtin_rotateleft32`) and a 32-bit
  `ComputeMask`. Add `rlwnm`, which is missing.
- Delete the 34 64-bit-only emitters. Make the decoder reject them, because on Espresso those encodings
  are either invalid or something else (see D).
- `CTR`, `LR` and `XER` become 32-bit. Delete `ctx.msr`, `mfmsr`/`mtmsrd` and the `skip_msr` option.
- Add the missing integer and system instructions from the census: `lmw`/`stmw`, the update and
  update-indexed forms (`lhzu`, `lhau`, `sthu`, `lwzux`, `lbzux`, `lhzux`, `lhaux`, `stbux`, `sthux`,
  `lfsu`, `lfsux`, `stfsu`, `stfsux`, `lfdu`, `lfdux`, `stfdu`, `stfdux`), `addc`, `addme`, `subfze`,
  `subfme`, `eqv`, `frsqrte`, the CR logic ops (`crxor`/`crclr`, `creqv`/`crset`, and the rest of the family),
  `lswi`/`stswi`, conditional calls (`bnel`, `beql`), `isync`/`dcbst` (no-ops), and `tw`/`trap`.
- `mftb` should advance at the Wii U timebase rate, which is the bus clock divided by 4. Most timing goes
  through `OSGetTime`/`OSGetTick` imports, so this is low priority.

### D. Paired-single support

- **Decoder.** Build the binutils tables for Espresso (a Gekko/Broadway "750cl"-style option set) instead
  of `"cell 64"`, or swap in a small table-driven decoder of our own. It must decode opcode 4 as
  `ps_*`/`psq_*x`/`dcbz_l` and opcodes 56/57/60/61 as `psq_l`/`psq_lu`/`psq_st`/`psq_stu`.
  `src/tools/insn_census.py` (Capstone with `CS_MODE_PS`) decodes 100% of `.text` and is a reference for
  checking a new decoder.
- **Registers.** Each FPR becomes a pair `{ double ps0; double ps1; }`. Add the eight GQRs to `PPCContext`
  and implement `mtspr`/`mfspr` for SPR 912–919 and the user aliases 896–903 (the game writes UGQR2–7).
- **Scalar instructions write ps1 too.** With paired singles enabled (HID2[PSE], always the case for
  games), several scalar instructions also write ps1:
  - `lfs`/`lfsx`/`lfsu`/`lfsux` load the single into both ps0 and ps1.
  - Single-precision arithmetic (`fadds`, `fmuls`, `fmadds`, …) and `frsp` write the result to both
    halves.
  - `lfd` and double-precision arithmetic write ps0 only.

  Get this right before anything else, because the GHS prologues save ps1 with
  `stfd`; `ps_merge10 fN,fN,fN`; `stfs` and restore it the same way. Callee-saved f14–f31 are corrupted if
  ps1 is modelled loosely. Use Dolphin's interpreter (`Interpreter_Paired.cpp`,
  `Interpreter_FloatingPoint.cpp`, `Interpreter_LoadStorePaired.cpp`) as the reference for exact semantics,
  including rounding of single results.
- **Quantised loads/stores.** `psq_l`, `psq_st`, `psq_lu`, `psq_stu`, `psq_lx` and `psq_stx` read the
  GQR named by the instruction's `I` field and the `W` bit. They handle float, u8, s8, u16 and s16 with a
  2^scale factor, loading one or two values. Emit a direct inline path when GQR contents are known
  (GQR0 is 0, meaning float with no scaling, by OS convention) and a runtime helper otherwise.
- **Paired arithmetic.** The 21 `ps_*` arithmetic mnemonics in the census map naturally onto SSE2 `__m128d` (or two
  doubles). `ps_merge10 fN,fN,fN` on its own is a swap and can use a single shuffle.
- **No VMX.** Remove VMX registers, VMX emitters and the flush-mode state machine. The Espresso FPU runs
  with ordinary IEEE behaviour unless a game sets FPSCR[NI]; keep `mtfsf`/`mffs` rounding-mode support only.

### E. Imports routed to our own implementations

- **Naming.** Create one symbol per import at its `.fimport_<lib>`/`.dimport_<lib>` address, named
  `<lib>_<Name>` (e.g. `coreinit_OSBlockMove`, `gx2_GX2Init`). Names come straight from the symbol table, so
  the ordinal tables and thunk patching go away.
- **Direct calls.** Most imports are called through `bl`, which the relocation side table (B) turns into a
  direct C++ call. Emit `PPC_EXTERN_FUNC(coreinit_OSBlockMove)` like any other function. The host provides
  the implementation, with a generated stub that logs and traps for anything not yet written.
- **Indirect calls.** GHS also calls imports indirectly:
  `lis r12, imp@ha; addi r12, r12, imp@l; mtctr r12; bctrl`. `OSBlockMove` alone accounts for 23,846 of
  these sites. Two parts:
  1. Make `PPC_CALL_INDIRECT_FUNC` resolve import addresses (0xC0A30A00–0xC0A37FFF) to host
     implementations, alongside `.text` addresses.
  2. Add a peephole that turns `lis/addi → mtctr → bctrl` with a known import target into a direct call.
     This is needed for performance, since `bctrl` is the most common call form in the game (441,291 sites).
- **Data imports.** The 10 data imports (`MEMAllocFromDefaultHeap`, `MEMAllocFromDefaultHeapEx`,
  `MEMFreeToDefaultHeap`, `_iob`, `environ`, `__gh_FOPEN_MAX`, and the C++/stdio/atexit hooks) need guest
  memory at their `.dimport` addresses. The runtime initialises them. The three `MEM*` ones are function
  pointers, so store addresses that resolve to host allocator functions through the indirect-call path.
- **Library mapping.** coreinit (threads, memory, time, filesystem), gx2 (graphics), snd_core/snd_user
  (audio), vpad/padscore/nsyskbd (input), proc_ui, sysapp, nn_* (account, save, friends, networking),
  nsysnet/nlibcurl (sockets), zlib125 and dmae. Cemu's implementations (`src/Cafe/OS/libs/`) are the
  behavioural reference. Networking/account/friends libraries (`nn_ac`, `nn_act`, `nn_fp`, `nn_ndm`,
  `nlibcurl`) can start as offline stubs.

### F. Other changes

- **Switch tables.** GHS emits `cmplwi idx, N; bgt default; lis/addic → table; slwi; add; mtctr; bctr`
  followed by N+1 `b target` instructions directly after the `bctr`. Detect this from the `lis`/`addic`
  `ADDR16_HA/LO` relocation that points to `bctr+4` inside the same function. Emit
  `switch (idx) { case i: goto <target of entry i>; }`, and stop the entries being emitted as normal
  instructions. The remaining `bctr` sites are tail calls through function pointers or imports, which
  XenonRecomp's `PPC_CALL_INDIRECT_FUNC; return;` already handles. No data relocation points inside a
  function, so there are no jump tables in `.rodata`/`.data`. This replaces XenonAnalyse entirely.
- **GHS register save helpers.** `_savesmall_N`/`_restsmall_N` (using `stwu`/`stmw`/`lmw`) replace the
  MSVC `__savegprlr_N` family. Recompile them as ordinary functions first. `_savesmall_N` executes
  `mflr r31`, so keep `skip_lr = false` until every LR read is understood. Inlining them is an optimisation.
- **Indirect-call table.** Do not put the lookup table in guest memory after the image. On Wii U,
  0x10000000+ is data and heap. Use a host-side array indexed by `(addr - 0x02000000) / 4` for `.text`
  (about 11.1M entries; 89 MB of pointers, or 44 MB of 32-bit indices into a function array). The import
  range is a separate small table. Vtables account for 307,794 `.rodata` relocations to function starts, all
  inside `.text`.
- **Config schema.** Drop `patch_file_path`, `patched_file_path`, `switch_table_file_path`,
  `restgprlr_*`/`savegprlr_*`/`*fpr*`/`*vmx*` addresses, `skip_msr`, `functions` (comes from the symbol
  table) and `invalid_instructions`. Keep `mid_asm_hook`, `skip_lr`, `*_as_local` (re-check the volatile set
  against the GHS EABI: r0, r3–r12 volatile; r14–r31 and f14–f31, both halves, non-volatile) and
  `longjmp_address`. `setjmp` is an undefined weak symbol here, so find the actual setjmp entry before
  relying on that option.
- **Output hygiene.** Generated files contain translated game code. Write them under `generated/`, which
  `.gitignore` and the local pre-commit hook keep out of git.

## Suggested order of work

1. **Loader.** Fork XenonRecomp into this repo's source tree as our own tool. Implement the RPX loader
   (A, B) and print the function list, import side table and relocation self-check.
2. **Decoder.** Add the Espresso decoder (D, decoding only). Diff its output against
   `insn_census.py` over the whole `.text`.
3. **First emitted code.** Move to the 32-bit register model and remove VMX (C, D registers). Make the
   recompiler emit every function, with unsupported instructions emitted as traps, then drive the trap
   count to zero using the census order (`lmw`/`stmw` and the update forms first).
4. **Paired singles.** Implement the semantics, including the scalar ps1 rules, with unit tests against
   Dolphin's behaviour.
5. **Calls.** Add the import routing, the indirect-call table and the switch-table detection (E, F).
6. **Runtime.** Bring up a minimal host runtime: memory layout at the default addresses, coreinit
   threads/memory/time, then GX2.

## Open questions

- Whether any code reads LR as a value beyond `_savesmall_N` (decides `skip_lr`).
- What the 568 undecodable bytes at 0x047E5258 are (likely data between the GHS runtime helpers).
- Whether the game sets FPSCR[NI] or changes rounding modes at run time.
- How the game uses `OSBlockMove` (likely UE3's `appMemmove`); its call volume makes it a hot path.
- C++ exceptions: `__cpp_exception_init_ptr`/`__cpp_exception_cleanup_ptr` are imported, so check whether
  any code actually throws.
