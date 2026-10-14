"""Compile RTXMFG's own XeFG replacement shaders and embed them as RCDATA resources (splat.1).

These are our HLSL sources only; no Intel program is read or embedded here. The compiler is the
pinned Windows SDK 10.0.22621.0 dxc (with its dxil.dll signer), and every output is pinned by
SHA-256, so a different compiler or an edited source fails the build instead of shipping silently.
"""
from pathlib import Path
import argparse
import hashlib
import subprocess

PINNED_DXC = "dxcompiler.dll: 1.6 - 1.6.2112.16 (e8295973c); dxil.dll: 1.6(101.6.2112.13)"
FIRST_RESOURCE_ID = 7100  # the 54 portable programs use 7000..7054
FLAGS = ["-nologo", "-T", "cs_6_0", "-E", "main", "-O3", "-Qstrip_reflect", "-Qstrip_debug"]
# name, source (relative to source/native), extra defines, pinned SHA-256 of the compiled container
SHADERS = [
    ("splat_motion_aware", "xess/port/splat_motion_aware.hlsl", [],
     "43ccfe670409fd807b604b870674012958fbb4067305a449304ee91ab6154f3f"),
    # splat.2: the inverted-depth variant (OptiScaler's XeFG output starts XeFG with INVERTED_DEPTH).
    ("splat_motion_aware_inverted", "xess/port/splat_motion_aware.hlsl", ["-DSPLAT_INVERTED_DEPTH=1"],
     "6c9bf4b59956c7fec3d764ffe6520e7aec3e6137553b9fca0ba65ecdd7642ce8"),
]

parser = argparse.ArgumentParser()
parser.add_argument("--dxc", type=Path, required=True, help="Windows SDK 10.0.22621.0 x64 dxc.exe")
parser.add_argument("--source", type=Path, required=True, help="source/native directory")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

if not (args.dxc.parent / "dxil.dll").is_file():
    raise SystemExit(f"dxil.dll (DXIL signer) missing beside {args.dxc}")
version = subprocess.run([str(args.dxc), "--version"], capture_output=True, text=True, check=True).stdout.strip()
if version != PINNED_DXC:
    raise SystemExit(f"Unpinned dxc: {version!r} (expected {PINNED_DXC!r})")

out = args.output / "xess_shaders"
out.mkdir(parents=True, exist_ok=True)
assets = []
for name, source, defines, pinned in SHADERS:
    target = out / f"{name}.dxil"
    subprocess.run([str(args.dxc), *FLAGS, *defines, "-Fo", str(target), str(args.source / source)], check=True)
    data = target.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned or data[:4] != b"DXBC":
        raise SystemExit(f"{source} compiled to {digest}, pinned {pinned}; update the pin only with a "
                         "re-validated shader (tests/xess_backend/splat_program_tests.cpp)")
    assets.append((f"shaders/{name}.dxil", target, digest, len(data)))

header = ['#pragma once', '#include "runtime_payload.h"',
          'namespace xess_payload::embedded {', 'inline constexpr Asset shaders[] = {']
rc = ['#include <windows.h>', 'LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL']
for index, (name, path, digest, size) in enumerate(assets, FIRST_RESOURCE_ID):
    header.append(f'    {{{index}, "{name}", "{digest}", {size}}},')
    rc.append(f'{index} RCDATA "{path.resolve().as_posix()}"')
header += ['};', '}']
for name, lines in (("embedded_xess_shaders.generated.h", header), ("embedded_xess_shaders.rc", rc)):
    text = "\n".join(lines) + "\n"
    target = args.output / name
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        target.write_text(text, encoding="utf-8")
print(f"Compiled and embedded {len(assets)} RTXMFG XeFG shader(s) with {PINNED_DXC}")
