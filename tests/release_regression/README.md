# Release regression harnesses

Use the pinned Windows x64 tools and SDK inputs in [BUILD.md](../../BUILD.md).
Build the release DLL first, then configure this separate project:

```powershell
cmake -S tests/release_regression -B build/release-tests `
    -G 'Visual Studio 17 2022' -A 'x64,version=10.0.22621.0' -T v143 `
    '-DCMAKE_VS_GLOBALS=VCToolsVersion=14.38.33130' `
    '-DSTREAMLINE_ROOT=C:/SDKs/streamline-sdk-v2.14.1' `
    '-DIMGUI_ROOT=C:/SDKs/reshade-6.8.0/deps/imgui' `
    '-DVULKAN_INCLUDE_DIR=C:/VulkanSDK/1.2.176.1/Include' `
    '-DVULKAN_LIBRARY=C:/VulkanSDK/1.2.176.1/Lib/vulkan-1.lib'
cmake --build build/release-tests --config Release --parallel 6
python tests/release_regression/run.py `
    --dll build/v1.3.3-hotfix.2/Release/RTXMFG.dll `
    --harness-dir build/release-tests/Release `
    --output build/release-test-results `
    --provider C:/Fixtures/nvngx_dlssg.dll
```

Supply an identified, supported DLSS-G provider for `--provider`. Its CPU
count/index gate is exercised using the normal publisher in the exact release
DLL. The runner verifies that the provider file remains unchanged. Without this
argument, that case is skipped and provider gate validation is incomplete.

The runner creates a separate directory per case, copies the exact DLL and
required fixtures, captures exit codes and hashes, and stops on failure.
`--only` accepts case names from `run.py` for focused reruns. It does not launch
games or replace installed files. WARP menu cases briefly use a foreground test
window; Vulkan cases require a compatible NVIDIA Vulkan device. Missing debug
or validation layers are reported in the individual logs.

Coverage includes Dynamic availability for legacy runtimes and range limits,
clean/default and saved controls, renamed loader forwarding
and duplicate ownership, observed wrapper controls, NVIDIA policy limits,
first-launch menu pixels and persistence across relaunch, WARP presentation and
layered swapchains, and Vulkan direct/dynamic/internal-Streamline/mixed routes.
Hotfix coverage also includes unaligned DXGI factory imports, native adapter
parent-factory returns, coexistence with existing hooks, writable/executable import tables,
HDR interface forwarding, locked status files and process identity, renderer DLL
imports and late loading, 64 KiB/128 KiB resolver threads, and log/status reuse
across launches. The first-launch case reuses its settings directory in a second process to
verify that the automatic menu does not reopen.

`factory-revision-menu` loads the exact DLL and reproduces Witcher 3's base
`IDXGIFactory` creation followed by `EnumAdapters1`, `IsCurrent` and newer
swapchain creation without requesting those factory revisions first. It also
checks the existing WARP menu, presentation and teardown behavior.
The supported-path fixture requests `IID_IDXGIFactory` through
`CreateDXGIFactory1`; `factory-revision-unsupported` uses the older
`CreateDXGIFactory` API, whose factory rejects `IDXGIFactory1`, and checks
safe error returns and proxy retirement in the exact DLL.
`factory-revision-present` exercises the same factory calls and WARP
presentation/teardown without the foreground-dependent keyboard assertions.
`interposer-entry-menu` reproduces Where Winds Meet's packed executable. It
reads the interposer's `CreateDXGIFactory*` exports from the export table
after a non-manual `slInit`, with no import slot or `GetProcAddress` call.
Executable calls must return exactly one menu factory proxy, including after
a second `slInit`. A foreign DLL calling the same exported entry must stay
native. The swapchain must then present, open and close the menu.
`rwx-aligned-menu` reproduces the Microsoft Store MSFS 2024 executable: its
eight-byte-aligned `USER32` import slots sit in a data page that becomes
executable after startup. The menu must open with game-only input suppression
through public-entry detours, leaving the slot bytes and page protection as found.
Run `FactoryRevisionTests.exe` directly from the Release harness directory for
the source-level supported/unsupported revision, lazy-query and concurrent
cache-lifetime cases. This target compiles the production proxy with isolated
renderer/coordinator stubs; it does not exercise the release DLL's backend.

These tests establish synthetic rendering, forwarding and CPU gate behavior.
They do not establish live DLSS-G provider Evaluate output or ordered, distinct
generated frames in any game. RTX 30 GPU execution remains separately unverified.
