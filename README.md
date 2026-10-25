# Universal MFG Unlocker V1.7.0

Multi Frame Generation for Windows x64 games, in one DLL:

- **DLSS Multi Frame Generation** controls on RTX 50, RTX 40 and RTX 30 series
  GPUs (RTX 20 experimental), with fixed multipliers up to 6x and Dynamic MFG
  where supported, on their own or through [OptiScaler](#optiscaler-support).
  RTX 50 cards run DLSS MFG natively, so MFG Unlocker changes nothing in NVIDIA's
  code there and only adds its menu controls.
- **XeSS Multi Frame Generation (XeMFG)** on NVIDIA, AMD and Intel GPUs, on
  its own ([XeMFG standalone](#xemfg-standalone), experimental) or through
  [OptiScaler](#optiscaler-support).

For DLSS MFG the game must already have a Streamline DLSS Frame Generation
integration. Available modes depend on the GPU, the active runtime and NVIDIA's
per game MFG override maximum. A listed maximum does not guarantee that the
active Frame Generation pipeline can apply it.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## V1.7.0

- **DLSS 5 support in the menu.** With NVIDIA's DLSS 5 dll (`nvngx_dlssnr.dll`)
  beside the game's executable or MFG Unlocker, a **DLSS 5** tab applies DLSS 5
  in games that run DLSS Super Resolution, DLAA or Ray Reconstruction. Choose
  the **Hook Method** (Auto, Before upscaling, After upscaling or Present), a
  **Look** (Subtle, Balanced or Strong) and the **Resolution Scale** (with
  **Auto**, which lowers it to keep DLSS 5's cost in budget, never below 75 %);
  **Advanced** holds the rest. **Half rate** runs the network on every other
  frame for about half the cost (DirectX 12, Before or After upscaling). In
  Vulkan games DLSS 5 runs After upscaling or Auto. MFG Unlocker does not
  include or download the dll, and the tab only appears when one is there.
- **XeMFG standalone (experimental).** XeSS Multi Frame Generation without
  OptiScaler, in DirectX 12 and Vulkan games, at 2x to 6x or Dynamic. See
  [XeMFG standalone](#xemfg-standalone).
- **MFG Unlocker's menu beside OptiScaler.** With OptiScaler, MFG Unlocker's
  menu now shares the screen with OptiScaler's while the game runs DLSS Super
  Resolution, DLAA, Ray Reconstruction or DLSS 5, and offers the DLSS presets
  and DLSS 5 there. Frame generation stays in OptiScaler's menu. Forza
  Motorsport and Forza Horizon keep it hidden. To hide it in every game, set
  `"optiScalerUi": "optiscaler"` in `RTXMFG-Universal.json`.
- **Smoother XeMFG.** A new flip queue presents each batch of XeMFG frames
  on an even cadence, which removes the stutter some games had. On AMD Radeon
  RX 7000 and RX 9000 with Adrenalin 26.10.1 or newer it also drives the
  driver's flip meter. Radeon RX 6000 and older, and Wine/Proton, keep V1.6.1's
  pacing. To go back to V1.6.1's pacing on any GPU, set `XeFGFlipQueue = off`
  under `[XeFG]` in `OptiScaler.ini`.
- **XeMFG in Vulkan games through OptiScaler** runs on OptiScaler's
  XeFG-on-Vulkan path with any frame generation input. Use the OptiScaler
  build linked under [OptiScaler support](#optiscaler-support).
- **RTX 30 and RTX 20:** DLSS Frame Generation starts when the game's
  Streamline allocates fewer frame buffers than MFG Unlocker expected
  (Final Fantasy VII Rebirth on an RTX 2070 SUPER), and when NVIDIA's runtime
  answers Streamline's startup check with an error. When it can't start,
  MFG Unlocker tells Streamline to keep it off instead of retrying every
  frame (Doom: The Dark Ages's video memory grew until it crashed while the
  retries ran).
- **Also:** the Reflex FPS limit works with Streamline Reflex 2.7 to 2.14
  (Wuthering Waves); a French menu translation; the menu's cursor follows the
  mouse smoothly while frame generation runs; and the log has timestamps, a
  crash record with a minidump beside it, and a `Session end` line.


## Install

The v1.7.0 download contains **`MFGUnlocker.dll`**. The menu is built in; no
separate ReShade or external loader installation is needed. To use OptiScaler,
install it as an OptiScaler plugin instead; see [OptiScaler support](#optiscaler-support).

1. Close the game. If upgrading, remove the old mod DLL as described below first.
2. Download `MFGUnlocker-v1.7.0.zip` from [Releases](https://github.com/dashdogy/MFG-Unlocker/releases)
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
  Ukrainian (Українська), Italian (Italiano), Swedish (Svenska), Turkish (Türkçe),
  Vietnamese (Tiếng Việt) and French (Français) are included;
  diagnostics stay in English. To add a translation, see
  [source/native/locale](source/native/locale/README.md).

Check the status line for pending changes or restart instructions.

## OptiScaler support

MFG Unlocker runs as an [OptiScaler](https://github.com/optiscaler/OptiScaler)
plugin, for both DLSS MFG and XeMFG. Use a current OptiScaler 10 build; the
0.9.x releases are not supported.

MFG Unlocker V1.7.0 pairs with our OptiScaler build,
[OptiScaler for MFG Unlocker V1.7.0](https://github.com/dashdogy/OptiScaler/releases/tag/mfgunlocker-v1.7.0) (dashdogy/OptiScaler).
Install OptiScaler as usual from its nightly-20261024, then replace
`OptiScaler.dll` with the one from that release, keeping the name it was
installed under. Stock OptiScaler does not have these yet, until they are
upstreamed:

- `FGOutput = XeFG` in Vulkan games, with any `FGInput` (`Upscaler` or
  `DLSSG`): XeMFG in Vulkan games needs this build.
- `FGInput = DLSSG` in games that check DLSS Frame Generation at startup;
  with stock OptiScaler, Doom: The Dark Ages greys out its Frame Generation
  option.
- `FGOutput = XeFG` in DirectX 12 games: OptiScaler's menu and FPS overlay
  no longer hold the game in Present.
- `FGInput = FSRFG` or `FSRFG30` (the game's own FSR Frame Generation): XeFG leaves the
  swap chain to it, and the game's UI still shows while frame generation is
  off (Modern Warfare 4).

DLSS MFG through OptiScaler, and XeMFG in DirectX 12 games with the
`Upscaler` input, also work with a current upstream OptiScaler 10 build.

1. Rename `MFGUnlocker.dll` to `MFGUnlocker.asi` and place it in OptiScaler's
   plugins folder (`OptiScaler\plugins` beside OptiScaler).
2. In `OptiScaler.ini`, set `LoadAsiPlugins = true` under `[Plugins]`.
3. Do not also install MFG Unlocker under a proxy filename.
4. Choose the frame generation output under `[FrameGen]`:
   - **DLSS MFG:** set `FGOutput = DLSSG` and choose the multiplier with
     **Override DLSSG Ratio** in OptiScaler's menu. On RTX 30 this route is
     experimental; if it does not start, use XeMFG.
   - **XeMFG:** set `FGOutput = XeFG`; see [XeSS MFG (XeMFG)](#xess-mfg-xemfg).

With OptiScaler, its menu owns the frame generation controls. MFG Unlocker's
menu shows beside it only while the game runs DLSS Super Resolution, DLAA,
Ray Reconstruction or DLSS 5, and offers the DLSS presets and DLSS 5 there
(hidden in Forza Motorsport and Forza Horizon). Set `"optiScalerUi": "optiscaler"`
in `RTXMFG-Universal.json` to keep it hidden in every game.

## XeSS MFG (XeMFG)

XeMFG unlocks Intel XeSS Frame Generation beyond 2x, up to 6x, on NVIDIA, AMD
and Intel GPUs. It runs on its own (XeMFG standalone, below) or through
OptiScaler.

### XeMFG standalone

Experimental. MFG Unlocker runs XeSS Frame Generation itself, without
OptiScaler, in DirectX 12 and Vulkan games:

1. Download Intel's XeSS SDK (3.0.2 is tested) and copy `libxess_fg.dll` and
   `libxell.dll` into a folder named `RTXMFG-XeSS` beside MFG Unlocker. MFG
   Unlocker does not include Intel's runtime, and leaves XeMFG off with a
   version it does not recognize (the menu says why).
2. Open the menu, tick **XeMFG standalone** and choose the **MFG mode** (2x to
   6x, or **Dynamic**). Restart the game; the mode can change while playing.
   Dynamic changes the frame count every game frame so XeMFG's frame rate
   follows the **Dynamic target** (the FPS limit when one is set, otherwise just
   under the display's refresh rate, or a value you set), up to the **Up to**
   count. **Rounding** picks the count closest to the target, the fewest that
   reach it, or the most that stay under it; **Allow 1X** lets it turn frame
   generation off; **Use target as cap** holds the frame rate at the target
   with the FPS limit.
3. In games with DLSS Frame Generation, turn it on in the game's settings.
   XeMFG replaces it: DLSS Frame Generation itself stays off, and XeMFG uses its
   inputs, including the game's HUD-less image. Other games need DLSS, XeSS or
   FSR 3.1 upscaling (or newer) on; XeMFG uses that upscaler's inputs. Turn off
   the game's own FSR or XeSS frame generation.

Its limits for now: on Vulkan only DLSS upscaling inputs are used; games with
display-resolution motion vectors are not supported; FSR 2 and FSR 3.0 built
into a game cannot be read; and without the game's HUD-less image, the HUD is
interpolated with the scene. The menu's status shows the inputs in use and what
XeMFG is waiting for.

### XeMFG with OptiScaler

- In OptiScaler, set the frame generation output to XeFG
  (`FGOutput = XeFG` under `[FrameGen]`) and choose the frame count in its menu.
- **Dynamic XeMFG** (XeMFG standalone has its own, above) needs OptiScaler with Dynamic MFG for XeFG
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
  `XeFGFlipMetering = off` under `[XeFG]` in `OptiScaler.ini` and restart the
  game.
- XeFG's scene-change detector is off. It treated fast camera turns as scene
  cuts, so the picture froze and then jumped; real cuts still reset when the
  game reports them. To use Intel's detector, set
  `XeFGSceneChangeThreshold = intel` (or a threshold from 0 to 1) under `[XeFG]`
  in `OptiScaler.ini`.
- XeFG's own HDR10 handling is off. With it on, players with very bright HDR
  displays saw interpolation errors in the brightest areas. To use it, set
  `XeFGHdr10Flag = stock` under `[XeFG]` in `OptiScaler.ini` and restart the
  game. This does not apply on Arc.
- OptiScaler's frame limit applies to output frames. At 6x, a 117 FPS limit
  leaves about 19.5 real frames per second; raise or remove it.
- MFG Unlocker uses the XeSS Frame Generation files that come with the game or
  OptiScaler (or, standalone, the `RTXMFG-XeSS` folder); it does not include
  Intel's runtime.

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
unavailable on Vulkan. XeMFG runs through OptiScaler or, experimentally, as
XeMFG standalone.
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
[issue #6](https://github.com/dashdogy/MFG-Unlocker/issues/6). On RTX 30,
restart after changing presets. This is a reported workaround, not a fix for
every game.

For [bug reports](https://github.com/dashdogy/MFG-Unlocker/issues), include
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
- French: thanks to Slideulidou (@slideulidou on Discord) for editing the French translation.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third party components retain their respective licences and notices.
`MFGUnlocker.dll` also embeds XeSS Frame Generation program data derived from
Intel's XeSS SDK. That data is Intel's work, is not covered by the MIT License,
and remains subject to Intel's licence terms. See [NOTICE.md](NOTICE.md).
