"""Run the release harnesses in isolated directories with an exact DLL.

This does not launch or install into games. Windows presentation cases briefly
create test windows; Vulkan requires a compatible NVIDIA Vulkan device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


# xefgrace.1: the profiled XeSS SDK 3.0.2 pair (libxess_fg 1.3.1 / libxell 1.3.2), as OptiScaler 10 ships it.
XESS_PAIR = {'libxess_fg.dll': 'EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27',
             'libxell.dll': 'D2030DCD694FDA8F2EC7E044B13E6DB8F0B56D4BA9113A5EFAD334E3F3DED8C7'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def runtime_log(executable):
    normalized = str(executable).replace('/', '\\')
    normalized = ''.join(chr(ord(c)+32) if 'A' <= c <= 'Z' else c for c in normalized)
    identity = 14695981039346656037
    raw = normalized.encode('utf-16-le')
    for i in range(0, len(raw), 2):
        identity = ((identity ^ int.from_bytes(raw[i:i+2], 'little')) * 1099511628211) & 0xffffffffffffffff
    stem = executable.stem[:40].lower()
    stem = ''.join(c if c.isascii() and (c.isalnum() or c in '-_') else '_' for c in stem)
    # Beside the executable (each case runs from its own writable folder).
    return Path(executable).parent/f'RTXMFG-{stem}-{identity:016X}.log'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dll', type=Path, required=True)
    parser.add_argument('--harness-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provider', type=Path)
    parser.add_argument('--probe-wrapper', type=Path,
                        help='Exact local Streamline DLSS-G wrapper for the unload/reload regression')
    parser.add_argument('--xess-pair', type=Path,
                        help='Folder with the profiled XeSS SDK 3.0.2 libxess_fg.dll/libxell.dll for the XeFG route timing cases')
    parser.add_argument('--session-diagnostics', action='store_true',
                        help='Beta builds with MFG_UNLOCK_SESSION_DIAGNOSTICS: the crash and exit record case')
    parser.add_argument('--only', nargs='*')
    args = parser.parse_args()
    dll = args.dll.resolve(strict=True)
    harness = args.harness_dir.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = [(f'default-{mode}', 'StartupDefaultsTests', mode, 'winmm.dll')
             for mode in ('clean', 'legacy', 'fixed', 'dynamic', 'follow', 'environment')]
    cases += [('loader-version', 'ReleaseLoadCheck', 'loader', 'version.dll'),
              ('crash-reporter-host', 'ReleaseLoadCheck', 'crash-reporter', 'winmm.dll'),
              ('adapter-parent-menu','WarpPresentHarness','adapter-parent-menu','winmm.dll'),
              ('adapter-parent-coexist-menu','WarpPresentHarness','adapter-parent-coexist-menu','winmm.dll'),
              ('adapter-parent-unaligned-menu','WarpPresentHarness','adapter-parent-unaligned-menu','winmm.dll'),
              ('loader-winmm', 'ReleaseLoadCheck', 'loader', 'winmm.dll'),
              ('themida-imports', 'ThemidaImportHost', '', 'version.dll'),
              ('control', 'ControlResolverTests', 'control', 'winmm.dll'),
              ('control-locked', 'ControlResolverTests', 'control-locked', 'winmm.dll'),
              ('status-transport', 'StatusTransportTests', '', 'winmm.dll'),
              ('scoped-import', 'ScopedImportTests', '', 'winmm.dll'),
              ('scoped-factory', 'ScopedFactoryTests', '', 'winmm.dll'),
              ('factory-revision-menu', 'WarpPresentHarness', 'factory-revision-menu', 'winmm.dll'),
              ('factory-revision-present', 'WarpPresentHarness', 'factory-revision-present', 'winmm.dll'),
              ('factory-revision-unsupported', 'WarpPresentHarness', 'factory-revision-unsupported', 'winmm.dll'),
              ('unaligned-graphics-menu', 'WarpPresentHarness', 'unaligned-graphics-menu', 'winmm.dll'),
              ('rwx-unaligned-graphics-menu', 'WarpPresentHarness', 'rwx-unaligned-graphics-menu', 'winmm.dll'),
              ('unaligned-graphics-coexist-menu', 'WarpPresentHarness', 'unaligned-graphics-coexist-menu', 'winmm.dll'),
              ('policy', 'NvidiaPolicyTests', '', 'winmm.dll'),
              ('dynamic-availability', 'DynamicAvailabilityTests', '', 'winmm.dll'),
              ('vulkan-menu-status', 'VulkanMenuStatusTests', '', 'winmm.dll'),
              ('first-launch', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-zh', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-uk', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-it', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-sv', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-tr', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-vi', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('first-launch-fr', 'WarpPresentHarness', 'first-launch-menu', 'winmm.dll'),
              ('pinned-fps-menu', 'WarpPresentHarness', 'pinned-fps-menu', 'winmm.dll'),
              ('startup-menu', 'WarpPresentHarness', 'startup-menu', 'winmm.dll'),
              ('unaligned-menu', 'WarpPresentHarness', 'unaligned-menu', 'winmm.dll'),
              ('rwx-unaligned-menu', 'WarpPresentHarness', 'rwx-unaligned-menu', 'winmm.dll'),
              ('rwx-aligned-menu', 'WarpPresentHarness', 'rwx-aligned-menu', 'winmm.dll'),
              ('hdr-startup-menu', 'WarpPresentHarness', 'hdr-startup-menu', 'winmm.dll'),
              ('hdr-unsupported-menu', 'WarpPresentHarness', 'hdr-unsupported-menu', 'winmm.dll'),
              ('layered-menu', 'WarpPresentHarness', 'layered-slinit-menu', 'winmm.dll'),
              ('reshade-wrapped-menu', 'WarpPresentHarness', 'reshade-wrapped-menu', 'winmm.dll'),
              ('manual-upgrade-factory-menu', 'WarpPresentHarness', 'manual-upgrade-factory-menu', 'winmm.dll'),
              ('manual-upgrade-swapchain-passthrough', 'WarpPresentHarness', 'manual-upgrade-swapchain-passthrough', 'winmm.dll'),
              ('manual-upgrade-immediate-menu', 'WarpPresentHarness', 'manual-upgrade-immediate-menu', 'winmm.dll'),
              ('interposer-entry-menu', 'WarpPresentHarness', 'interposer-entry-menu', 'winmm.dll'),
              ('interposer-host-menu', 'WarpPresentHarness', 'interposer-host-menu', 'winmm.dll'),
              ('interposer-host-dynamic', 'WarpPresentHarness', 'interposer-host-dynamic', 'winmm.dll'),
              ('vsync-override-menu', 'WarpPresentHarness', 'vsync-override-menu', 'winmm.dll'),
              ('directinput-menu', 'WarpPresentHarness', 'directinput-menu', 'winmm.dll'),
              ('slowpump-menu', 'WarpPresentHarness', 'slowpump-menu', 'winmm.dll')]
    cases += [(name, 'EngineFactoryTests', name, 'winmm.dll') for name in
              ('engine-direct', 'engine-dynamic', 'engine-late', 'engine-middleware', 'engine-external', 'engine-conflict')]
    cases += [('engine-menu', 'WarpPresentHarness', 'engine-menu', 'winmm.dll')]
    cases += [(f'optiscaler-{mode}', 'OptiScalerHostTests', mode, 'winmm.dll') for mode in
              ('standalone', 'proxy', 'plugin', 'late', 'optin', 'hidden', 'shared', 'forza', 'forza-shared',
               'impostor', 'fakenvapi', 'streamline', 'second-streamline', 'streamline-device', 'streamline-warp')]
    cases += [(f'gpuname-{mode}', 'GpuNameStartupTests', mode, 'winmm.dll') for mode in ('off', 'on', 'invalid')]
    if args.session_diagnostics:
        cases.append(('session-diagnostics', 'SessionDiagnosticsTests', 'diagnostics', 'winmm.dll'))
    cases += [('small-stack-resolver', 'SmallStackResolverTests', 'small-stack', 'winmm.dll')]
    cases += [('log-retention', 'LogRetentionTests', 'retention', 'winmm.dll')]
    cases += [('interval-retention', 'LogRetentionTests', 'interval-retention', 'winmm.dll')]
    cases += [('log-location', 'LogRetentionTests', 'log-location', 'winmm.dll')]
    cases += [(name, executable, 'vulkan', 'winmm.dll') for name, executable in (
        ('vulkan-direct', 'VulkanMenuTests'), ('vulkan-dynamic', 'VulkanDynamicMenuTests'),
        ('vulkan-streamline', 'VulkanStreamlineMenuTests'), ('vulkan-mixed', 'VulkanMixedMenuTests'))]
    if args.provider:
        cases.append(('provider-gate', 'ProviderGateTests', 'provider', 'winmm.dll'))
    if args.probe_wrapper:
        args.probe_wrapper = args.probe_wrapper.resolve(strict=True)
        cases.append(('plugin-probe-lifetime', 'PluginProbeLifetimeTests', 'probe', 'winmm.dll'))
    if args.xess_pair:
        args.xess_pair = args.xess_pair.resolve(strict=True)
        for filename, expected in XESS_PAIR.items():
            if sha(args.xess_pair/filename) != expected:
                parser.error(f'{args.xess_pair/filename} is not the profiled SDK 3.0.2 {filename}')
        cases += [(f'optiscaler-{mode}', 'OptiScalerHostTests', mode, 'winmm.dll') for mode in ('xefg-proxy', 'xefg-plugin')]
        # xefgmenu.1: the menu on OptiScaler's XeFG output (D3D12 on the default adapter, a test window).
        cases += [(f'optiscaler-{mode}', 'OptiScalerHostTests', mode, 'winmm.dll') for mode in
                  ('xefg-output', 'xefg-output-optin', 'xefg-output-hidden', 'xefg-output-factory')]
    if args.only:
        unknown = set(args.only) - {case[0] for case in cases}
        if unknown:
            parser.error(f'Unknown or unavailable cases: {sorted(unknown)}')
        cases = [case for case in cases if case[0] in args.only]
    # First launches with a menu language saved: its font is built at the first frame
    # and the full menu draws in that language.
    languages = {'first-launch-zh': 'zh-CN', 'first-launch-uk': 'uk-UA',
                 'first-launch-it': 'it-IT', 'first-launch-sv': 'sv-SE',
                 'first-launch-tr': 'tr-TR', 'first-launch-vi': 'vi-VN',
                 'first-launch-fr': 'fr-FR'}
    # crashhost.1: the harness runs under a crash reporter's executable name.
    # optimenu.1: and under Forza Motorsport's for the ForzaTech default.
    host_names = {'crash-reporter-host': 'UnityCrashHandler64.exe',
                  'optiscaler-forza': 'forza_steamworks_release_final.exe',
                  'optiscaler-forza-shared': 'forza_steamworks_release_final.exe'}
    rows = []
    for name, executable, mode, proxy in cases:
        destination = Path(tempfile.mkdtemp(prefix=name+'-', dir=output))
        for filename in (executable+'.exe', 'sl.interposer.dll',
                         'ControlWrapperFixture.dll', 'ExistingOverlayFixture.dll', 'ScopedCallerFixture.dll', 'EngineFactoryFixture.dll',
                         'OptiScalerHostFixture.dll', 'FakeNvapiFixture.dll', 'MaximumClampFixture.dll',
                         'RendererHostFixture.dll', 'RendererDynamicFixture.dll'):
            shutil.copy2(harness/filename, destination/filename)
        for filename in ('LateEngineFixture.dll', 'sl.enginefixture.dll'):
            shutil.copy2(harness/'EngineFactoryFixture.dll', destination/filename)
        if (harness/'hotpatch'/'sl.interposer.dll').is_file():
            (destination/'hotpatch').mkdir()
            shutil.copy2(harness/'hotpatch'/'sl.interposer.dll', destination/'hotpatch'/'sl.interposer.dll')
        shutil.copy2(harness/'EngineFactoryFixture.dll', output/'ExternalEngineFixture.dll')
        candidate = destination/proxy
        shutil.copy2(dll, candidate)
        if mode.startswith('xefg-'):
            (destination/'OptiScaler').mkdir()
            for filename in XESS_PAIR:
                shutil.copy2(args.xess_pair/filename, destination/'OptiScaler'/filename)
        if name in languages:
            (destination/'RTX40MFG-UI.ini').write_text(f'[Overlay]\nLanguage={languages[name]}\n', encoding='utf-8')
        elif name == 'pinned-fps-menu':
            (destination/'RTX40MFG-UI.ini').write_text(
                '[Overlay]\nFirstLaunchMenuShown=1\nPinnedFps=1\n', encoding='utf-8')
        elif name != 'first-launch':
            (destination/'RTX40MFG-UI.ini').write_text(
                '[Overlay]\nFirstLaunchMenuShown=1\n', encoding='utf-8')
        provider_hash = sha(args.provider) if mode == 'provider' else None
        probe_hash = sha(args.probe_wrapper) if mode == 'probe' else None
        host = destination/host_names.get(name, executable+'.exe')
        if host.name != executable+'.exe':
            shutil.copy2(destination/(executable+'.exe'), host)
        command = [str(host)]
        if mode == 'vulkan':
            command += [str(candidate), str(destination)]
        elif mode == 'provider':
            command += [str(args.provider.resolve()), 'dll', str(candidate)]
        elif mode == 'probe':
            command += [str(candidate), str(args.probe_wrapper)]
        elif mode:
            command += [mode, str(candidate)]
        environment = dict(os.environ)
        for key in ('RTXMFG_SLOT_FAULT', 'RTX_MFG_ACTIVE_MULTIPLIER',
                    'RTX_MFG_CONFIG_PATH', 'RTX_MFG_STATUS_PATH'):
            environment.pop(key, None)
        def run(label, command):
            with (destination/(label+'.stdout.txt')).open('wb') as stdout, \
                 (destination/(label+'.stderr.txt')).open('wb') as stderr:
                process = subprocess.Popen(command, cwd=destination, env=environment,
                    stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    code = process.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    code = -1
            text = (destination/(label+'.stdout.txt')).read_text(encoding='utf-8', errors='replace')
            logfile = runtime_log(Path(command[0]))
            if logfile.is_file():
                shutil.copy2(logfile, destination/(label+'.runtime.log'))
            row = dict(name=label, pid=process.pid, exit=code,
                       passed=code == 0 and not any(line.startswith('FAIL ') for line in text.splitlines()),
                       assertions=sum(line.startswith('PASS ') for line in text.splitlines()),
                       dllSHA256=sha(candidate), harnessSHA256=sha(Path(command[0])), directory=str(destination))
            if label in languages:
                log = destination/(label+'.runtime.log')
                log = log.read_bytes().decode('utf-16' if log.read_bytes()[:2] == b'\xff\xfe' else 'utf-8', errors='replace') if log.is_file() else ''
                row['languageFontReady'] = f'UI language {languages[label]}: font ready' in log
                row['passed'] &= row['languageFontReady']
            if provider_hash:
                row['providerSHA256'] = provider_hash
                row['providerUnchanged'] = sha(args.provider) == provider_hash
                row['passed'] &= row['providerUnchanged']
            if probe_hash:
                row['probeWrapperPath'] = str(args.probe_wrapper)
                row['probeWrapperSHA256'] = probe_hash
                row['probeWrapperUnchanged'] = sha(args.probe_wrapper) == probe_hash
                row['passed'] &= row['probeWrapperUnchanged']
            rows.append(row)
            print(json.dumps(row), flush=True)
            (output/'results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
            if not row['passed']:
                raise SystemExit(f'Failed {label}: see {destination}')
        run(name, command)
        if name == 'first-launch':
            # Same executable directory and persisted marker, new process.
            run('first-launch-restart', [command[0], 'startup-menu', str(candidate)])
    print(f'COMPLETE cases={len(rows)} assertions={sum(row["assertions"] for row in rows)}')


if __name__ == '__main__':
    main()
