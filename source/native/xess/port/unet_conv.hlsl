// unet.1: RTXMFG's own replacements for five of XeFG's U-Net conv programs (native programs in libxess_fg 3.0.2
// ec5e0c65, all with the same root signature): the decoder layers 3ed03daf / 748b0baf / cb391582 and the pooled
// encoder layers aa016078 / efbe3788. Each is compiled from this file with its own defines
// (xess/generate_xess_shaders.py) and swapped in by ShaderPort only for the exact program it replaces.
//
// Same work as the replaced program, for the Dispatch XeFG issues for it (the group footprint is Intel's):
// - conv3x3 over IN input slices of 16 int8 channels -> OUT output slices of 16, int32 accumulation;
// - POOL: the int32 maximum of the 2x2 conv positions of each output position;
// - q = trunc(float(acc) * scale + bias) (the same fmul + fadd as Intel's program), clamped to LO..127, one byte per
//   channel, a uint4 per (position, slice);
// - BIAS (aa016078): every input word + 0x80808080 as one 32-bit add;
// - DECODER: each input slice is T = clamp(skip + up2x(low), 0, 127) with Intel's integer bilinear 9/3/3/1 (sdiv 16);
//   positions outside the root constants' W x H read 0;
// - every position of the Dispatch footprint is written; out-of-range loads read 0 and out-of-range stores are dropped.
// Integer accumulation does not depend on the order, and the float epilogue is the same operation sequence, so the
// output is byte-identical (tests/xess_cuda; devtests unet_program_equivalence).
//
// Only the work split differs: Intel's decoders run 4 positions per thread and read the weights from the buffer;
// here each thread computes PY positions of one output slice, and the input window and the current input slice's
// weights for all OUT slices are staged in groupshared memory.
#define UNET_ROOT_SIGNATURE \
    "DescriptorTable(SRV(t0), SRV(t4), UAV(u0)), DescriptorTable(SRV(t1), SRV(t2), SRV(t3)), " \
    "RootConstants(num32BitConstants=4, b0), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, " \
    "addressW=TEXTURE_ADDRESS_WRAP, mipLODBias=0.0, maxAnisotropy=16, comparisonFunc=COMPARISON_LESS_EQUAL, " \
    "borderColor=STATIC_BORDER_COLOR_OPAQUE_WHITE, minLOD=0.0, maxLOD=3.4028234663852886e+38)"
#ifndef DECODER
#define DECODER 0
#endif
#ifndef POOL
#define POOL 0
#endif
#ifndef BIAS
#define BIAS 0
#endif
#ifndef IN
#define IN 1
#endif
#ifndef OUT
#define OUT 1
#endif
#ifndef RELU
#define RELU 1
#endif
#ifndef FOOT_W
#define FOOT_W 16
#endif
#ifndef FOOT_H
#define FOOT_H 16
#endif
#ifndef PX
#define PX 1
#endif
#ifndef PY
#define PY 1
#endif
#ifndef ZT
#define ZT OUT
#endif
#define TW (FOOT_W / PX)
#define TH (FOOT_H / PY)
#define NTHREADS (TW * TH * ZT)
#define ZPER (OUT / ZT)
#define CP (POOL ? 2 : 1)          // conv positions per output position along x and y (2x2 max pool)
#define WW (CP * FOOT_W + 2)
#define WH (CP * FOOT_H + 2)
#define LO (RELU ? 0 : -128)

Texture2DArray<uint4> t0 : register(t0);
#if DECODER
Texture2DArray<uint4> t4 : register(t4);
cbuffer Constants : register(b0) { uint4 constants; };
#endif
StructuredBuffer<uint4> t1 : register(t1);
StructuredBuffer<float4> t2 : register(t2);
StructuredBuffer<float4> t3 : register(t3);
RWTexture2DArray<uint4> u0 : register(u0);

groupshared uint4 win[WW * WH];
// One input slice's weights for all OUT slices, [z][144]: W[(z*IN + slice)*144 + tap*16 + word*4 + s].l feeds output
// channel 16z + 4s + l from input channels 4*word .. 4*word+3.
groupshared uint4 wsm[OUT * 144];

int SByte(uint w, uint k) { return int(w << (24 - 8 * k)) >> 24; }

uint4 InputTexel(int x, int y, uint slice)
{
#if DECODER
    const int Wd = int(constants.x), Hd = int(constants.y);
    if (x < 0 || y < 0 || x >= Wd || y >= Hd) return uint4(0, 0, 0, 0);
    const int xn = x >> 1, yn = y >> 1, xf = (x & 1) ? xn + 1 : xn - 1, yf = (y & 1) ? yn + 1 : yn - 1;
    const uint4 a = t0.Load(int4(xn, yn, slice, 0)), b = t0.Load(int4(xf, yn, slice, 0));
    const uint4 c = t0.Load(int4(xn, yf, slice, 0)), d = t0.Load(int4(xf, yf, slice, 0));
    const uint4 s = t4.Load(int4(x, y, slice, 0));
    uint4 res;
    [unroll] for (uint w = 0; w < 4; ++w)
    {
        uint packed = 0;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const int v = SByte(s[w], k) + (9 * SByte(a[w], k) + 3 * (SByte(b[w], k) + SByte(c[w], k)) + SByte(d[w], k)) / 16;
            packed |= uint(clamp(v, 0, 127)) << (8 * k);
        }
        res[w] = packed;
    }
    return res;
#else
    uint4 v = t0.Load(int4(x, y, slice, 0));   // out of range reads 0
#if BIAS
    v += 0x80808080u;
#endif
    return v;
#endif
}

[RootSignature(UNET_ROOT_SIGNATURE)]
[numthreads(TW, TH, ZT)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const int X0 = int(gid.x) * FOOT_W, Y0 = int(gid.y) * FOOT_H;
    int acc[ZPER][PY][PX][CP * CP][16];
    [unroll] for (uint z0 = 0; z0 < ZPER; ++z0) [unroll] for (uint j0 = 0; j0 < PY; ++j0) [unroll] for (uint i0 = 0; i0 < PX; ++i0)
        [unroll] for (uint c0 = 0; c0 < CP * CP; ++c0) [unroll] for (uint o0 = 0; o0 < 16; ++o0) acc[z0][j0][i0][c0][o0] = 0;
    for (uint slice = 0; slice < IN; ++slice)
    {
        if (slice) GroupMemoryBarrierWithGroupSync();
        for (uint e = index; e < WW * WH; e += NTHREADS) win[e] = InputTexel(CP * X0 - 1 + int(e % WW), CP * Y0 - 1 + int(e / WW), slice);
        for (uint f = index; f < OUT * 144; f += NTHREADS) wsm[f] = t1[((f / 144) * IN + slice) * 144 + f % 144];
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint tap = 0; tap < 9; ++tap)
        {
            const uint ky = tap / 3, kx = tap % 3;
            uint4 texel[PY][PX][CP * CP];
            [unroll] for (uint j = 0; j < PY; ++j) [unroll] for (uint i = 0; i < PX; ++i) [unroll] for (uint c = 0; c < CP * CP; ++c)
                texel[j][i][c] = win[(CP * (tid.y + j * TH) + c / CP + ky) * WW + CP * (tid.x + i * TW) + c % CP + kx];
            [unroll] for (uint word = 0; word < 4; ++word)
                [unroll] for (uint zz = 0; zz < ZPER; ++zz)
                {
                    const uint base = (tid.z * ZPER + zz) * 144 + tap * 16 + word * 4;
                    [unroll] for (uint s4 = 0; s4 < 4; ++s4)
                    {
                        const uint4 w = wsm[base + s4];
                        [unroll] for (uint l = 0; l < 4; ++l)
                            [unroll] for (uint j = 0; j < PY; ++j) [unroll] for (uint i = 0; i < PX; ++i) [unroll] for (uint c = 0; c < CP * CP; ++c)
                                acc[zz][j][i][c][s4 * 4 + l] = dot4add_i8packed(texel[j][i][c][word], w[l], acc[zz][j][i][c][s4 * 4 + l]);
                    }
                }
        }
    }
    [unroll] for (uint zz = 0; zz < ZPER; ++zz)
    {
        const uint z = tid.z * ZPER + zz;
        float scale[16], bias[16];
        [unroll] for (uint q4 = 0; q4 < 4; ++q4)
        {
            const float4 sv = t2[z * 4 + q4], bv = t3[z * 4 + q4];
            [unroll] for (uint l = 0; l < 4; ++l) { scale[q4 * 4 + l] = sv[l]; bias[q4 * 4 + l] = bv[l]; }
        }
        [unroll] for (uint j = 0; j < PY; ++j) [unroll] for (uint i = 0; i < PX; ++i)
        {
            uint4 outWords = uint4(0, 0, 0, 0);
            [unroll] for (uint o = 0; o < 16; ++o)
            {
                int a = acc[zz][j][i][0][o];
                [unroll] for (uint c = 1; c < CP * CP; ++c) a = max(a, acc[zz][j][i][c][o]);   // int32 max pool before requant
                const int q = int(float(a) * scale[o] + bias[o]);   // fmul + fadd, as Intel's program (no mad: a driver fuses both alike)
                outWords[o / 4] |= (uint(clamp(q, LO, 127)) & 0xffu) << (8 * (o % 4));
            }
            u0[uint3(X0 + tid.x + i * TW, Y0 + tid.y + j * TH, z)] = outWords;
        }
    }
}
