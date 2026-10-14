# Universal MFG Unlocker V1.5.0

Multi Frame Generation for Windows x64 games, in one DLL:

- **DLSS Multi Frame Generation** controls on RTX 40 and RTX 30 series GPUs,
  with fixed multipliers up to 6x and Dynamic MFG where supported.
- **XeSS Multi Frame Generation (XeMFG)** on NVIDIA, AMD and Intel GPUs
  through [OptiScaler](#optiscaler-support).

For DLSS MFG the game must already have a Streamline DLSS Frame Generation
integration. Available modes depend on the GPU, the active runtime and NVIDIA's
per game MFG override maximum. A listed maximum does not guarantee that the
active Frame Generation pipeline can apply it.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## V1.5.0

- **New name: Universal MFG Unlocker.** The download contains
  `MFGUnlocker.dll`. When upgrading, remove the old `RTXMFG.dll` (or its renamed
  copy) first so only one copy loads. Settings in `RTXMFG-Universal.json` carry
  over.
- **OptiScaler support.** MFG Unlocker now runs as an OptiScaler plugin
  (`MFGUnlocker.asi` in `OptiScaler\plugins`), working with OptiScaler's frame
  generation, latency reduction and menu. See
  [OptiScaler support](#optiscaler-support).
- **XeSS Multi Frame Generation through OptiScaler**, 2x to 6x on any GPU, with
  Intel's multi frame model by default. See [XeSS MFG (XeMFG)](#xess-mfg-xemfg).
- **DLSS Frame Generation Preset C**, NVIDIA's new performance mode, on RTX 40
  and RTX 30, including at 3x to 6x. Select **Preset C** in the menu or in the
  NVIDIA app's DLSS override. It needs DLSS Frame Generation 310.9.16, which
  the NVIDIA app provides.
- **RTX 30 series support is now stable**, with smoother pacing at 3x and above
  and DLSS Frame Generation 310.9.16.

## Install

The v1.5.0 download contains **`MFGUnlocker.dll`**. The menu is built in; no
separate ReShade or external loader installation is needed. To use XeMFG, install
it as an OptiScaler plugin instead; see [OptiScaler support](#optiscaler-support).

1. Close the game. If upgrading, remove the old mod DLL as described below first.
2. Download `MFGUnlocker-v1.5.0.zip` from [Releases](https://github.com/dashdogy/RTX40MFG-Unlock/releases)
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
  (performance)** where supported. Preset changes after initialization require
  a restart.
- **Limit FPS with Reflex** provides a separate cap where supported.
  The manual cap is unavailable while Dynamic is selected or active.
- **V-Sync: Off** presents the game's frames without V-Sync, for games that
  keep V-Sync on with Frame Generation. **Game** keeps the game's setting.
  NVIDIA Control Panel V-Sync settings take priority.

Check the status line for pending changes or restart instructions.

## OptiScaler support

MFG Unlocker runs as an [OptiScaler](https://github.com/optiscaler/OptiScaler)
plugin. Use a current OptiScaler 10 build; the 0.9.x releases are not supported.

1. Rename `MFGUnlocker.dll` to `MFGUnlocker.asi` and place it in OptiScaler's
   plugins folder (`OptiScaler\plugins` beside OptiScaler).
2. In `OptiScaler.ini`, set `LoadAsiPlugins = true` under `[Plugins]`.
3. Do not also install MFG Unlocker under a proxy filename.

With OptiScaler, its menu owns the frame generation controls and MFG Unlocker's
own menu stays hidden.

## XeSS MFG (XeMFG)

XeMFG unlocks Intel XeSS Frame Generation beyond 2x, up to 6x, on NVIDIA, AMD
and Intel GPUs. It runs only through OptiScaler.

- In OptiScaler, set the frame generation output to XeFG
  (`FGOutput = XeFG` under `[FrameGen]`) and choose the frame count in its menu.
- MFG Unlocker runs Intel's multi frame model on any GPU by default. To use
  Intel's 2x model instead, set `XeFGModel = default` under `[XeFG]` in
  `OptiScaler.ini` and restart the game.
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

RTX 40 has DirectX 12 and experimental Vulkan paths. RTX 30 support is
DirectX 12 only. Dynamic MFG is unavailable on Vulkan. XeMFG requires OptiScaler.
Compatibility varies by game; artifacts, freezes, black screens and crashes
remain possible.

If the menu does not appear, check the executable folder, chosen proxy name,
and any leftover older mod files. Fully close the game before changing files.

For frozen presentation, some users reported that **Preset B** helped in
[issue #6](https://github.com/dashdogy/RTX40MFG-Unlock/issues/6). Restart after
changing presets. This is a reported workaround, not a fix for every game.

For [bug reports](https://github.com/dashdogy/RTX40MFG-Unlock/issues), include
the game, GPU, driver, proxy filename (or OptiScaler plugin), multiplier and
preset, plus the matching `%TEMP%\RTXMFG-<game>-<path-hash>.log` and
`RTXMFG-Universal.status.json` beside the mod settings. Each launch keeps the
previous runtime log as `%TEMP%\RTXMFG-<game>-<path-hash>.previous.log` and
creates a new log. The status file is replaced on each launch.
With OptiScaler, also include `OptiScaler.log`.

## Uninstall

Close the game and remove the renamed MFG Unlocker DLL, or `MFGUnlocker.asi`
from OptiScaler's plugins folder. Restore any original DLL you renamed to a
`Hooked` filename. Keep `RTXMFG-Universal.json` if you want to retain your
settings.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third party components retain their respective licences and notices.
`MFGUnlocker.dll` also embeds XeSS Frame Generation program data derived from
Intel's XeSS SDK. That data is Intel's work, is not covered by the MIT License,
and remains subject to Intel's licence terms. See [NOTICE.md](NOTICE.md).
