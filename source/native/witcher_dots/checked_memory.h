#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
namespace witcher_dots {
inline bool Readable(const void* p,size_t size) noexcept {
    const auto start=reinterpret_cast<uintptr_t>(p);
    if(!start||size>UINTPTR_MAX-start)return false;
    uintptr_t at=start;const uintptr_t end=start+size;
    while(at<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m))||m.State!=MEM_COMMIT
            ||(m.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        const DWORD access=m.Protect&0xff;
        if(access!=PAGE_READONLY&&access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY
            &&access!=PAGE_EXECUTE_READ&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY)return false;
        const uintptr_t next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        if(next<=at)return false;at=std::min(next,end);
    }
    return true;
}
inline bool CopyChecked(void* dst,const void* src,size_t size) noexcept {
    if(!Readable(src,size))return false;
    __try { std::memcpy(dst,src,size);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Hot paths (every hair build): memory already established as committed and
// readable this call (a game object validated by the builder hook, a game
// argument block, a table this runtime verified) is read under a structured
// exception guard only, without querying the process address space.
inline bool CopyFast(void* dst,const void* src,size_t size) noexcept {
    if(!dst||!src)return false;
    __try { std::memcpy(dst,src,size);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool ReadFast(const void* base,size_t offset,T& out) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(base);
    return address&&offset<=UINTPTR_MAX-address&&CopyFast(&out,reinterpret_cast<void*>(address+offset),sizeof(T));
}
template<class T> bool Read(const void* base,size_t offset,T& out) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(base);
    return offset<=UINTPTR_MAX-address&&CopyChecked(&out,reinterpret_cast<void*>(address+offset),sizeof(T));
}
}
