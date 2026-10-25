"""Embed the XeMFG-on-Vulkan SPIR-V program set (xvk) as RCDATA resources.

Each program is a pair <dxil-sha256>.spv + <dxil-sha256>.json (binding contract: BINDINGS.md of the research
workspace): SPIR-V 1.6 translated from the exact DXIL that XeFG hands to CreateComputePipelineState, proven
bit-exact against that DXIL on D3D12. The folder's SHA256SUMS.txt lists every file and is pinned below; only the
SPIR-V is embedded, the bindings become a compiled table. Intel's runtime DLLs are never shipped.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

MANIFEST_SHA256 = "fe53a14b1d2b7a86a94fe7bd11095ca9727cf0df2e9e7abd539244845622db1b"  # 83 programs (21 Oct: + 4 inverted depth; 22 Oct, presentqueue.1 #630: + 18 for XeFG's AMD selection and the 2x-model route)
FIRST_RESOURCE_ID = 7400  # XeSS programs 7000.., XeFG shaders 7100.., CUDA fatbin 7300
TYPES = {"STORAGE_BUFFER": 0, "UNIFORM_BUFFER": 1, "SAMPLED_IMAGE": 2, "STORAGE_IMAGE": 3, "SAMPLER": 4}

parser = argparse.ArgumentParser()
parser.add_argument("--programs", type=Path, required=True,
                    help="directory containing SHA256SUMS.txt and the <sha256>.spv/.json pairs")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

manifest_path = args.programs / "SHA256SUMS.txt"
manifest = manifest_path.read_bytes()
if hashlib.sha256(manifest).hexdigest() != MANIFEST_SHA256:
    raise SystemExit(f"Pinned xvk program manifest mismatch: {manifest_path} "
                     f"({hashlib.sha256(manifest).hexdigest()})")

listed = {}
for line in manifest.decode("ascii").splitlines():
    if not line.strip():
        continue
    digest, name = line.split(maxsplit=1)
    name = name.lstrip("*")
    if len(digest) != 64 or "/" in name or "\\" in name or name in listed:
        raise SystemExit(f"Malformed xvk manifest row: {line!r}")
    listed[name] = digest.lower()


def verified(name):
    path = args.programs / name
    data = path.read_bytes()
    if listed.get(name) != hashlib.sha256(data).hexdigest():
        raise SystemExit(f"xvk program file does not match the manifest: {path}")
    return path, data


programs = []
for name in sorted(listed):
    if not name.endswith(".json"):
        continue
    key = name[:-5]
    path, data = verified(name)
    meta = json.loads(data)
    if meta.get("sha256") != key or len(key) != 64 or any(c not in "0123456789abcdef" for c in key):
        raise SystemExit(f"xvk program metadata names another program: {path}")
    spv_path, spv = verified(key + ".spv")
    if len(spv) < 20 or len(spv) % 4 or struct.unpack_from("<I", spv)[0] != 0x07230203:
        raise SystemExit(f"Not a SPIR-V module: {spv_path}")
    bindings = []
    for b in meta.get("bindings", []):
        if b["type"] not in TYPES or not 0 <= int(b["binding"]) < 256:
            raise SystemExit(f"Unsupported binding in {path}: {b}")
        bindings.append((int(b["binding"]), TYPES[b["type"]]))
    if len({b for b, _ in bindings}) != len(bindings):
        raise SystemExit(f"Duplicate binding in {path}")
    push = int(meta.get("pushConstantBytes", 0))
    if push < 0 or push > 256 or push % 4:
        raise SystemExit(f"Push constant size out of range in {path}")
    programs.append((key, spv_path, listed[key + ".spv"], len(spv), push, bindings))
if not programs or len(programs) * 2 != len(listed):
    raise SystemExit("xvk manifest must list exactly one .spv and one .json per program")

header = ["#pragma once", '#include "xvk_embedded.h"', "namespace xvk::embedded {",
          f'inline constexpr char manifestSha256[] = "{MANIFEST_SHA256}";',
          "inline constexpr BindingRow bindings[] = {"]
rows = []
for index, program in enumerate(programs):
    programs[index] = program + (len(rows),)
    rows.extend(program[5])
header += [f"    {{{b}, {t}}}," for b, t in rows] or ["    {0, 0},"]
header += ["};", "inline constexpr ProgramAsset programs[] = {"]
rc = ["#include <windows.h>", "LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL"]
for index, (key, spv_path, spv_hash, size, push, bindings, first) in enumerate(programs):
    resource = FIRST_RESOURCE_ID + index
    rc.append(f'{resource} RCDATA "{spv_path.resolve().as_posix()}"')
    header.append(f'    {{"{key}", {resource}, "{spv_hash}", {size}, {push}, {first}, {len(bindings)}}},')
header += ["};", "}"]

args.output.mkdir(parents=True, exist_ok=True)
for name, lines in (("embedded_xvk_programs.generated.h", header), ("embedded_xvk_programs.rc", rc)):
    text = "\n".join(lines) + "\n"
    target = args.output / name
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        target.write_text(text, encoding="utf-8")
print(f"Embedded {len(programs)} verified xvk SPIR-V programs")
