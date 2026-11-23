# Building dt-port

This is for developers. Players should use a release and its setup tool (see [README.md](README.md)).

dt-port has two halves:

- **The recompiler** (`src/recomp`, `dt_recomp`) reads the game's executable and writes C++, one function per guest
  function. It is adapted from [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for Wii U RPX files and the
  Espresso CPU.
- **The runtime** (`src/runtime`, `dt_port`) is everything the generated code runs on:
  - the guest CPU model (`ppc_context.h`);
  - our implementations of the Wii U system libraries (`cafe/`);
  - GX2 graphics translated to Vulkan (`gpu/`);
  - sound, input and the window (`host/`).

You need your own copy of the game to build anything that runs, because the generated code is made from it.

## The rule: no game code or data in git

Never commit anything from the game, or anything generated from it. That includes:

- recompiler output (`generated/`);
- disassembly or decompiler exports and Ghidra projects;
- the symbol table dump (`src/notes/symbols.csv`), extracted packages, movies and audio;
- screenshots and shader caches.

`.gitignore` is an allowlist and blocks these paths. Recreate this pre-commit hook in a fresh clone; it rejects them
even with `git add -f`:

```sh
cat > .git/hooks/pre-commit <<'EOF'
#!/bin/sh
# Refuse to commit game data or recompiled game code, even if .gitignore is bypassed with `git add -f`.
bad=$(git diff --cached --name-only --diff-filter=ACMR | grep -E \
  '^(game|tools|ref)/|\.(wua|wud|wux|rpx|rpl|elf|xex|gpr|xxx|upk|tfc|pck|bnk|gfx|swf)$|(^|/)extracted/|(^|/)generated/|(^|/)ppc_recomp\.[^/]*\.cpp$|(^|/)ppc_func_mapping\.|(^|/)ppc_config\.h$|^src/notes/symbols\.csv$')
if [ -n "$bad" ]; then
  echo "pre-commit: refusing to commit game data or generated game code:" >&2
  echo "$bad" | sed 's/^/  /' >&2
  exit 1
fi
EOF
chmod +x .git/hooks/pre-commit
```

Short excerpts in notes (a few addresses, symbol names or instructions, for analysis) are fine.

## The game files

Put the extracted titles in `game/` (or anywhere, see `--game-dir`):

```
game/0005000010177600_v16/   base title: code/, content/, meta/
game/0005000e10177600_v16/   update (version 16): code/, meta/
```

The recompiler and the runtime both read the RPX as it ships (its sections are zlib-compressed), from the update's
`code/` folder. A decompressed copy (`game/dt.elf`) also works, and the Python analysis tools expect one.

The program must be version 16. Its SHA-256 is `0659e26fb0bb8f1d444c401d1fd1dce4a3de568c38e56d54c97b1e3e410cbe75`.

## Linux

Requirements:
- clang (18 or newer), CMake 3.24 or newer, and Ninja;
- the development headers of the system parts you want SDL3 to use: X11 or Wayland, and PulseAudio, PipeWire or ALSA
  for sound.

The first configure downloads SDL3, zlib, Dear ImGui, addrlib, Vulkan-Headers, volk, zstd and ZArchive, all pinned by
hash.

```sh
cmake -S src/recomp -B build/recomp -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/recomp
build/recomp/dt_recomp game/0005000e10177600_v16/code/DevilsThirdGame-WiiU-Shipping.rpx generated  # ~10 s
cmake -S src/runtime -B build/game -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build/game -j 12                          # the generated code is large: lower -j if RAM runs out
build/game/dt_port --game-dir game
```

## Windows

Requirements: Visual Studio 2022 or its Build Tools, with the "Desktop development with C++" workload and the "C++
Clang tools for Windows" component (clang-cl and lld-link).

```bat
src\tools\build-windows.cmd [game-dir]
build\game\dt_port.exe --game-dir <game-dir>
```

The script finds Visual Studio itself and does four things:

1. builds `dt_recomp`;
2. regenerates `generated\` (the output is byte-identical to Linux);
3. builds `dt_port.exe` with the generated code linked in;
4. on the first configure, fetches the dependencies.

`src\tools\dt-port.cmd` starts the game with a window. It finds the game through `DT_GAME_DIR`, then the first line of
`build\game-dir.txt`, then `game\`.

### The release build (game module)

Releases do not contain the generated code. With `-DDT_GAME_MODULE=ON`, `dt_port.exe` loads it from `dt_game.dll`,
which the setup tool builds on the player's PC with a bundled clang-cl and lld-link.

- **The module is freestanding:** no C or C++ library, no system headers, and it imports nothing.
- **The runtime hands it its functions and data** through `DtModuleHost` (see `ppc_context.h`).
- **Both carry an ABI stamp** (`module_abi.h`), so after an update the runtime starts the setup tool to rebuild the
  module.

```bat
rem configure without recompiler output, as CI does: builds dt_port.exe, DevilsThirdSetup.exe and sdk\
cmake -S src\runtime -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl ^
      -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DDT_GAME_MODULE=ON -DDT_GENERATED_DIR=%CD%\build\nogen
cmake --build build\release
cmake -DBUILD=%CD%\build\release -DRECOMP=%CD%\build\recomp -DLLVM=<LLVM folder> -DOUT=%CD%\build\dist ^
      -DSOURCE=%CD% -P src\tools\package_release.cmake
```

`.github/workflows/release.yml` does the same with LLVM 19.1.7. For a `v*` tag, it attaches the zip to a draft
release.

The setup tool has a command line for testing:
- `--check PATH` checks a dump;
- `--extract PATH DEST` extracts it;
- `--install PATH` checks the dump and builds the module, without the window.

## Running and debugging

`dt_port --help` lists every option. The most useful:

| Option | What it does |
|---|---|
| `--log FILE`, `--no-import-log`, `--quiet NAME` | the call log (every import call, with its arguments) |
| `--frames N`, `--hidden`, `--no-window` | unattended runs |
| `--press F:BUTTONS[:N]` | scripted GamePad input, e.g. `--press 1000:plus --press 1600:a` |
| `--save-dir DIR`, `--config FILE` | keep scripted runs away from your own save and settings |
| `--gpu-stats`, `--frame-csv FILE` | renderer counters, per-frame timings (`src/tools/frame_report.py`) |
| `--capture-frame N` | every render pass of frame N as PNGs, with a draw log |
| `--validate` | the Vulkan validation layer (needs the Vulkan SDK) |
| `--sample FILE` | a sampling profiler (Windows; `src/tools/sample_report.py`) |

Hang and crash reports print each thread's native stack, and the recompiled functions on it are the game's own call
chain. `src/tools/symbolize.py LOG --map build/game/dt_port.map` names them.

Measure 3D performance only on real GPUs. WSL's Vulkan is a software renderer.

## Conventions

- Generated functions are `sub_XXXXXXXX`, with the game's symbol name in a comment. Imports are `imp_<lib>_<name>`.
  Function boundaries come from the RPX's symbol table.
- **Hooks.** `src/recomp/hooks.txt` lists guest functions the runtime may replace. The runtime registers each with
  `DT_HOOK(addr, fn, name)` and calls the original with `hooks::Original(addr)`. Changing the list means running
  `dt_recomp` again.
- **Imports.** Implement them with `CAFE_IMPORT(lib, "Name", fn)`. Arguments are unpacked and logged automatically.
  Implement them in the order the game first calls them, with offline behaviour for network services.
- **References.** Cemu (MPL 2.0) and decaf-emu (GPL) are for behaviour only; don't copy their code. For paired-single
  and FPU semantics, see Dolphin's interpreter.
- Treat the game's files as untrusted input. Run Python tools that read them with `python3 -I`.

## Tools

`src/tools/` has analysis scripts that use only the Python standard library, among them:

- `rpx_report.py` and `ghs_demangle.py` (a Green Hills C++ demangler);
- `import_users.py`;
- `ue3_package.py` (the game's packages) and `gfx_movies.py` (its Scaleform menus);
- `frame_report.py` and `sample_report.py`.

They default to `game/dt.elf`, and anything they extract goes to `build/`, which is never committed.
