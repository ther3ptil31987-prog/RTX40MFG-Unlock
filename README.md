# Universal MFG Unlocker V1.6.1

Multi Frame Generation for Windows x64 games, in one DLL:

- **DLSS Multi Frame Generation** controls on RTX 50, RTX 40 and RTX 30 series
  GPUs (RTX 20 experimental), with fixed multipliers up to 6x and Dynamic MFG
  where supported, on their own or through [OptiScaler](#optiscaler-support).
  RTX 50 cards run DLSS MFG natively, so MFG Unlocker changes nothing in NVIDIA's
  code there and only adds its menu controls.
- **XeSS Multi Frame Generation (XeMFG)** on NVIDIA, AMD and Intel GPUs
  through [OptiScaler](#optiscaler-support).

For DLSS MFG the game must already have a Streamline DLSS Frame Generation
integration. Available modes depend on the GPU, the active runtime and NVIDIA's
per game MFG override maximum. A listed maximum does not guarantee that the
active Frame Generation pipeline can apply it.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## V1.6.1

- **Change DLSS presets while you play, on every RTX card.** Live preset
  switching now works on RTX 50, RTX 30 and RTX 20 cards as well as RTX 40:
  the DLSS Frame Generation preset (Game/driver, A, B or C) and the DLSS
  Super Resolution and Ray Reconstruction presets apply while the game runs.
  Frame Generation restarts once when its preset changes. Vulkan games, and
  DLSS Frame Generation versions that only have their original preset, still
  need a game restart. See [Usage](#usage).
- **RTX 50 series support.** MFG Unlocker recognises RTX 50 cards, and the
  menu works there as on the other cards: MFG mode and Dynamic, the DLSS
  presets, the Reflex FPS limit and the FPS readout. RTX 50 runs DLSS MFG
  natively, so MFG Unlocker changes nothing in NVIDIA's code there.
- **RTX 30 and RTX 20:** a fix for the DLSS Frame Generation option staying
  greyed out when the NVIDIA app's DLSS override supplies DLSS Frame
  Generation (reported in Final Fantasy VII Rebirth). If it still happens,
  the log now says why.

## V1.6.0

- **Change DLSS presets while you play.** DLSS Frame Generation presets
  apply while Frame Generation runs (RTX 40). A new menu section shows whether DLSS Super
  Resolution or Ray Reconstruction is running and switches their presets live.
  See [Usage](#usage).
- **DLSS Frame Generation on RTX 20** (experimental): RTX 2060 to RTX 2080 Ti,
  the same way as on RTX 30. Support is experimental; if you run into errors,
  please attach your logs to an issue.
- **RTX 30 DLSS Frame Generation in Vulkan games** (experimental). **V-Sync: Off**
  and the measured DLSS FPS now also work in Vulkan games.
- **XeMFG:** XeSS Frame Generation 1.3.4 support, smoother pacing after a game
  hitch, evenly spaced generated frames with flip metering, and no more freeze
  then jump on fast camera turns. See [XeSS MFG (XeMFG)](#xess-mfg-xemfg).
- **Logs are written to the game folder** instead of `%TEMP%`.
- **Fixes two crashes at game launch:** when Streamline loads the NVIDIA app's
  newer DLSS Frame Generation plug-in in place of the game's own copy, and with
  OptiScaler in some Streamline games such as Subnautica 2.

**Many fixes for RTX 30 series cards:**

- **No more stall between generated frames at 3x and above.** RTX 30 DLSS
  Frame Generation now uses the driver's flip metering by default.
- **Star Wars Outlaws:** the black flicker after a resize or fullscreen switch
  is fixed.
- **OptiScaler:** RTX 30 DLSS Frame Generation is detected correctly when
  OptiScaler is loaded, and OptiScaler's own DLSSG output can now start it on
  RTX 30 (experimental). See [OptiScaler support](#optiscaler-support).
- **Game FG option: Unlock** shows the DLSS Frame Generation option in games
  that hide it on RTX 30 cards. See
  [Compatibility and troubleshooting](#compatibility-and-troubleshooting).
- **XeMFG runs on the tensor cores** of RTX 30 cards.

Also new:

- **XeMFG on tensor cores.** On RTX 20, RTX 30, RTX 40 and RTX 50 GPUs the
  model's network layers run on the tensor cores. On other GPUs the model runs
  about 7% faster than in V1.5.0.
- **XeMFG uses the driver's flip metering on NVIDIA GPUs**, the way DLSS Frame
  Generation paces its frames on RTX 40. See [XeSS MFG (XeMFG)](#xess-mfg-xemfg).
- **XeMFG Dynamic** holds its target better and no longer stays on a higher
  multiplier once the target is reached. It needs OptiScaler's Dynamic MFG for
  XeFG, in OptiScaler nightlies from 20 October 2026 on; see
  [XeSS MFG (XeMFG)](#xess-mfg-xemfg).
- **Native 5x and 6x on Intel Arc.**
- **RTX 40 with OptiScaler's DLSSG output** is no longer stuck at 2x.
- **XeMFG is no longer stuck at 2x in games that start it right at launch**
  (seen in Trails in the Sky 2nd Chapter with OptiScaler). MFG Unlocker now
  hooks XeSS Frame Generation before OptiScaler first creates it.
- **Lower XeMFG latency without flip metering** (AMD and Intel GPUs, or with
  flip metering off). MFG Unlocker's pacing no longer holds each frame back by
  XeSS Frame Generation's GPU latency; frames go out on XeFG's own schedule.
- **Updated NVIDIA game list:** games such as Dragon's Dogma 2 are no longer
  capped at 2x.
- **Menu translations:** Simplified Chinese, Ukrainian, Italian, Swedish,
  Turkish and Vietnamese. A **Pin** button keeps the menu on screen, and
  **Pin FPS** shows a small real and DLSS FPS readout in the top left corner
  (DLSS Frame Generation only).
- Includes the VRAM leak fix from V1.5.0-hotfix.1.

## Install

The v1.6.1 download contains **`MFGUnlocker.dll`**. The menu is built in; no
separate ReShade or external loader installation is needed. To use OptiScaler,
install it as an OptiScaler plugin instead; see [OptiScaler support](#optiscaler-support).

1. Close the game. If upgrading, remove the old mod DLL as described below first.
2. Download `MFGUnlocker-v1.6.1.zip` from [Releases](https://github.com/dashdogy/RTX40MFG-Unlock/releases)
   and extract it.
3. Rename `MFGUnlocker.dll` to **one** supported filename below that the game
   loads early. Place it beside the **actual game executable**, not the launcher.
4. Launch the game, enable Frame Generation where available, and press
   **Backspace** to open the menu. On first launch it opens automatically.

Install only one copy. Do not overwrite a DLL belonging to the game or another
mod; choose another suitable name if it is already occupied. Leaving the file
named `MFGUnlocker.dll` does not make the game load it automatically.

### Supported filenames

```text
version.dll
dinput8.dll
winmm.dll
d3d9.dll
d3d10.dll
d3d11.dll
d3d12.dll
dxgi.dll
dsound.dll
wininet.dll
winhttp.dll
binkw64.dll
bink2w64.dll
xinput1_1.dll
xinput1_2.dll
xinput1_3.dll
xinput1_4.dll
xinput9_1_0.dll
xinputuap.dll
```

The correct name depends on the game. A supported name must actually be loaded
before Frame Generation starts; Vulkan games may not load DirectX proxy names.

**For Bink games:** back up the game's original DLL first, then rename
`binkw64.dll` to `binkw64Hooked.dll`, or `bink2w64.dll` to
`bink2w64Hooked.dll`. Give MFG Unlocker the original filename and keep both
files beside the executable. These routes require the game's original Bink DLL.

## Upgrade

From v1.4 or another single DLL release: close the game, delete the installed
`RTXMFG.dll` (or the name you gave it), and install `MFGUnlocker.dll` under the
same filename. Settings and logs keep their `RTXMFG` file names, so your
preferences carry over.

When upgrading from v1.2, move `RTX40MFGCore.dll`, `RTX40MFG.asi`, and
`RTX40MFG-UI.addon64` to a backup outside the game's loading directories.
Also remove older Universal/Bridge mod components or corresponding RTX 30 split
files, if installed. Preserve unrelated mods and their loaders.

## Usage

Press **Backspace** to open or close the menu. The hotkey can be changed there.
The default **Follow game** mode keeps the game's requested multiplier. The
first launch menu opens when the game window is foreground and does not reopen
automatically after it has been shown and saved.

- Choose **Follow game**, a fixed multiplier, or **Dynamic** when available.
- Dynamic can target the display refresh rate or a custom FPS value.
- Select **Game/driver**, **Preset A**, **Preset B**, or **Preset C
  (performance)** where supported. A new preset applies while Frame Generation
  runs (it restarts Frame Generation once) on RTX 50, RTX 40, RTX 30 and RTX 20
  in DirectX 12 games. Vulkan games, and DLSS-G versions that only have their
  original preset, need a game restart.
- When the game runs **DLSS Super Resolution**, the menu shows it and lets you
  pick its preset (J, K, L or M); with **Ray Reconstruction** the same section
  offers D, E or F (F needs NVIDIA driver 580 or newer). A new preset applies
  right away. **Game / driver default** leaves the choice to the game and the
  NVIDIA app. DirectX 12 games, without OptiScaler.
- **Limit FPS with Reflex** provides a separate cap where supported.
  The manual cap is unavailable while Dynamic is selected or active.
- **V-Sync: Off** presents the game's frames without V-Sync, for games that
  keep V-Sync on with Frame Generation. **Game** keeps the game's setting.
  NVIDIA Control Panel V-Sync settings take priority. In Vulkan games, Off
  applies when the game next creates its swap chain.
- The **language** list beside the menu key sets the menu language. **Auto**
  follows the Windows display language. Simplified Chinese (简体中文),
  Ukrainian (Українська), Italian (Italiano), Swedish (Svenska), Turkish (Türkçe)
  and Vietnamese (Tiếng Việt) are included;
  diagnostics stay in English. To add a translation, see
  [source/native/locale](source/native/locale/README.md).

Check the status line for pending changes or restart instructions.

## OptiScaler support

MFG Unlocker runs as an [OptiScaler](https://github.com/optiscaler/OptiScaler)
plugin, for both DLSS MFG and XeMFG. Use a current OptiScaler 10 build; the
0.9.x releases are not supported.

1. Rename `MFGUnlocker.dll` to `MFGUnlocker.asi` and place it in OptiScaler's
   plugins folder (`OptiScaler\plugins` beside OptiScaler).
2. In `OptiScaler.ini`, set `LoadAsiPlugins = true` under `[Plugins]`.
3. Do not also install MFG Unlocker under a proxy filename.
4. Choose the frame generation output under `[FrameGen]`:
   - **DLSS MFG:** set `FGOutput = DLSSG` and choose the multiplier with
     **Override DLSSG Ratio** in OptiScaler's menu. On RTX 30 this route is
     experimental; if it does not start, use XeMFG.
   - **XeMFG:** set `FGOutput = XeFG`; see [XeSS MFG (XeMFG)](#xess-mfg-xemfg).

With OptiScaler, its menu owns the frame generation controls and MFG Unlocker's
own menu stays hidden.

## XeSS MFG (XeMFG)

XeMFG unlocks Intel XeSS Frame Generation beyond 2x, up to 6x, on NVIDIA, AMD
and Intel GPUs. It runs only through OptiScaler.

- In OptiScaler, set the frame generation output to XeFG
  (`FGOutput = XeFG` under `[FrameGen]`) and choose the frame count in its menu.
- **Dynamic XeMFG** needs OptiScaler with Dynamic MFG for XeFG
  ([pull request #1220](https://github.com/optiscaler/OptiScaler/pull/1220),
  merged on 19 October 2026): an
  [OptiScaler nightly](https://github.com/optiscaler/OptiScaler-nightly/releases)
  from 20 October 2026 on, or the next OptiScaler release. Tick
  **Dynamic MFG** next to **Frame Gen** in OptiScaler's menu and set the
  **Target FPS**; the slider, then shown as **Max Frame Gen**, sets the highest
  multiplier.
- On Intel Arc, XeMFG uses Intel's native driver model and pacer. MFG Unlocker
  restores 5x and 6x when it recognizes the loaded XeFG and XeLL driver pair;
  an unknown driver keeps its stock limits. Arc support is
  experimental. Leave the frame generation override in Intel Graphics Software
  off or following the game. The model and tensor core settings below do not
  apply on Arc.
- MFG Unlocker runs Intel's multi frame model on any GPU by default. To use
  Intel's 2x model instead, set `XeFGModel = default` under `[XeFG]` in
  `OptiScaler.ini` and restart the game.
- On RTX 20, RTX 30, RTX 40 and RTX 50 GPUs, the model's int8 network layers run
  on the tensor cores (CUDA kernels launched inside XeFG's own command list; their output is
  identical to Intel's). Other GPUs, and any layer this does not cover, run
  as before. To turn it off, set `XeFGTensorCores = off` under `[XeFG]` in
  `OptiScaler.ini` and restart the game.
- On NVIDIA GPUs, MFG Unlocker paces XeMFG's frames with the driver's flip
  metering, the way DLSS Frame Generation does on RTX 40. To turn it off, set
  `XeFGFlipMetering = off` under `[XeFG]` in `OptiScaler.ini`. To compare on and
  off while playing, set `XeFGFlipMeteringHotkey = on` there and press
  **Page Up**. Page Up is also OptiScaler's default FPS overlay key, so it
  toggles that overlay too. Restart the game after changing either line.
- XeFG's scene-change detector is off. It treated fast camera turns as scene
  cuts, so the picture froze and then jumped; real cuts still reset when the
  game reports them. To use Intel's detector, set
  `XeFGSceneChangeThreshold = intel` (or a threshold from 0 to 1) under `[XeFG]`
  in `OptiScaler.ini`.
- OptiScaler's frame limit applies to output frames. At 6x, a 117 FPS limit
  leaves about 19.5 real frames per second; raise or remove it.
- MFG Unlocker uses the XeSS Frame Generation files that come with the game or
  OptiScaler; it does not include Intel's runtime.

## Path traced hair (The Witcher 3)

**Path traced HairWorks on RTX 40.** A hair compatibility path built into the
mod, for supported Witcher 3 DirectX 12 builds, retains the game's hair
materials and shading while bringing animated strands into its path traced
lighting.

For The Witcher 3 DirectX 12, install MFG Unlocker as **`winmm.dll`** beside
`witcher3.exe`.

Enable the game's HairWorks, path tracing and **Path Traced Hair** settings;
MFG Unlocker follows those settings automatically.
**Fully restart the game after changing Path Traced Hair or Frame Generation.**
The mod menu's **Path traced hair** line shows its activity or why it is
unavailable. Support is experimental and may stop working after a game patch.

## Compatibility and troubleshooting

RTX 50, RTX 40 and RTX 30 have DirectX 12 and experimental Vulkan paths. RTX 20
support is experimental. On RTX 50 the menu offers the modes the game's DLSS
Frame Generation reports, up to NVIDIA's per game maximum. Dynamic MFG is
unavailable on Vulkan. XeMFG requires OptiScaler.
Compatibility varies by game; artifacts, freezes, black screens and crashes
remain possible.

If the menu does not appear, check the executable folder, chosen proxy name,
and any leftover older mod files. Fully close the game before changing files.

RTX 30 DLSS Frame Generation uses the driver's flip metering by default. To go
back to the V1.5.0 pacing, set `Rtx30FlipMetering=0` under `[Compatibility]` in
`RTX40MFG-UI.ini` beside the game executable, then restart the game.

Some games remove their DLSS Frame Generation option on RTX 30 cards even
though MFG Unlocker supports it (for example Wuthering Waves, where the option
then appears but Frame Generation does not start yet). To show it, tick
**Game FG option: Unlock** in the menu's Frame generation section, then restart
the game. The choice is saved in `RTX40MFG-UI.ini` beside the game executable
(`GpuNameOverride` under `[Compatibility]`).

The game then sees an RTX 30 card as an NVIDIA GeForce RTX 4090, so its
recommended graphics settings may assume one. DLSS, Streamline, the driver and
MFG Unlocker keep using your real GPU. The setting is off by default and does
nothing on other GPUs. Online games with anti cheat may treat any mod as a risk.

For frozen presentation, some users reported that **Preset B** helped in
[issue #6](https://github.com/dashdogy/RTX40MFG-Unlock/issues/6). On RTX 30,
restart after changing presets. This is a reported workaround, not a fix for
every game.

For [bug reports](https://github.com/dashdogy/RTX40MFG-Unlock/issues), include
the game, GPU, driver, proxy filename (or OptiScaler plugin), multiplier and
preset, plus `RTXMFG-<game>-<path-hash>.log` and `RTXMFG-Universal.status.json`
from the game folder, beside the game's executable and the mod settings. If
that folder cannot be written, the log goes to `%TEMP%` instead. Each launch
keeps the previous runtime log as `RTXMFG-<game>-<path-hash>.previous.log` and
creates a new log. The status file is replaced on each launch.
With OptiScaler, also include `OptiScaler.log`.

## Uninstall

Close the game and remove the renamed MFG Unlocker DLL, or `MFGUnlocker.asi`
from OptiScaler's plugins folder. Restore any original DLL you renamed to a
`Hooked` filename. Keep `RTXMFG-Universal.json` if you want to retain your
settings. The `RTXMFG-*.log` files in the game folder are diagnostics only and
can be deleted.

## Translation credits
- Vietnamese: thanks to Shi (@shinsir on Discord) for editing the whole Vietnamese translation.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third party components retain their respective licences and notices.
`MFGUnlocker.dll` also embeds XeSS Frame Generation program data derived from
Intel's XeSS SDK. That data is Intel's work, is not covered by the MIT License,
and remains subject to Intel's licence terms. See [NOTICE.md](NOTICE.md).
