"""Compile the Vulkan host's input-copy compute shader (xess/vulkan/vk_input_copy.comp) into vk_input_copy_spv.h.

Built with the module's pinned glslangValidator (Vulkan SDK 1.2.176.1); every variant's SPIR-V is pinned below.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

GLSLANG_SHA256 = "8F0838D3FD981D4086D81675C4CFA4381691AEAFC3651D12D62F61EF0CEBB33E"
VARIANTS = {  # MFG_TARGET -> pinned SPIR-V SHA-256
    0: "F6F48F06D01134D9596B117E0C98B4A1A098325AB5B67268EEDEE8554955B7CA",
    1: "112A67E0DF42B09EDAB40BE77F505D58F5B2D3B357BDA64A45F145FB247CE0D3",
    2: "2E34C018E32F1A5FD819E93E2E46CF31C454312C8D3F53AE22D491D2E1470FBC",
}

parser = argparse.ArgumentParser()
parser.add_argument("--glslang", type=Path, required=True)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

if hashlib.sha256(args.glslang.read_bytes()).hexdigest().upper() != GLSLANG_SHA256:
    raise SystemExit("Expected glslangValidator from Vulkan SDK 1.2.176.1")
args.output.mkdir(parents=True, exist_ok=True)
header = ["#pragma once", "#include <cstdint>", "namespace xess_vulkan::input_copy {"]
for target, pinned in VARIANTS.items():
    spv = args.output / f"vk_input_copy_{target}.spv"
    subprocess.run([str(args.glslang), "-V", f"-DMFG_TARGET={target}", "-o", str(spv),
                    str(args.source / "xess" / "vulkan" / "vk_input_copy.comp")], check=True, stdout=subprocess.DEVNULL)
    raw = spv.read_bytes()
    digest = hashlib.sha256(raw).hexdigest().upper()
    if pinned is not None and digest != pinned:
        raise SystemExit(f"Input-copy shader {target} output drifted: {digest}")
    words = struct.unpack(f"<{len(raw) // 4}I", raw)
    header.append(f"inline constexpr uint32_t shader{target}[] = {{")
    for at in range(0, len(words), 8):
        header.append("    " + ",".join(f"0x{word:08x}u" for word in words[at:at + 8]) + ",")
    header.append("};")
    print(f"vk_input_copy variant {target}: {digest} ({len(raw)} bytes)")
header.append("}")
text = "\n".join(header) + "\n"
target_path = args.output / "vk_input_copy_spv.h"
if not target_path.exists() or target_path.read_text(encoding="ascii") != text:
    target_path.write_text(text, encoding="ascii")
