# GX2 plan

How Devil's Third uses GX2, the Wii U graphics library, and how we should render it on Vulkan.

Sources:
- `src/tools/import_users.py --lib gx2`, which lists the game functions that reference each gx2 import,
  taken from the RPX relocations.
- The symbol table.
- `CookedWiiU/` in the base title.
- The headless runs of `build/game/dt_port`, where every GX2 call is logged with its arguments.

## Summary

- **One caller.** All 95 gx2 imports are reached through UE3's Wii U RHI (`RHI*`, `FGX2*` classes), plus the
  Bink movie renderer, Scaleform's mesh cache and a statically linked copy utility (`GX2UT*`). That is 84
  game functions in total.
- **No display lists.** The game imports none of `GX2BeginDisplayList`, `GX2EndDisplayList`,
  `GX2CallDisplayList`, `GX2DirectCallDisplayList` or `GX2GetCurrentDisplayList`.
- **No hand-written PM4.** No game code writes PM4 command packets. The only `0xC0nn____` constants are in
  unrelated code (nn::Result values, Nex lists, float constants). The game never sees the command buffer.
- **Shaders.** They are precompiled Latte (R7xx-family) GPU programs, from UE3's global shader map and the
  per-material shader maps in cooked packages. The game wraps the binaries in `GX2VertexShader`/
  `GX2PixelShader` objects. Fetch shaders are generated at run time from attribute descriptions
  (`GX2InitFetchShaderEx`).
- **Textures.** They ship already tiled for the GPU, in about 2.9 GB of texture caches. They are streamed
  into the game's own texture pool, which the game defragments with DMA copies (`dmae`) ordered by
  GX2 semaphores.

**Recommendation:** reimplement GX2 at the API level on Vulkan rather than emulating PM4 command
processing. The reasoning is under "Recommendation" below.

## The 95 imports, by purpose

Call-site counts are the number of relocations referencing each import. "GX2UT only" marks functions that
only the statically linked copy utility (`GX2UTSetCopyState` / `GX2UTCopySurfaceRect`) calls. It uses them
to reset state to defaults, so the game's own rendering never uses those features.

| Group | Imports |
|---|---|
| **Display and scan-out** (18) | `GX2Init`, `GX2SetupContextStateEx`, `GX2SetContextState` (17 sites), `GX2CalcTVSize`, `GX2SetTVBuffer`, `GX2SetTVScale`, `GX2SetTVEnable`, `GX2CalcDRCSize`, `GX2SetDRCBuffer`, `GX2SetDRCScale`, `GX2SetDRCEnable`, `GX2GetTVGamma`, `GX2SetTVGamma`, `GX2GetDRCGamma`, `GX2SetDRCGamma`, `GX2SetSwapInterval`, `GX2CopyColorBufferToScanBuffer`, `GX2SwapScanBuffers` |
| **Submission and synchronisation** (7) | `GX2Flush` (9), `GX2DrawDone`, `GX2GetLastSubmittedTimeStamp`, `GX2GetRetiredTimeStamp`, `GX2WaitTimeStamp`, `GX2Invalidate` (26, the most referenced), `GX2SetSemaphore` (texture-pool defragmentation only) |
| **Surfaces and render targets** (13) | `GX2CalcSurfaceSizeAndAlignment`, `GX2GetSurfaceFormatBits` (GX2UT only), `GX2InitColorBufferRegs`, `GX2InitDepthBufferRegs`, `GX2CalcDepthBufferHiZInfo`, `GX2InitDepthBufferHiZEnable`, `GX2InitTextureRegs`, `GX2SetColorBuffer`, `GX2SetDepthBuffer`, `GX2AllocateTilingApertureEx`, `GX2FreeTilingAperture`, `GX2CopySurface`, `GX2ConvertDepthBufferToTextureSurface` |
| **Clears, viewport, scissor** (4) | `GX2ClearColor`, `GX2ClearDepthStencilEx`, `GX2SetViewport`, `GX2SetScissor` |
| **Shaders and uniforms** (8) | `GX2CalcFetchShaderSizeEx`, `GX2InitFetchShaderEx`, `GX2SetFetchShader`, `GX2SetVertexShader`, `GX2SetPixelShader`, `GX2SetShaderModeEx` (GX2UT only), `GX2SetVertexUniformReg`, `GX2SetPixelUniformReg` |
| **Textures and samplers** (8) | `GX2SetPixelTexture`, `GX2SetVertexTexture`, `GX2SetPixelSampler`, `GX2InitSampler`, `GX2InitSamplerClamping`, `GX2InitSamplerLOD`, `GX2InitSamplerXYFilter`, `GX2InitSamplerZMFilter` |
| **Vertex input and draws** (5) | `GX2SetAttribBuffer`, `GX2DrawEx`, `GX2DrawIndexedEx`, `GX2SetPrimitiveRestartIndex` (GX2UT only), `GX2SetStreamOutEnable` (GX2UT only) |
| **Fixed-function state** (19) | `GX2SetBlendControl`, `GX2SetColorControl`, `GX2SetTargetChannelMasks`, `GX2SetAlphaTest`, `GX2SetDepthStencilControl`, `GX2SetStencilMask`, `GX2SetPolygonControl`, `GX2SetPolygonOffset`, `GX2SetRasterizerClipControlEx`, `GX2SetLineWidth`. GX2UT only: `GX2SetAlphaToMask`, `GX2SetBlendConstantColor`, `GX2SetDepthOnlyControl`, `GX2SetRasterizerClipControl`, `GX2SetPointSize`, `GX2SetPointLimits`, `GX2SetTessellation`, `GX2SetMaxTessellationLevel`, `GX2SetMinTessellationLevel` |
| **Occlusion queries** (3) | `GX2QueryBegin`, `GX2QueryEnd`, `GX2QueryGetOcclusionResult` |
| **Debugging and performance counters** (10) | `GX2DebugTagUserString`, `GX2DebugTagUserStringVA` (GX2UT only), `GX2InitPerfMetric`, `GX2GetPerfMetricF32`, `GX2GetPerfMetricU64`, `_GX2ResetCounters`, `_GX2SampleCounters`, `_GX2SetCounterInfo`, `_GX2StartCounters`, `_GX2StopCounters` (all from `FGX2Manager::StartPerf`/`EndPerf`) |

Tessellation, stream-out, point sprites, primitive restart and alpha-to-coverage are therefore out of scope
for this game.

## Who calls GX2

Everything goes through a small, disciplined layer:

- **UE3 RHI.** For example, `RHISetBoundShaderState` (fetch, vertex and pixel shader plus uniform registers),
  `RHISetRenderTarget`, `RHISetBlendState`, `RHISetSamplerState`, `RHIDrawIndexedPrimitive` /
  `RHIDrawPrimitiveUP` (`GX2SetAttribBuffer` + `GX2DrawEx`/`GX2DrawIndexedEx`), `RHIClear` and `RHICopyTexture2D`.
  Fences are `RHISetFence`/`RHIWaitFence` (timestamps) and command kicks are `RHIKickCommandBuffer`
  (`GX2Flush`).
- **`FGX2Manager`.** It owns TV/DRC scan buffers and gamma, copies to scan buffers and runs the perf counters.
  `FGX2Viewport::SwapBuffers` calls `GX2SwapScanBuffers`.
- **Resources.** `FGX2Surface`, `FGX2Texture`, `FGX2Texture2DResourceMem` and `FGX2Buffer` compute layouts with
  `GX2CalcSurfaceSizeAndAlignment` and set up registers with `GX2Init*Regs`. They mark CPU writes with
  `GX2Invalidate`. CPU readbacks (`RHIReadSurfaceData`) use tiling apertures.
- **`FGX2TexturePool`.** It allocates streamed textures and defragments with `DMAECopyMem`/`DMAESemaphore` plus
  `GX2SetSemaphore`.
- **Others.**
  - Bink (`Draw_Bink_textures`, `Wait_for_Bink_textures`) decodes video into textures on the CPU and fences
    with timestamps.
  - Scaleform renders through the same RHI.
  - `GX2UTCopySurfaceRect` is the statically linked copy utility.

What a frame looks like in the headless runs:
- **During the startup movies** (frames 1–3000): one draw per frame (the movie quad), plus about 7 viewport
  and scissor changes, 6 render-target binds, 5 `GX2Invalidate`, 4 context-state switches, a few
  uniform/texture/sampler binds, 3 `GX2Flush` and 2 timestamp waits.
- **Swap interval.** The game switches between 1 and 2 (60 and 30 Hz).

## Shaders

- **Where they come from.**
  - `CookedWiiU/GlobalShaderCache-WiiU.bin` is an 8.7 MB UE3 global shader map. It starts with magic `GSMB`
    and has named entries such as `FUICaptureVertexShader`.
  - Material shaders live in the shader maps inside the cooked packages (`*.xxx`).
- **How they're built.** `FGX2VertexShader` and `FGX2PixelShader` are constructed from a byte array
  (`__ct__16FGX2VertexShaderFRC36TArray…`). They build the `GX2VertexShader`/`GX2PixelShader` structures,
  which hold register values plus a pointer to the Latte program, and call `GX2Invalidate` on the program
  memory.
- **Constants.** They are passed as uniform *registers* (`GX2SetVertexUniformReg`/`GX2SetPixelUniformReg`),
  not uniform blocks.
- **Vertex input.** `FGX2VertexDeclaration` builds a fetch shader at run time with `GX2InitFetchShaderEx`
  from a `GX2AttribStream` array (format, buffer, offset, endian swap).

Either rendering approach has to translate Latte shader microcode to SPIR-V. That is the bulk of the work.
It's a well-understood ISA: Cemu's `LatteDecompiler` and decaf's SPIR-V translator are the behavioural
references, but MPL 2.0 and GPL respectively, so read them, don't copy them. Translations should be cached
on disk by program hash. The headless runtime can enumerate every shader the game loads for offline testing.

At the API level, the fetch shader doesn't need translating: `GX2InitFetchShaderEx` gives us the
attribute layout directly, which maps onto Vulkan vertex input state, or a vertex-pulling prologue for
formats Vulkan lacks.

## Textures and surfaces

- **Already tiled.** Textures are stored in the Latte GPU tiling layout. The game has no swizzle code,
  and texture data comes straight from `Textures.tfc` (1.4 GB), `Lighting.tfc` (1.1 GB) and
  `ShadowTextures.tfc` (0.4 GB) into pool memory.
  - The renderer must de-tile on upload, keyed on `GX2Invalidate(TEXTURE)` and surface descriptors.
  - That needs an exact implementation of Latte's address library: tile modes, pitch/height alignment,
    bank/pipe swizzle and mip layout.
  - The headless `GX2CalcSurfaceSizeAndAlignment` only approximates it. That is safe while nothing is
    sampled, but the real values must match what the cooker assumed.
- **Formats.** Not yet catalogued. The first rendering step should log each surface's
  `GX2SurfaceFormat` as it is created.
  - BCn formats map to Vulkan directly.
  - Big-endian component order and the GPU's sRGB/format quirks need per-format handling.
- **Render targets and copies.**
  - Render targets are `GX2ColorBuffer`/`GX2DepthBuffer` over surfaces.
  - `GX2CopySurface`, `GX2ConvertDepthBufferToTextureSurface` and `GX2CopyColorBufferToScanBuffer` become
    Vulkan copies or blits.
  - Tiling apertures (`RHIReadSurfaceData`) need a linear CPU copy of a GPU image.
- **Texture pool defragmentation.** It moves texture memory with DMA while the GPU may read it. That is
  ordered by `DMAESemaphore`/`GX2SetSemaphore`, and our cache of host textures must follow the moves.

## Recommendation: API-level GX2 on Vulkan

**Reimplement the 95 GX2 functions on Vulkan directly.** Don't generate and then interpret PM4 command
streams as Cemu does.

Why the API level fits this game:

1. **Nothing below the API is ever observed.** No display lists, no PM4, and no game code touches the
   command buffer. The game only sees GX2 structures, which it builds with GX2's own helpers, plus
   timestamps.
2. **The surface is small and known.** There are 95 functions, every one is used, and 14 matter only as
   resets inside a copy utility. The game's real state usage is a modest subset: no tessellation,
   stream-out or point sprites.
3. **The API level gives better information.**
   - Fetch shaders become explicit vertex layouts instead of programs to execute.
   - Draws arrive as calls with their state, not as register writes to track.
   - Textures and render targets arrive as typed descriptors, not raw register values.
4. **It's less work and faster.** A PM4 path means reimplementing what GX2 writes into the ring (register
   packing for every state call) and then decoding it again. That's useful for an emulator that must run
   any game, but pure overhead for one game we compile ahead of time.
5. **It suits static recompilation.** The renderer runs in-process on the calling thread. State calls can
   go straight into a Vulkan command recorder, and `GX2Flush`/timestamps map onto queue submissions and
   timeline semaphores.

What we still have to build either way, roughly in order:

1. **Shaders.** Latte shader binary → SPIR-V translator, plus an on-disk cache. This is the biggest
   single piece.
2. **Surfaces.** Exact Latte surface layout (AddrLib) for sizes and alignments, and de-tiling on texture
   upload.
3. **The Vulkan backend.**
   - Render targets, pipelines built from GX2 state (blend, depth/stencil, rasterizer, vertex layout).
   - Descriptor sets for textures and samplers.
   - Uniform registers pushed as uniform buffers.
4. **Synchronisation and GPU-visible memory.**
   - Timestamps/fences as Vulkan timeline semaphores.
   - Tracking CPU writes (`GX2Invalidate`) into GPU-visible guest memory, including texture-pool moves.
5. **Presentation.** TV scan buffer to the SDL3 window's swapchain, honouring the swap interval. The GamePad
   (DRC) image can be shown in a second window or as an inset later.

The main risk of the API level is GX2 functions whose exact side effects the game relies on. The likely ones
are the register words that `GX2Init*Regs` writes into structures and `GX2CalcSurfaceSizeAndAlignment`'s
outputs. Both are computed by our code, so they can be made exact where the game reads them. The headless
log already records every call with arguments, which gives a test corpus for each function.
