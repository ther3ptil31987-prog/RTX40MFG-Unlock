"""Compile the XeFG tensor-core kernels (xess/cuda/xefg_cuda_layers.cu) and embed them as one RCDATA fatbin.

The fatbin holds SASS for sm_75 (RTX 20; m8n8k16 mma), sm_86 (RTX 30; also runs on RTX 40), sm_89
(RTX 40) and sm_120 (RTX 50), no PTX: the driver picks the matching SASS, and an architecture without one
keeps the DXIL port (the route requires compute capability 7.5 with tensor cores, 8.6, 8.9 or 12.0).
The compiler is the pinned CUDA Toolkit 13.2 nvcc, run in the pinned MSVC toolset's environment, and the output is pinned by SHA-256 (nvcc output is deterministic), so a
different compiler or an edited kernel fails the build instead of shipping silently. Re-validate with
tests/xess_cuda before updating the pin.
"""
from pathlib import Path
import argparse
import hashlib
import subprocess

PINNED_NVCC = "Cuda compilation tools, release 13.2, V13.2.51"
RESOURCE_ID = 7300  # XeSS programs use 7000..7054, RTXMFG's XeFG shaders 7100..
SOURCE = "xess/cuda/xefg_cuda_layers.cu"
KERNELS = ["L_aa016078", "L_198d6bf8", "L_dcd8becf", "L_efbe3788", "L_50f0c205", "L_a3c3caa1", "L_9481d452",
           "L_06e97933", "L_3ed03daf", "L_748b0baf", "L_cb391582", "L_fc1b8a04", "L_3459485", "L_3452733",
           "L_flow16_acc17", "L_3453044"]
PINNED_FATBIN = "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7"

parser = argparse.ArgumentParser()
parser.add_argument("--nvcc", type=Path, required=True, help="CUDA Toolkit 13.2 bin/nvcc.exe")
parser.add_argument("--vcvars", type=Path, required=True, help="VC/Auxiliary/Build/vcvars64.bat of the build's Visual Studio")
parser.add_argument("--vcvars-ver", required=True, help="MSVC toolset version (VCToolsVersion)")
parser.add_argument("--source", type=Path, required=True, help="source/native directory")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

version = subprocess.run([str(args.nvcc), "--version"], capture_output=True, text=True, check=True).stdout
if PINNED_NVCC not in version:
    raise SystemExit(f"Unpinned nvcc: {version!r} (expected {PINNED_NVCC!r})")

out = args.output / "xess_cuda"
out.mkdir(parents=True, exist_ok=True)
target = out / "xefg_cuda_layers.fatbin"
command = (f'call "{args.vcvars}" -vcvars_ver={args.vcvars_ver} >nul 2>nul && "{args.nvcc}" -fatbin -O3 '
           f'-gencode arch=compute_75,code=sm_75 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 '
           f'-gencode arch=compute_120,code=sm_120 '
           f'-o "{target}" "{args.source / SOURCE}"')
# One command-line string: cmd.exe /s strips only the outer quotes (a list would escape the inner ones).
# The compiler's output goes to stdout: the build script treats any stderr text as a failure.
compiled = subprocess.run(f'cmd.exe /d /s /c "{command}"', capture_output=True, text=True)
print((compiled.stdout + compiled.stderr).strip())
if compiled.returncode != 0:
    raise SystemExit(f"nvcc failed with exit {compiled.returncode}")
data = target.read_bytes()
digest = hashlib.sha256(data).hexdigest()
for kernel in KERNELS:
    if (kernel.encode() + b"\0") not in data:
        raise SystemExit(f"{kernel} missing from {target}")
if digest != PINNED_FATBIN:
    raise SystemExit(f"{SOURCE} compiled to {digest}, pinned {PINNED_FATBIN}; update the pin only after "
                     "tests/xess_cuda passes with this fatbin")

header = ['#pragma once', '#include "runtime_payload.h"', 'namespace xefg_cuda::embedded {',
          f'inline constexpr xess_payload::Asset fatbin{{{RESOURCE_ID}, "cuda/xefg_cuda_layers.fatbin", "{digest}", {len(data)}}};',
          '}']
rc = ['#include <windows.h>', 'LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL',
      f'{RESOURCE_ID} RCDATA "{target.resolve().as_posix()}"']
for name, lines in (("embedded_xefg_cuda.generated.h", header), ("embedded_xefg_cuda.rc", rc)):
    text = "\n".join(lines) + "\n"
    path = args.output / name
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
print(f"Compiled and embedded the XeFG tensor-core kernels ({len(KERNELS)}) with {PINNED_NVCC}")
