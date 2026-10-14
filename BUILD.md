# Building v1.5.0

The v1.5.0 release combines the DLSS MFG backend, the built-in menu, RTX 30
support and XeSS Multi Frame Generation (XeMFG) through OptiScaler in one
DLL. ProductVersion is 1.5.0; FileVersion is 1.5.0.933.

The release target is `RTXMFGUnified`, producing `Release/MFGUnlocker.dll`.
Use Windows x64, Visual Studio 2022/MSVC 14.38.33130, Windows SDK
10.0.22621.0 (including its `dxc.exe`), CMake 4.3.3, and Python 3.10 or newer.

Required external inputs:

- Streamline SDK **2.14.1**.
- Dear ImGui revision `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`
  from ReShade 6.8.0, including its backends.
- Vulkan SDK 1.2.176.1 headers and `glslangValidator.exe`.
- XeSS SDK **3.0.2** public headers (`inc/xess_fg`, `inc/xell`).
- The pinned XeSS Frame Generation program set: `programs/programs.tsv`
  (SHA-256 `D74BD783D29280ED372653701CE9B0395362E381FAACE1213206E0BDD9BFC9E5`)
  and the 54 DXIL programs it lists. Most are derived from Intel's XeSS SDK;
  see [NOTICE.md](NOTICE.md). They are a separately prepared build input and are
  not included in this source tree.
- The validated SM86 kernel cache, with this directory layout:

```text
native-cache/ampere_native_manifest.inc
native-cache/manifest.json
native-cache/RTX30MFG-Kernels/*.fatbin
native-cache-3109/ampere_native_manifest.inc
native-cache-3109/manifest.json
native-cache-3109/RTX30MFG-Kernels/*.fatbin
all-provider-layouts/native-cache-complete/ampere_native_manifest_additional.inc
all-provider-layouts/native-cache-complete/additional-resources.json
all-provider-layouts/native-cache-complete/RTX30MFG-Kernels/*.fatbin
```

The complete-program cache includes the DLSS Frame Generation 310.9.16 program
(additional program 10, resources 5200-5225).

Kernel caches and the XeSS program set are separately prepared build inputs. A
source checkout alone cannot reproduce the DLL without them. The build script
pins the tools, SDK headers, program set and manifests by SHA-256; CMake also
verifies every embedded kernel's and program's size and hash. No provider DLL is
modified on disk.

With those inputs available, run from the repository root, supplying their
actual paths and a fresh build directory:

```powershell
./source/native/build.ps1 `
    -StreamlineRoot 'C:/SDKs/streamline-sdk-v2.14.1' `
    -ImGuiRoot 'C:/SDKs/reshade-6.8.0/deps/imgui' `
    -NativeCacheRoot 'C:/BuildInputs/rtxmfg-sm86' `
    -XessSdkRoot 'C:/SDKs/xess-sdk-3.0.2' `
    -XessProgramsRoot 'C:/BuildInputs/xess-programs' `
    -BuildDirectory "$PWD/build/v1.5.0" `
    -EnableNgxCreateResultDiagnostics
```

This reproduces the release configuration: the built-in menu and bounded hook
installation are enabled, with the existing bounded NGX Create result log.
GPU fault capture, menu suppression, skipped GPU work and test fault injection
remain disabled. Do not enable those diagnostic switches for a release build.

The proxy dispatchers, Vulkan shader arrays, adapted ImGui DX12 backend, the
embedded XeSS program table and MFG Unlocker's own XeFG shaders
(`source/native/xess/port/splat_motion_aware.hlsl`, compiled with the Windows
SDK's `dxc.exe`) are generated in the build directory. The proxy and shader
outputs' normalized SHA-256 values must match
`source/native/generated_sha256.json`; the XeSS generators pin their own inputs
and outputs. The fixed export map is recorded in `proxy_exports.txt`; generation
does not scan DLLs on the build machine. Third-party notices, including the
XeSS program-data notice, remain embedded in the release DLL.

The Witcher 3 hair converter header is generated from
`source/native/witcher_dots/converter.hlsl` and `converter_source.h.in`.
Generated headers and runtime shader caches are not source inputs to publish.
The hair code discovery in `source/native/witcher_dots/game_discovery.cpp`
decodes instructions with the hde64 decoder bundled with MinHook in
`source/native/third_party/minhook`; it needs no additional build input.

Test projects:

- `tests/release_regression`: menu, loader, control, NVIDIA policy and provider
  gate harnesses. See its [README](tests/release_regression/README.md). They
  must load the freshly built DLL. The Vulkan cases use a real Vulkan device;
  DirectX 12 presentation cases use WARP. They exercise synthetic presentation
  and control paths, not a game's Frame Generation output.
- `tests/provider_310916`: DLSS Frame Generation 310.9.16 (Preset C) admission
  audits against the 310.9.0 and 310.9.16 providers, mapped without running
  provider code. Its RTX 30 audit is built only when configured with the same
  `MFG_AMPERE_*` SM86 cache arguments as the release, and checks that the
  complete 26-slot native program is selected.
- `tests/ampere_font` and `tests/ampere_runtime`: RTX 30 publication, selection
  and policy checks with data-only provider images and mock CUDA calls.

These checks do not establish live provider execution or correct final
presentation in a game.
