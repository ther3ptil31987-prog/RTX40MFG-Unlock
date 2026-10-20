"""Compile RTXMFG's own XeFG replacement shaders and embed them as RCDATA resources (splat.1, unet.1).

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
FLAGS = ["-nologo", "-E", "main", "-O3", "-Qstrip_reflect", "-Qstrip_debug"]
# name, source (relative to source/native), shader target, extra defines, pinned SHA-256 of the compiled container
UNET = "xess/port/unet_conv.hlsl"
SHADERS = [
    ("splat_motion_aware", "xess/port/splat_motion_aware.hlsl", "cs_6_0", [],
     "43ccfe670409fd807b604b870674012958fbb4067305a449304ee91ab6154f3f"),
    # splat.2: the inverted-depth variant (OptiScaler's XeFG output starts XeFG with INVERTED_DEPTH).
    ("splat_motion_aware_inverted", "xess/port/splat_motion_aware.hlsl", "cs_6_0", ["-DSPLAT_INVERTED_DEPTH=1"],
     "6c9bf4b59956c7fec3d764ffe6520e7aec3e6137553b9fca0ba65ecdd7642ce8"),
    # unet.1: five of XeFG's U-Net conv programs (decoders 3ed03daf / 748b0baf / cb391582, pooled encoders
    # aa016078 / efbe3788); FOOT_W x FOOT_H is the replaced program's group footprint.
    ("unet_3ed03daf", UNET, "cs_6_6", ["-DDECODER=1", "-DIN=1", "-DOUT=1", "-DFOOT_W=16", "-DFOOT_H=32", "-DPY=2"],
     "9900c30febf95bfa81b4da1178e5773863116beba4d3f032897da4254b6d8926"),
    ("unet_748b0baf", UNET, "cs_6_6", ["-DDECODER=1", "-DIN=4", "-DOUT=4", "-DFOOT_W=8", "-DFOOT_H=16", "-DPY=2"],
     "e78063f97ad2c0913412f07589cd17d0f2a6ebeeb6edd70647a69cca6b6a02bf"),
    ("unet_cb391582", UNET, "cs_6_6", ["-DDECODER=1", "-DIN=2", "-DOUT=2", "-DFOOT_W=32", "-DFOOT_H=8", "-DPY=2"],
     "7a29fbf311cd8dee5b8c1cfdadf31ee983092a512cf9c10845a079b457eb62a9"),
    ("unet_aa016078", UNET, "cs_6_6", ["-DPOOL=1", "-DBIAS=1", "-DIN=1", "-DOUT=1", "-DFOOT_W=16", "-DFOOT_H=16", "-DPY=2"],
     "ce847b58086ce7e9d25f7d8b90e989247cd4582226506d9552112f1ef4130643"),
    ("unet_efbe3788", UNET, "cs_6_6", ["-DPOOL=1", "-DIN=4", "-DOUT=8", "-DFOOT_W=8", "-DFOOT_H=4"],
     "efa8da5ef5f90c62b789b0fed8e77e2ce043e558d164cc552ca7fb9127cedb00"),
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
for name, source, profile, defines, pinned in SHADERS:
    target = out / f"{name}.dxil"
    subprocess.run([str(args.dxc), "-T", profile, *FLAGS, *defines, "-Fo", str(target), str(args.source / source)], check=True)
    data = target.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned or data[:4] != b"DXBC":
        raise SystemExit(f"{source} ({name}) compiled to {digest}, pinned {pinned}; update the pin only with a "
                         "re-validated shader (devtests splat_program_equivalence / unet_program_equivalence, tests/xess_cuda)")
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
