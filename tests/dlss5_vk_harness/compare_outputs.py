"""vkdlss5.1: compares the DLSS outputs the Vulkan harness and its D3D12 twin dumped (output-<phase>.rgba16f, 2560x1440
RGBA16F, rows packed). Prints per phase: mean and max absolute difference over RGB, the share of channels differing by
more than 2e-3, and whether the two are bit-identical. Usage: compare_outputs.py <vulkan run> <d3d12 run> [phase ...]
"""
import array
import math
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT = 2560, 1440


def load(path):
    data = path.read_bytes()
    if len(data) != WIDTH * HEIGHT * 8:
        raise SystemExit(f"{path}: {len(data)} bytes, expected {WIDTH * HEIGHT * 8}")
    halfs = array.array("H", data)
    return data, [struct.unpack("<e", struct.pack("<H", h))[0] for h in halfs]


vulkan, d3d12 = Path(sys.argv[1]), Path(sys.argv[2])
phases = sys.argv[3:] or ["a-off", "b-on"]
for phase in phases:
    raw_v, v = load(vulkan / f"output-{phase}.rgba16f")
    raw_d, d = load(d3d12 / f"output-{phase}.rgba16f")
    total = mx = 0.0
    changed = count = 0
    for i in range(0, len(v), 4):
        for c in range(3):
            x, y = v[i + c], d[i + c]
            if not (math.isfinite(x) and math.isfinite(y)):
                continue
            diff = abs(x - y)
            total += diff
            mx = max(mx, diff)
            changed += diff > 2e-3
            count += 1
    print(f"{phase}: identical={raw_v == raw_d} meanAbs={total / max(count, 1):.6f} maxAbs={mx:.4f} "
          f"differing={100.0 * changed / max(count, 1):.3f}%")
