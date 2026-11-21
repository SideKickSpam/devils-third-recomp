# Milestone 6: a full PC version

Goal: perfectly smooth play on a 240 Hz G-Sync monitor: no hitches and steady frame times, not maximum fps. Then the
features of a PC version: an in-game settings menu, rebindable controls with matching button prompts, any internal
resolution and, last, an unlimited frame rate.

Rules for every stage:
- Commit and push after each working stage, and report it so it can be tested by hand.
- Test on Windows (performance, `--validate` clean through boot and the corridor walk) and Linux (build, menus,
  `--selftest`, a short headless run).
- Settings live in one config file (below); command-line options override it for a run without changing it.

## Stage 1 (done): presentation, VRR and fullscreen

Where we are: `Device::CreateSwapchain` prefers MAILBOX, then IMMEDIATE. The window is resizable, but the scan buffer
is stretched over the whole window, and nothing reacts to a resize except an out-of-date swapchain.
`GX2SwapScanBuffers` paces swaps on a 59.94 Hz grid times the swap interval.

- **Present mode** `vsync = fifo | mailbox | immediate`, default FIFO. FIFO is the mode VRR is designed around: with
  G-Sync each present starts a refresh, so the display follows the game's frame rate. Mailbox is kept for fixed-refresh
  monitors and immediate for testing.
- **Window modes** `display = windowed | borderless | exclusive`.
  - Borderless covers the monitor with a desktop-mode fullscreen window, so on Windows the swapchain gets
    independent flip.
  - Exclusive switches the display mode (resolution and highest refresh) through SDL. On Windows it also allows
    `VK_EXT_full_screen_exclusive`.
  - Alt+Enter and F11 switch between windowed and the configured fullscreen mode.
- **Resizable window** whose size is remembered. The swapchain is recreated on every pixel-size change. The image is
  letterboxed to its aspect ratio (16:9) instead of stretched.
- **Config file** `settings.ini`:
  - Windows: `%APPDATA%\dt-port\settings.ini`; Linux: `~/.local/share/dt-port/settings.ini`.
  - `--config FILE` uses another file.
  - Plain `section.key = value` lines, so the launcher and people can read and edit it.
- **Check:** NVIDIA's G-SYNC indicator in windowed, borderless and exclusive modes. Fixed monitors: no tearing with
  FIFO or mailbox.

## Stage 2: stutter
Fix the biggest causes first, measured, not guessed.

- **Frame-time CSV log** (`--frame-csv FILE`): one row per frame with its time and where the time went. Columns: frame,
  ms since the previous present, then game and render thread code/wait/fs/swap/gx2/other, compile ms, pipelines and
  shaders created, draws, new surfaces, KB uploaded and present steps. It is the basis for every measurement in this
  stage: frame-time percentiles, how many frames miss the 20 ms budget, and why.
- **Slow-frame causes**, from the CSV over several corridor runs: how many frames over budget come from compiles,
  game code, file reads, uploads or presentation.
- **Precompile cached pipelines at startup:**
  - Store each pipeline's key and creation parameters next to `pipelines.bin`.
  - Build them all on worker threads during boot. The driver cache makes this fast, and no draw then creates a
    pipeline it has seen before.
  - New shaders still translate on first sight. Making that asynchronous (skip the draw for a frame) is a later option.
- **Game-code spikes:** sample them with a real profiler on the Windows build: Intel VTune (hotspots, with
  `dt_port.map` / PDB symbols), or Windows Performance Recorder and WPA. Map host addresses back to `sub_X` names.
- **The ~9 ms file read.** Some reads of 0 KB take 9-10 ms on the local drive too, so this is a fixed delay, not the
  disk. Possible sources:
  - our FS worker or the AppIO thread waking on a timer;
  - an `OSSleepTicks` / `OSWaitEvent` timeout in the game's own async I/O loop (`AsyncIOSystem`);
  - Wwise's streaming manager polling.

  Measure the time from issue to completion, worker to AppIO thread, and AppIO thread to the callback.
- **Per-instance vertex attributes** (format 0x813 in level 2): use instance input rate with the fetch shader's
  divisor (`VK_EXT_vertex_attribute_divisor` / Vulkan 1.4), or fetch by `InstanceIndex` in the shader.

## Stage 3: 60 fps cap
A 60 fps option, only if quick testing shows the game stays correct at 60:

- **Find what caps the game at 50.** Candidates:
  - UE3's `UEngine::GetMaxTickRate` / `appUpdateTimeAndHandleMaxTickRate`, with `MaxSmoothedFrameRate` and
    `bSmoothFrameRate` in the engine ini;
  - the GX2 swap interval;
  - our own 59.94 Hz swap pacing.
- **Quick tests at 60 against 50:**
  - movement and run speed (distance over a scripted walk);
  - physics;
  - cutscene and Bink movie speed;
  - Wwise audio sync.
- **If anything breaks at 60,** the game stays at 50 for now, and the plan notes what broke for the last stage.

## Stage 4: settings in the game's own menus (option B)

PC settings live in the game's menus, not in a mod-style overlay. The game's Options and pause menus are Scaleform GFx
movies (ActionScript 2, CLIK components) driven by UnrealScript (`D3UIWidget_OptionMenu` with one class per page,
`D3UIWidget_PauseMenu`); see `src/tools/gfx_movies.py`. A "PC Settings" entry is added to those menus at runtime and
opens our own page, restyled to match the game. Nothing on disk is modified and no game files are shipped: every patch
is made in memory when the game loads the original from the player's files.

**Fallback (done).** The Dear ImGui overlay (`gpu/overlay.*`, `host/menu.*`): F1, Escape to close, the controller's
Guide button, or Back + right-stick click. It stays until the integrated version is done, and afterwards as a way in
from anywhere.

**Settings that overlap are the game's.** We do not duplicate them; our page links to the game's Options instead.

| The game's own option (Options page) | What we do |
|---|---|
| Camera sensitivity, horizontal and vertical (Camera) | Game's. Mouse input becomes right-stick input, so it applies to the mouse too. |
| Invert horizontal / vertical (Camera) | Game's. Our invert-Y is removed. |
| Brightness, gamma, contrast (Video) | Game's. |
| Screen margins (Video) | Game's (TV overscan); should be 0 on PC. |
| Voice / effects / music volume (Audio) | Game's. Our master volume is removed. |
| Vibration (Game) | Game's; VPAD rumble is passed to SDL so it reaches PC controllers. |
| Controller layout presets (Game) | Game's; our rebinding (stage 5) works underneath. |
| Difficulty, aim assist, cover assist, violence, subtitles, languages | Game's. |

PC-only settings (ours): window mode, vsync, frame-rate cap, frame-time readout, internal resolution (stage 7), mouse
speed (on top of the game's sensitivity), stick dead zone, rebinding (stage 5), mute in background.

**Steps, each committed when it works:**
1. **Function hooks in the recompiler.** `dt_recomp` reads a list of guest functions (`src/recomp/hooks.txt`: address or
   symbol name). Each listed function is emitted as `orig_sub_X`, and `sub_X` calls the runtime's hook, which can call
   the original. Unhooked builds are unchanged. Also needed for stage 8.
2. **Restyled settings page.** Our page drawn in the game's style:
   - fonts read from the player's `UI_Fonts.xxx` at startup (the SWF `DefineFont3` outlines of Eurostile Next and
     NewRodin, rasterized into ImGui's atlas);
   - colours and layout taken from the options movie: dark panel, page list, row highlight, slider and arrow-stepper
     look;
   - the game's menu sounds: its movies play them through `UWwiseAudio::Static_PlayUISound` (`PLAY_UI_CURSOR`,
     `PLAY_UI_OK`, `PLAY_UI_CANCEL`), not `ExternalInterface`; our page queues them and a hook on `UGameEngine::Tick`
     plays them on the Game Thread (done);
   - controller navigation with the game's button roles.
3. **Movie patcher and loader hook (done).** `assets/swf_patch.*` patches the movie bytes in memory in the
   `FGFxFile` constructor hook (the patched copy stays allocated; FGFxFile keeps the pointer). It deep-copies a menu
   button of the game (the sprites holding its state labels get new edit texts reading "PC Settings", without the
   Label class or init callbacks, so UnrealScript never sees or renames them), registers the copy with the source's
   own class (its `registerClass` init action, re-targeted), and places it with the source's clip actions, its
   `enableInitCallback` turned off, plus a load handler: `addEventListener("click")` -> `ExternalInterface.call(
   "DtPortSettings")` and `"focusIn"` -> `"DtPortFocus"` (the cursor sound). Pause menu (`PauseMenu_new`,
   `posemenu_sidebar`, single-player frame): between Options and Back to Title, which moves down a row. Title menu
   (`TitleMenu`): a fourth tile after Options, inside Options' wrapper sprite so it follows the intro animation.
   The game's own focus handling reaches both (D-pad, stick, mouse). The options movie's tabs are UnrealScript's
   (a seventh tab would not be reachable with ZL/ZR), so the title entry is a title tile instead.
   Original plan for this step: Hook GFx's file loading (`FGFxFileOpener::OpenFile` / `FGFxFile`): when the game
   loads `PauseMenu_new` or `optionmenu` (and the title menu's options), patch the original bytes in memory. The patch
   clones one of the menu's own buttons (`options_mc` in the pause menu; a page tab in the options menu) as
   "PC Settings", whose press calls `ExternalInterface.call("DtPortSettings")`. Hook
   `FGFxExternalInterface::Callback` to catch that call before UnrealScript does.
   **Pausing (done):** F1 and the Guide button open the page through the game's own pause. In a level they send a
   Plus press and the page opens when the game's pause menu (`UI_InGame.PauseMenu_new`) starts; single-player pauses
   (`UWorld::IsPaused` follows the pause menu), multiplayer keeps running as the game decides. Closing the page then
   sends B to close that pause menu. Over the pause menu and in the front end the page opens at once; during Bink
   movies (`BinkDecodeFrame` in the last 10 frames), loading or any other game menu or prompt (an open movie that is
   not part of the HUD) it does not open. Not yet seen: in-engine cutscenes (Plus may skip them; none found so far).
   **The game's own options on the PC page (done):** every option of the game's Options menu is on our tabs, through
   `D3SystemSettings` and the option pages' own apply calls (see BUILDING.md, `game_settings.*`, `ue3.*`); the game's
   Options entries open our page (a `ProcessEvent` hook on their click handlers), with a row to the original menu.
   Checked: a volume changed on our page shows in the game's own Audio page; brightness previews live; a language change
   reloads to the title in the new language; the title's and the pause menu's Options both open the page.
4. **Input handoff.** When "PC Settings" is chosen, the game's menu stays open but receives no input; our page takes
   keyboard, mouse and controller; closing it returns focus to the same entry in the game's menu.

## Stage 5: customizable controls (brought forward: the Controls tab)

**Done (`host/bindings.*`, the Controls tab of the PC page):**
- Bindings sit on top of the game's own controller layouts. The game has four (Options > Controls: Default,
  Shooter, Acrobat, Ninja); each maps GamePad buttons to actions (`OptionGameCtrLayoutList` in
  `[DevilsThirdGame.D3UIWidget_OptionMenuGame]`, applied by its UnrealScript as `Bindings_Standard/_Shooter/...` over
  the XboxTypeS keys). We map keys, mouse buttons and controller buttons to GamePad buttons, so the game's layout
  still decides what each does.
- The player's layout is read from the save (`d3sys.sav`, UE3 tagged property `ControllerLayout`, IntProperty; absent
  means 0). Action and layout names come from the game's own text (`DT_UI_Pause.INT` `[Menus]`: "Guard / Dodge",
  "Reload / Interact", ...) in the player's language, read from the Coalesced file the game decrypts at boot
  (`appDecryptData` hook; kept in memory only, `assets/coalesced.*`).
- Targets: the 16 GamePad buttons (A, B, X, Y, L, R, ZL, ZR, stick clicks, D-pad, +, -) and, for the keyboard, the
  four left-stick directions. Each has two keyboard/mouse slots (keys, mouse buttons, wheel notches) and one
  controller slot (buttons and triggers). Defaults are the port's earlier fixed mapping. Saved as
  `bindings.<target>.keys` / `.pad` (SDL scancode and gamepad names).
- Rebinding: select a cell, Enter/A, press the new input (Escape cancels; 8 s timeout). An input already used on the
  same device asks first ("Space is already used for Jump. Use it for Reload / Interact instead?") and moves it.
  Delete/Y restores the cell's default. Reset rows for keyboard and mouse and for the controller.
- Mouse speed and stick dead zone stay on the tab. Escape, F1, F11 and the Guide button cannot be bound.

- Enbaku (the game's one two-button action; its help: "Press Jump + Fast Melee Attack together"): its own target,
  F by default, no controller default (pads press the two buttons as on the Wii U), bindable to a single button. It
  presses the layout's GA_JUMP and GA_MELEE buttons on the same frame for as long as it is held; the game's input
  bindings put `Button bSpecialModeStartA` / `B` on those buttons in every layout, and
  `DevilsThirdPlayerController.CheckSpecialMode` (bytecode read with a scratch decoder) adds DeltaTime to a hold timer
  per button while held (reset on release) and calls `StartSpecialMode` when both are held and both timers are at most
  0.033 * 3 = 0.099 s: the presses must come within about 0.1 s of each other. No other controller check uses such a
  window (throws, dodges and melee strings are states or single buttons).

**Still open:** separate X/Y mouse sensitivity, outer dead zone, response curve, trigger threshold.

## Stage 6: controller detection and button prompts

- **Pad type:** `SDL_GetGamepadType` classifies each pad as Xbox, PlayStation (DS4, DualSense), Nintendo (Pro,
  Joy-Con) or generic.
- **Hot-plug:** add and remove events.
- **Active input:** the last device that produced input. Prompts switch between the controller and keyboard/mouse.
- **Confirm and back:**
  - Default to the platform's convention: south confirms on Xbox and PlayStation; east confirms on Nintendo, as on
    the Wii U.
  - The game's A action is mapped to the confirm button.
  - Configurable.
- **Button prompts.** First find how the game draws Wii U button prompts, using `--capture-frame` on the title menu
  and in-game help. Candidates:
  - Scaleform GFx movies with button sprites or a button font (glyphs in a private-use range);
  - UE3 textures drawn by Canvas;
  - a texture atlas.

  Then replace them at the lowest level that works:
  - a texture: swap its contents per controller type when uploaded;
  - a font glyph: substitute in the glyph texture;
  - a GFx symbol: patch the movie's image.

  Our own icon art (Xbox, PlayStation, Nintendo, keyboard/mouse) is drawn for this project, or taken from a
  CC0 / permissively licensed prompt set; nothing from the game goes in git.
- **Status.** Device detection and hot-swapping, platform confirm/back, the PC page's glyphs, and the shared prompt
  library (`ui_common/buttonlibrary`, patched in memory with drawn caps, faces, symbols and mouse icons: title, menus,
  Options header, sticks and D-pad) are done. Still to do: Wii U art drawn locally in other movies (option menu footer,
  dialogs), the HUD's built-in keyboard texts (HudText) and Enbaku's keyboard prompt.

## Stage 7: internal resolution

- **Status (stage 7a, done):** 1080p/1440p/4K/custom on the Graphics tab (applies at the next start). TV-derived targets
  are scaled; viewports, scissors, clears, copies, uploads, scan-out and the pixel-position input follow. Next: texel
  fetches and texture size queries on scaled textures, the pause menu's blur, bloom/DOF checks, shadow-map resolution,
  then the snapshot copies (the main GPU cost at 1440p).

- **Setting:** 720p (native), 1080p, 1440p, 4K or custom. Start with 2560x1440.
- **Scaling rules**, the way Cemu's graphic packs do it:
  - Surfaces whose size derives from the TV resolution (1280x720 and its fractions, from the game's render-target
    setup) get scaled images.
  - The guest still sees the original sizes and pitches.
  - Viewports, scissors and `GX2CopySurface` regions into or out of scaled surfaces are scaled too, and so are
    resolves and the scan-buffer copy.
- **Shaders that assume the original pixel size:**
  - texel-size constants (1/1280, 1/720) in post-processing: bloom, DOF, FXAA, SSAO and shadow-map
    filtering;
  - `POSITION_ENA` pixel positions.

  These are found with captures and fixed per shader hash, by rewriting the constant in the uniform upload or
  scaling the position input.
- **UI:** stays sharp. GFx and Canvas draws render into the scaled scan buffer at the higher resolution, with the
  same layout.
- **Shadow maps and reflection targets** have their own size setting and are not tied to the screen.

## Stage 8: unlimited frame rate
Remove every frame-rate dependency in the game code, then offer 120, 144, 240 and unlimited.

- **Function hooks in the recompiler.** `dt_recomp` reads a list of guest functions to hook (by name or address).
  For each one it emits the translation as `orig_sub_X` and makes `sub_X` call a runtime hook. The hook can call
  the original. This avoids hand-editing generated code.
- **Cap setting:** 50, 60, 120, 144, 240 or unlimited. Swap pacing follows the cap instead of 59.94 Hz.
- **Testing at each cap:**
  - movement and run speed (distance over a scripted walk);
  - physics (PhysX step);
  - particles;
  - melee combo timing;
  - cutscene and Bink movie speed;
  - Wwise audio sync.

  Code that assumes a fixed step is found by name (the symbols are present) and patched with a hook. The cap is
  never lowered to hide it.
