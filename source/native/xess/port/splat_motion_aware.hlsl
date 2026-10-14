// splat.1: RTXMFG's own replacement for XeFG's motion-vector splat program (native program,
// SHA-256 3f3f91d2..., in libxess_fg 3.0.2 ec5e0c65), run once per generated frame.
//
// Same work as the replaced program:
// - Each source pixel moves by its motion vector times `scale`.
// - It writes key = depth key << 16 | half bits of the displacement into a 2x2 destination
//   footprint, with an unsigned atomic max per entry (two planes of dst W*H uints: x, then y).
// - The per-pixel math (validity, rounding, key bits) is the same operation for operation.
//
// Only the group's shared tile placement differs:
// - The replaced program centres a 16x24 tile on the group's own pixels, so any splat that
//   moves more than ~4 px becomes a global atomic.
// - Here the tile covers the bounding box of the group's targets, so camera motion stays in
//   shared memory. Groups whose box does not fit use a tile centred on their own pixels.
//
// An unsigned max is order independent, so the destination buffer is byte-identical
// (tests/xess_backend/splat_program_tests.cpp). The root signature matches the replaced
// program: t0, t1 and u0 tables, then 10 root constants at b0.
// splat.2: compiled a second time with SPLAT_INVERTED_DEPTH=1 for the program's inverted-depth
// variant (SHA-256 c3649191..., the one OptiScaler's XeFG uses).
#define SPLAT_ROOT_SIGNATURE \
    "DescriptorTable(SRV(t0)), DescriptorTable(SRV(t1)), DescriptorTable(UAV(u0)), " \
    "RootConstants(num32BitConstants = 10, b0)"
#define TILE_W 40
#define TILE_H 48

cbuffer Constants : register(b0)
{
    uint2 srcSize;
    uint2 dstSize;
    float2 scale;
    int2 motionOffset;
    int2 depthOffset;
};
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWByteAddressBuffer Target : register(u0);

groupshared uint tileX[TILE_W * TILE_H];
groupshared uint tileY[TILE_W * TILE_H];
groupshared int boxMinX, boxMinY, boxMaxX, boxMaxY;

// The program's float -> half packing (round half up on the magnitude), integer ops only.
uint PackHalf(float value)
{
    const uint b = asuint(value) + 4096u;
    const uint e8 = b >> 23; // exponent with the sign bit above it
    const uint e = e8 & 255u;
    const uint m = b & 8388607u;
    const uint sign = (b >> 16) & 32768u;
    const uint normal = e > 112u ? (((e8 << 10) + 16384u) & 31744u) | (m >> 13) : 0u;
    const uint denormal = (e - 102u) < 11u ? (((m + 8384512u) >> ((125u - e8) & 31u)) + 1u) >> 1 : 0u;
    const uint overflow = e > 143u ? 32767u : 0u;
    return sign | overflow | normal | denormal;
}

[RootSignature(SPLAT_ROOT_SIGNATURE)]
[numthreads(8, 16, 1)]
void main(uint3 group : SV_GroupID, uint3 pixel : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index == 0)
    {
        boxMinX = 0x7fffffff; boxMinY = 0x7fffffff; boxMaxX = -0x7fffffff; boxMaxY = -0x7fffffff;
    }
    for (uint i = index; i < TILE_W * TILE_H; i += 128u) { tileX[i] = 0u; tileY[i] = 0u; }

    bool valid = false;
    int tx = 0, ty = 0;
    uint keyX = 0u, keyY = 0u;
    if (pixel.x < srcSize.x && pixel.y < srcSize.y)
    {
        const float2 mv = Motion.Load(int3(int(motionOffset.x + pixel.x), int(motionOffset.y + pixel.y), 0));
#if SPLAT_INVERTED_DEPTH
        // splat.2: the inverted-depth specialisation (XeFG init flag INVERTED_DEPTH, OptiScaler's default).
        // Valid pixels flip their depth, then the key is built after the branch from 1 - depth, as the
        // replaced program does; the two subtractions stay in separate blocks like the original's.
        float z = Depth.Load(int3(int(depthOffset.x + pixel.x), int(depthOffset.y + pixel.y), 0));
        float keyMotionX = mv.x, keyMotionY = mv.y;
#else
        const float z = Depth.Load(int3(int(depthOffset.x + pixel.x), int(depthOffset.y + pixel.y), 0));
#endif
        if (isfinite(mv.x) && isfinite(mv.y))
        {
            const float dx = scale.x * mv.x;
            const float dy = scale.y * mv.y;
            valid = true;
            if (srcSize.x == dstSize.x && srcSize.y == dstSize.y)
            {
                tx = int(floor(dx + 0.5f)) + int(pixel.x);
                ty = int(floor(dy + 0.5f)) + int(pixel.y);
            }
            else
            {
                tx = int(round(((float(pixel.x) + 0.5f) + dx) * float(dstSize.x) / float(srcSize.x)));
                ty = int(round(((float(pixel.y) + 0.5f) + dy) * float(dstSize.y) / float(srcSize.y)));
            }
#if SPLAT_INVERTED_DEPTH
            z = 1.0f - z;
            keyMotionX = dx;
            keyMotionY = dy;
#else
            const uint depthKey = min(uint((1.0f - z) * 32767.0f), 32767u) << 16;
            keyX = PackHalf(dx) | depthKey | 0x80000000u;
            keyY = PackHalf(dy) | depthKey | 0x80000000u;
#endif
        }
#if SPLAT_INVERTED_DEPTH
        const uint depthKey = min(uint((1.0f - z) * 32767.0f), 32767u) << 16;
        keyX = PackHalf(keyMotionX) | depthKey | 0x80000000u;
        keyY = PackHalf(keyMotionY) | depthKey | 0x80000000u;
#endif
    }
    GroupMemoryBarrierWithGroupSync();
    // The footprint (tx..tx+1, ty..ty+1) clipped to the destination; box over the clipped taps.
    const int x0 = max(tx, 0), x1 = min(tx + 1, int(dstSize.x) - 1);
    const int y0 = max(ty, 0), y1 = min(ty + 1, int(dstSize.y) - 1);
    const bool any = valid && x0 <= x1 && y0 <= y1;
    const int waveX0 = WaveActiveMin(any ? x0 : 0x7fffffff), waveY0 = WaveActiveMin(any ? y0 : 0x7fffffff);
    const int waveX1 = WaveActiveMax(any ? x1 : -0x7fffffff), waveY1 = WaveActiveMax(any ? y1 : -0x7fffffff);
    if (WaveIsFirstLane()) // lanes without taps contribute the neutral values
    {
        InterlockedMin(boxMinX, waveX0); InterlockedMax(boxMaxX, waveX1);
        InterlockedMin(boxMinY, waveY0); InterlockedMax(boxMaxY, waveY1);
    }
    GroupMemoryBarrierWithGroupSync();
    const int bx0 = boxMinX, by0 = boxMinY, bx1 = boxMaxX, by1 = boxMaxY;
    if (bx1 < bx0) return; // no taps in this group; uniform, so every thread returns
    const bool fits = bx1 - bx0 < TILE_W && by1 - by0 < TILE_H;
    const int ox = fits ? bx0 : int(group.x * 8u) - (TILE_W - 8) / 2;
    const int oy = fits ? by0 : int(group.y * 16u) - (TILE_H - 16) / 2;
    if (any)
    {
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const int x = tx + int(k & 1u), y = ty + int((k >> 1) & 1u);
            if (x < 0 || y < 0 || x >= int(dstSize.x) || y >= int(dstSize.y)) continue;
            const int lx = x - ox, ly = y - oy;
            if (lx >= 0 && ly >= 0 && lx < TILE_W && ly < TILE_H)
            {
                InterlockedMax(tileX[ly * TILE_W + lx], keyX);
                InterlockedMax(tileY[ly * TILE_W + lx], keyY);
            }
            else
            {
                uint ignored;
                Target.InterlockedMax(uint(y * int(dstSize.x) + x) * 4u, keyX, ignored);
                Target.InterlockedMax(uint((y + int(dstSize.y)) * int(dstSize.x) + x) * 4u, keyY, ignored);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Flush the written entries; when the box fits, only the box can hold any.
    const int w = fits ? bx1 - bx0 + 1 : TILE_W, h = fits ? by1 - by0 + 1 : TILE_H;
    for (int e = int(index); e < w * h; e += 128)
    {
        const int lx = e % w, ly = e / w;
        const uint vx = tileX[ly * TILE_W + lx];
        if ((vx & 0x80000000u) == 0u) continue;
        const int x = ox + lx, y = oy + ly;
        if (x < 0 || y < 0 || x >= int(dstSize.x) || y >= int(dstSize.y)) continue;
        uint ignored;
        Target.InterlockedMax(uint(y * int(dstSize.x) + x) * 4u, vx, ignored);
        Target.InterlockedMax(uint((y + int(dstSize.y)) * int(dstSize.x) + x) * 4u, tileY[ly * TILE_W + lx], ignored);
    }
}
