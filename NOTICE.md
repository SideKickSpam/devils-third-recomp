# Notices, credits and disclaimer

## Disclaimer

dt-port is an independent, non-commercial fan project for game preservation. It is **not affiliated with, endorsed
by or sponsored by Nintendo or Valhalla Game Studios**, or any other rights holder. *Devil's Third* is a trademark of
its owners; Wii U is a trademark of Nintendo.

**No game files are included** in this repository or in any release: no game code, no translated game code, no
assets, textures, sounds, movies or text. dt-port only runs with a copy of the game that **you provide yourself**,
which you must have obtained legally (for example, dumped from a disc or console you own). Everything derived from
the game — the recompiled program, shader caches, saves — is generated on your own computer from your own copy and
never leaves it.

## dt-port's own code

MIT License, see [LICENSE](LICENSE).

## Third-party components

| Component | Used for | License |
|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev and contributors) | the basis of the recompiler (`src/recomp`), adapted to Wii U executables | MIT |
| [addrlib](https://github.com/decaf-emu/addrlib) (Advanced Micro Devices, Inc.; decaf-emu's R600/R700 fork) | GPU surface layouts and tiling | MIT-style (AMD) |
| [SDL 3](https://github.com/libsdl-org/SDL) | window, input, controllers, audio | zlib |
| [zlib](https://github.com/madler/zlib) (Jean-loup Gailly and Mark Adler) | decompression | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut) | the overlay: settings page, weapon menu, subtitles | MIT |
| [Roboto](https://fonts.google.com/specimen/Roboto) (Google; shipped with Dear ImGui) | fallback font for the overlay | Apache 2.0 |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (The Khronos Group) | Vulkan API | Apache 2.0 / MIT |
| [volk](https://github.com/zeux/volk) (Arseny Kapoulkine) | Vulkan loader | MIT |

Each component's full license text ships with the release in the `licenses/` folder.

The AMD R700-family ISA opcode names in `src/runtime/gpu/shader/r700_opcodes.inc` come from AMD's public R700 ISA
documentation.

## References and thanks

The behaviour of the Wii U's system libraries was studied with the help of these projects; no code was copied from
them:

- [Cemu](https://github.com/cemu-project/Cemu) (MPL 2.0), the Wii U emulator.
- [decaf-emu](https://github.com/decaf-emu/decaf-emu) (GPL), the Wii U emulator (only its MIT-licensed addrlib fork is
  used, as a dependency).
- [Dolphin](https://github.com/dolphin-emu/dolphin)'s interpreter, for Gekko/Espresso paired-single and FPU behaviour.
