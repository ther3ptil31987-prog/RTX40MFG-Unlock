cbuffer DotsParams : register(b0)
{
    uint SegmentCount;
    uint SourceVertexCount;
    uint SourceIndexCount;
    uint OutputVertexCount;
    uint SegmentsPerStrand;
};
StructuredBuffer<float4> StrandVertices : register(t0);
StructuredBuffer<uint> SegmentStarts : register(t1);
RWStructuredBuffer<float3> DotsVertices : register(u0);

void CollapsedSegment(uint base, float3 p)
{
    [unroll] for (uint j = 0; j < 12; ++j)
        DotsVertices[base + j] = p;
}
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint segment = tid.x;
    if (segment >= SegmentCount || segment >= SourceIndexCount ||
        segment >= OutputVertexCount / 12)
        return;
    uint base = segment * 12;
    uint first = SegmentStarts[segment];
    uint segmentsPerStrand = max(SegmentsPerStrand, 1);
    uint expected = (segment / segmentsPerStrand) * (segmentsPerStrand + 1) +
                    segment % segmentsPerStrand;
    if (SourceVertexCount < 2 || first >= SourceVertexCount - 1 || first != expected)
    {
        CollapsedSegment(base, float3(0,0,0));
        return;
    }
    float4 p = StrandVertices[first];
    float4 q = StrandVertices[first + 1];
    float3 safeP = all(isfinite(p.xyz)) ? p.xyz : float3(0,0,0);
    float3 d = q.xyz - p.xyz;
    float length2 = dot(d, d);
    if (!all(isfinite(p)) || !all(isfinite(q)) || p.w < 0 || q.w < 0 ||
        (p.w == 0 && q.w == 0) || !isfinite(length2) || length2 <= 1.0e-20)
    {
        CollapsedSegment(base, safeP);
        return;
    }
    float3 t = d * rsqrt(length2);
    float3 a = abs(t);
    float3 reference = (a.x <= a.y && a.x <= a.z) ? float3(1,0,0) :
                       ((a.y <= a.z) ? float3(0,1,0) : float3(0,0,1));
    float3 s = normalize(cross(t, reference));
    float3 v = cross(t, s);
    // Geometric compensation for two orthogonal strips.
    const float k = 1.11072073454;
    [unroll] for (uint face = 0; face < 2; ++face)
    {
        float3 axis = face == 0 ? s : v;
        float3 A = p.xyz + axis * (p.w * k);
        float3 B = q.xyz - axis * (q.w * k);
        float3 C = q.xyz + axis * (q.w * k);
        float3 D = p.xyz - axis * (p.w * k);
        if (!all(isfinite(A)) || !all(isfinite(B)) ||
            !all(isfinite(C)) || !all(isfinite(D)))
        {
            CollapsedSegment(base, safeP);
            return;
        }
        uint f = base + face * 6;
        DotsVertices[f+0] = A;
        DotsVertices[f+1] = B;
        DotsVertices[f+2] = C;
        DotsVertices[f+3] = A;
        DotsVertices[f+4] = D;
        DotsVertices[f+5] = B;
    }
}
