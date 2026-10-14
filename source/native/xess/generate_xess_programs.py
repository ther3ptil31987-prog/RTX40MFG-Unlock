"""Embed the 54 portable XeSS MFG DXIL programs as RCDATA resources.

Only our ported programs are embedded. Intel's runtime DLLs are never shipped.
The program manifest is pinned; every program must match its manifest row.
"""
from pathlib import Path
import argparse
import hashlib

MANIFEST_SHA256 = "d74bd783d29280ed372653701ce9b0395362e381faace1213206e0bdd9bfc9e5"
FIRST_RESOURCE_ID = 7000
PROGRAM_COUNT = 54

parser = argparse.ArgumentParser()
parser.add_argument("--programs", type=Path, required=True,
                    help="directory containing programs/programs.tsv and programs/*.dxil")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

manifest_path = args.programs / "programs" / "programs.tsv"
manifest = manifest_path.read_bytes()
if hashlib.sha256(manifest).hexdigest() != MANIFEST_SHA256:
    raise SystemExit(f"Pinned XeSS program manifest mismatch: {manifest_path}")

assets = [("programs/programs.tsv", manifest_path, MANIFEST_SHA256, len(manifest))]
for line in manifest.decode("ascii").splitlines():
    key, digest = line.split("\t")
    if not key.isdigit() or len(digest) != 64:
        raise SystemExit(f"Malformed program manifest row: {line!r}")
    path = args.programs / "programs" / f"{key}.dxil"
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest or data[:4] != b"DXBC":
        raise SystemExit(f"XeSS program does not match its manifest row: {path}")
    assets.append((f"programs/{key}.dxil", path, digest, len(data)))
if len(assets) != PROGRAM_COUNT + 1:
    raise SystemExit(f"Expected {PROGRAM_COUNT} programs, found {len(assets) - 1}")

header = ['#pragma once', '#include "runtime_payload.h"',
          'namespace xess_payload::embedded {', 'inline constexpr Asset assets[] = {']
rc = ['#include <windows.h>', 'LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL']
for index, (name, path, digest, size) in enumerate(sorted(assets), FIRST_RESOURCE_ID):
    header.append(f'    {{{index}, "{name}", "{digest}", {size}}},')
    rc.append(f'{index} RCDATA "{path.resolve().as_posix()}"')
header += ['};', '}']

args.output.mkdir(parents=True, exist_ok=True)
for name, lines in (("embedded_xess_programs.generated.h", header), ("embedded_xess_programs.rc", rc)):
    text = "\n".join(lines) + "\n"
    target = args.output / name
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        target.write_text(text, encoding="utf-8")
print(f"Embedded {len(assets) - 1} verified XeSS programs")
