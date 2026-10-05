#include "geometry.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace witcher_dots {
namespace {
Vec3 Add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 Scale(Vec3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
float Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
Vec3 Unit(Vec3 a,Vec3 fallback) {
    const float n=Dot(a,a);
    return std::isfinite(n)&&n>1e-20f?Scale(a,1/std::sqrt(n)):fallback;
}
}
Plan MakePlan(uint64_t segments,uint64_t sourceVertices,uint64_t sourceIndices,uint64_t budget) noexcept {
    if (!segments||segments>kMaxSegments||sourceVertices<2||sourceVertices>UINT32_MAX
        ||sourceIndices<segments||sourceIndices>UINT32_MAX||segments>budget/kBytesPerSegment) return {};
    return {static_cast<uint32_t>(segments),static_cast<uint32_t>(segments*kVerticesPerSegment),
        static_cast<uint32_t>((segments+63)/64),segments*kBytesPerSegment};
}
std::array<Vec3,12> Tessellate(StrandVertex p,StrandVertex q) noexcept {
    std::array<Vec3,12> out{};
    const Vec3 safe=Finite(p.position)?p.position:Vec3{};
    out.fill(safe);
    const Vec3 d=Sub(q.position,p.position);
    const float n=Dot(d,d);
    if (!Finite(p.position)||!Finite(q.position)||!std::isfinite(p.radius)||!std::isfinite(q.radius)
        ||p.radius<0||q.radius<0||(p.radius==0&&q.radius==0)||!std::isfinite(n)||n<=1e-20f) return out;
    const Vec3 t=Scale(d,1/std::sqrt(n));
    const Vec3 a{std::abs(t.x),std::abs(t.y),std::abs(t.z)};
    const Vec3 ref=(a.x<=a.y&&a.x<=a.z)?Vec3{1,0,0}:(a.y<=a.z?Vec3{0,1,0}:Vec3{0,0,1});
    const Vec3 s=Unit(Cross(t,ref),{}),v=Cross(t,s);
    constexpr float k=1.11072073454f;
    for (uint32_t face=0;face<2;++face) {
        const Vec3 axis=face==0?s:v;
        const Vec3 A=Add(p.position,Scale(axis,p.radius*k)),B=Sub(q.position,Scale(axis,q.radius*k));
        const Vec3 C=Add(q.position,Scale(axis,q.radius*k)),D=Sub(p.position,Scale(axis,p.radius*k));
        if (!Finite(A)||!Finite(B)||!Finite(C)||!Finite(D)) { out.fill(safe); return out; }
        const uint32_t f=face*6;
        out[f]=A;out[f+1]=B;out[f+2]=C;out[f+3]=A;out[f+4]=D;out[f+5]=B;
    }
    return out;
}
float StrandU(uint32_t triangle,float x,float y) noexcept { return triangle&1?y:x+y; }
uint64_t SourceVertex(uint32_t triangle,uint32_t segmentsPerStrand) noexcept {
    const uint64_t s=std::max(segmentsPerStrand,1u),p=triangle>>2;
    return (p/s)*(s+1)+p%s;
}
Vec3 RoundedNormal(StrandVertex p,StrandVertex q,Vec3 origin,Vec3 direction,Vec3 fallback) noexcept {
    const Vec3 ba=Sub(q.position,p.position), op=Sub(origin,p.position);
    const float m0=Dot(ba,ba), dir2=Dot(direction,direction), rr=p.radius-q.radius;
    if (!Finite(op)||!Finite(ba)||!Finite(direction)||!std::isfinite(rr)||p.radius<0||q.radius<0
        ||!std::isfinite(m0)||!std::isfinite(dir2)||m0<=1e-20f||dir2<=1e-20f) return Unit(fallback,{0,1,0});
    const float shift=-Dot(direction,op)/dir2;
    const Vec3 oa=Add(op,Scale(direction,shift));
    const float m1=Dot(ba,oa),m2=Dot(ba,direction),m3=Dot(direction,oa),m5=Dot(oa,oa);
    const float d2=m0-rr*rr,k2=d2*dir2-m2*m2;
    const float k1=d2*m3-m1*m2+m2*rr*p.radius;
    const float k0=d2*m5-m1*m1+2*m1*rr*p.radius-m0*p.radius*p.radius;
    const float disc=k1*k1-k0*k2;
    if (!std::isfinite(disc)||disc<0||!std::isfinite(k2)||std::abs(k2)<=1e-12f||d2<=0) return Unit(fallback,{0,1,0});
    const float hit=(-std::sqrt(disc)-k1)/k2,y=m1-p.radius*rr+hit*m2;
    const Vec3 normal=Sub(Scale(Add(oa,Scale(direction,hit)),d2),Scale(ba,y));
    if (!std::isfinite(hit)||hit+shift<0||!Finite(normal)) return Unit(fallback,{0,1,0});
    return Unit(normal,Unit(fallback,{0,1,0}));
}
}
