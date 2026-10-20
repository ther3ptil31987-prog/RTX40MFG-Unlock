#pragma once

// Structural, process-memory-only discovery of the provider's cached endpoint
// configuration. No provider RVA, filename, file hash or embedded version selects
// a mutation site. Unknown layouts stay restart-only.
#include "provider_image.h"
#include "third_party/minhook/src/hde/hde64.h"
#include <algorithm>
#include <cstring>

namespace dlssg_preset::config
{
struct Function
{
    uintptr_t begin = 0, end = 0;
    bool Contains(uintptr_t address) const noexcept { return address >= begin && address < end; }
};
struct Layout
{
    HMODULE module = nullptr;
    void* volatile* slot = nullptr;
    uintptr_t controlVtable = 0;
    Function create, evaluate, apply, report;
    uintptr_t configurationReturn = 0;
};

inline bool Region(HMODULE module, uintptr_t address, size_t bytes, bool executable, bool writable = false) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !bytes || !VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory))
        || memory.AllocationBase != module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = memory.Protect & 0xff;
    const bool x = p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE
        || p == PAGE_EXECUTE_WRITECOPY;
    const bool w = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE
        || p == PAGE_EXECUTE_WRITECOPY;
    const auto start = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return (!executable || x) && (!writable || w) && address >= start
        && address - start < memory.RegionSize && bytes <= memory.RegionSize - (address - start);
}

inline Function Enclosing(HMODULE module, uintptr_t address) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    uint32_t size = 0;
    DWORD64 owner = 0;
    auto* entry = RtlLookupFunctionEntry(address, &owner, nullptr);
    if (!provider_image::ImageSize(module, size)) return {};
    for (unsigned depth = 0; depth < 16 && entry && owner == base; ++depth)
    {
        RUNTIME_FUNCTION f{};
        if (!Region(module, reinterpret_cast<uintptr_t>(entry), sizeof(f), false)
            || !provider_image::SafeCopy(&f, entry, sizeof(f)) || f.EndAddress <= f.BeginAddress
            || f.EndAddress > size || f.UnwindData >= size) return {};
        uint8_t unwind[4]{};
        if (!provider_image::SafeCopy(unwind, reinterpret_cast<void*>(base + f.UnwindData), sizeof(unwind))) return {};
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO))
            return Region(module, base + f.BeginAddress, f.EndAddress - f.BeginAddress, true)
                ? Function{base + f.BeginAddress, base + f.EndAddress} : Function{};
        const size_t codes = (size_t(unwind[2]) + 1) & ~size_t(1);
        entry = reinterpret_cast<PRUNTIME_FUNCTION>(base + f.UnwindData + 4 + codes * 2);
    }
    return {};
}

inline uintptr_t Relative(const uint8_t* p, size_t displacement, size_t length) noexcept
{
    int32_t value = 0;
    memcpy(&value, p + displacement, sizeof(value));
    return reinterpret_cast<uintptr_t>(p) + length + value;
}

inline bool Rtti(HMODULE module, uintptr_t table, const char* name) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    uint32_t size = 0;
    uintptr_t locator = 0;
    uint32_t col[6]{};
    uintptr_t methods[2]{};
    if (!provider_image::ImageSize(module, size) || table < base + 8 || table - base >= size
        || !Region(module, table - 8, 24, false)
        || !provider_image::SafeRead(table - 8, locator)
        || !Region(module, locator, sizeof(col), false)
        || !provider_image::SafeCopy(col, reinterpret_cast<void*>(locator), sizeof(col))
        || col[0] != 1 || col[1] || col[2] || col[3] >= size || col[5] >= size
        || base + col[5] != locator
        || strlen(name) + 17 > size - col[3]
        || !Region(module, base + col[3], strlen(name) + 17, false)
        || memcmp(reinterpret_cast<void*>(base + col[3] + 16), name, strlen(name) + 1)
        || !provider_image::SafeCopy(methods, reinterpret_cast<void*>(table), sizeof(methods))) return false;
    return Region(module, methods[0], 1, true) && Region(module, methods[1], 1, true);
}

// Candidate byte scans are validated by walking the owning function. Byte
// strings inside instructions cannot establish a mutation signature.
// stopAtRet: decode only through the first RET (functions with jump-table data
// after their final RET, inside the unwind range).
template<class Visitor>
inline bool Walk(Function f, Visitor visit, bool stopAtRet = false) noexcept
{
    for (auto address = f.begin; address < f.end;)
    {
        hde64s instruction{};
        const auto length = hde64_disasm(reinterpret_cast<void*>(address), &instruction);
        if (!length || (instruction.flags & F_ERROR) || length > f.end - address) return false;
        if (!visit(reinterpret_cast<const uint8_t*>(address), instruction)) return false;
        if (stopAtRet && instruction.opcode == 0xc3) return true;
        address += length;
    }
    return f.begin && f.end > f.begin;
}

// Calls visit(data, length) for each executable section (bounds and executable
// image protection checked), or for each read-only data section (bounds checked);
// false for an invalid image or section, or a visitor that refuses. Callers
// guard the image reads (SEH).
template<class Visitor>
inline bool Sections(HMODULE module, bool executable, Visitor visit) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    uint32_t size = 0;
    if (!provider_image::ImageSize(module, size)) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base
        + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    if (!nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96
        || !Region(module, reinterpret_cast<uintptr_t>(sections), nt->FileHeader.NumberOfSections * sizeof(*sections), false)) return false;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
    {
        const auto flags = sections[s].Characteristics;
        if (executable ? !(flags & IMAGE_SCN_MEM_EXECUTE)
            : (flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) || !(flags & IMAGE_SCN_MEM_READ)) continue;
        const size_t length = sections[s].Misc.VirtualSize;
        if (sections[s].VirtualAddress >= size || length > size - sections[s].VirtualAddress
            || (executable && !Region(module, base + sections[s].VirtualAddress, length, true))
            || !visit(reinterpret_cast<const uint8_t*>(base + sections[s].VirtualAddress), length)) return false;
    }
    return true;
}

inline Layout Discover(HMODULE module, void* reader) noexcept
{
    Layout result{};
    // All image reads below are guarded as a unit; no C++ objects needing unwind
    // live in this SEH function. Helpers use only bounded POD/image reads.
    __try
    {
        if (!reader) return {};
        Function helpers[2]{};
        unsigned keys = 0;
        if (!Sections(module, true, [&](const uint8_t* data, size_t length) {
            for (size_t i = 0; i + 18 <= length; ++i)
            {
                const auto* p = data + i;
                constexpr uint8_t key[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
                if (memcmp(p, key, sizeof(key)) || Relative(p, 6, 10) != reinterpret_cast<uintptr_t>(reader)) continue;
                if (keys == 2) return false;
                helpers[keys++] = Enclosing(module, reinterpret_cast<uintptr_t>(p));
            }
            return true;
        })) return {};
        if (keys != 2 || !helpers[0].begin || !helpers[1].begin || helpers[0].begin == helpers[1].begin) return {};
        // A real configuration call loads this from the singleton immediately
        // before ApplyRenderPreset; the reporting helper receives EndpointCore.
        unsigned matches = 0;
        if (!Sections(module, true, [&](const uint8_t* data, size_t length) {
            for (size_t i = 0; i + 12 <= length; ++i)
            {
                const auto* p = data + i;
                if (memcmp(p, "\x48\x8b\x0d", 3) || p[7] != 0xe8) continue;
                const auto target = Relative(p + 7, 1, 5);
                unsigned h = target == helpers[0].begin ? 0 : target == helpers[1].begin ? 1 : 2;
                if (h == 2) continue;
                if (++matches != 1) return false;
                result = {module, reinterpret_cast<void* volatile*>(Relative(p, 3, 7)), 0,
                    Enclosing(module, reinterpret_cast<uintptr_t>(p)), {}, helpers[h], helpers[1-h], 0};
            }
            return true;
        })) return {};
        if (matches != 1 || !result.create.begin || (reinterpret_cast<uintptr_t>(result.slot) & 7)
            || !Region(module, reinterpret_cast<uintptr_t>(result.slot), 16, false, true)) return {};
        const auto slot = reinterpret_cast<uintptr_t>(result.slot);
        unsigned guard = 0, publish = 0, apply = 0, construct = 0;
        const bool decoded = Walk(result.create, [&](const uint8_t* p, const hde64s& in) {
            const auto remaining = result.create.end - reinterpret_cast<uintptr_t>(p);
            if (remaining >= 14 && in.len == 8 && !memcmp(p, "\x48\x83\x3d", 3)
                && !p[7] && Relative(p, 3, 8) == slot && p[8] == 0x0f && p[9] == 0x85
                && result.create.Contains(Relative(p + 8, 2, 6))) ++guard;
            if (remaining >= 25 && in.len == 4 && !memcmp(p, "\x48\x8d\x43\x10\x48\x89\x05", 7)
                && Relative(p + 4, 3, 7) == slot
                && !memcmp(p + 11, "\x4c\x8b\x35", 3) && Relative(p + 11, 3, 7) == slot + 8
                && !memcmp(p + 18, "\x48\x89\x1d", 3) && Relative(p + 18, 3, 7) == slot + 8) ++publish;
            if (remaining >= 12 && in.len == 7 && !memcmp(p, "\x48\x8b\x0d", 3)
                && Relative(p, 3, 7) == slot && p[7] == 0xe8 && Relative(p + 7, 1, 5) == result.apply.begin)
            { ++apply; }
            if (remaining >= 17 && in.len == 7 && !memcmp(p, "\x48\x8d\x05", 3)
                && !memcmp(p + 7, "\x48\x89\x03\x48\x8d\x4b\x10\xe8", 8))
            {
                const auto table = Relative(p, 3, 7);
                constexpr uint8_t counts[]{0xc7,0x40,0x08,1,0,0,0,0xc7,0x40,0x0c,1,0,0,0};
                if (reinterpret_cast<uintptr_t>(p) >= result.create.begin + sizeof(counts)
                    && !memcmp(p - sizeof(counts), counts, sizeof(counts))
                    && Rtti(module, table, ".?AV?$_Ref_count_obj2@VEndpointConfiguration@@@std@@"))
                { result.controlVtable = table; ++construct; }
            }
            return true;
        });
        if (!decoded || guard != 1 || publish != 1 || apply != 1 || construct != 1) return {};
        // EndpointCore RTTI independently proves the two typed entry ABIs and
        // pairs Create (slot 1, six integer/pointer arguments) with Evaluate
        // (slot 2, four pointer arguments). Unknown vtable layouts are rejected.
        unsigned tables = 0;
        if (!Sections(module, false, [&](const uint8_t* data, size_t length) {
            for (size_t i = 0; i + 24 <= length; i += 8)
            {
                const auto table = reinterpret_cast<uintptr_t>(data) + i;
                uintptr_t create = 0, evaluate = 0;
                if (!provider_image::SafeRead(table + 8, create) || create != result.create.begin
                    || !Rtti(module, table, ".?AVEndpointCore@@")) continue;
                if (!provider_image::SafeRead(table + 16, evaluate) || ++tables != 1) return false;
                result.evaluate = Enclosing(module, evaluate);
                if (result.evaluate.begin != evaluate) return false;
            }
            return true;
        }) || tables != 1) return {};
        constexpr uint8_t endpointEntry[]{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,
            0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
        if (memcmp(reinterpret_cast<void*>(result.create.begin), endpointEntry, sizeof(endpointEntry))
            || memcmp(reinterpret_cast<void*>(result.evaluate.begin), endpointEntry, sizeof(endpointEntry))) return {};
        // Refuse any additional endpoint accessor to the singleton. The CRT's
        // exit destructor references only the control-block slot; the pinned
        // provider cannot unload during the session.
        if (!Sections(module, true, [&](const uint8_t* data, size_t length) {
            for (size_t i = 0; i + 8 <= length; ++i)
            {
                const auto* p = data + i;
                // Decode candidate RIP-relative operands, including SIMD and
                // non-MOV accesses; unknown object readers cannot bypass gates.
                hde64s in{};
                hde64_disasm(p, &in);
                if (!in.len || (in.flags & F_ERROR) || !(in.flags & F_MODRM) || in.p_67
                    || in.modrm_mod != 0 || in.modrm_rm != 5 || in.len > length - i) continue;
                const auto target = reinterpret_cast<uintptr_t>(p) + in.len + static_cast<int32_t>(in.disp.disp32);
                if (target < slot || target >= slot + 8) continue;
                const auto address = reinterpret_cast<uintptr_t>(p);
                bool boundary = false;
                if (!Walk(Enclosing(module, address), [&](const uint8_t* instruction, const hde64s&) {
                    boundary |= instruction == p; return true;
                })) return false;
                if (boundary && !result.create.Contains(address) && !result.evaluate.Contains(address)) return false;
            }
            return true;
        })) return {};
        unsigned configurationReads = 0, reportReads = 0;
        if (!Walk(result.apply, [&](const uint8_t* p, const hde64s& in) {
            if (in.len == 5 && p[0] == 0xe8 && Relative(p, 1, 5) == reinterpret_cast<uintptr_t>(reader))
            {
                // Only the FG preset-key call, not the two other DRS settings.
                if (reinterpret_cast<uintptr_t>(p) >= result.apply.begin + 5
                    && !memcmp(p - 5, "\xb9\xf1\x1d\xe4\x10", 5))
                { result.configurationReturn = reinterpret_cast<uintptr_t>(p + 5); ++configurationReads; }
            }
            return true;
        }) || !Walk(result.report, [&](const uint8_t* p, const hde64s& in) {
            if (in.len == 5 && p[0] == 0xe8 && Relative(p, 1, 5) == reinterpret_cast<uintptr_t>(reader)
                && reinterpret_cast<uintptr_t>(p) >= result.report.begin + 5
                && !memcmp(p - 5, "\xb9\xf1\x1d\xe4\x10", 5)) ++reportReads;
            return true;
        }) || configurationReads != 1 || reportReads != 1) return {};
        return result;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}

struct ResetState { void* object = nullptr; void* control = nullptr; };

inline bool ReadResetState(const Layout& layout, ResetState& state) noexcept
{
    state = {};
    const auto slot = reinterpret_cast<uintptr_t>(layout.slot);
    if ((slot & 7) || !layout.controlVtable || !Region(layout.module, slot, 16, false, true)
        || !provider_image::SafeRead(slot, state.object) || !provider_image::SafeRead(slot + 8, state.control)) return false;
    // Already reset, or not created yet: the provider alone owns the retained
    // control pointer and will release it when it publishes its next object.
    if (!state.object && !state.control) return true;
    const auto control = reinterpret_cast<uintptr_t>(state.control);
    MEMORY_BASIC_INFORMATION heap{};
    uintptr_t table = 0;
    long counts[2]{};
    if (!control || (control & 7) || control > UINTPTR_MAX - 16
        || (state.object && reinterpret_cast<uintptr_t>(state.object) != control + 16)
        || !VirtualQuery(state.control, &heap, sizeof(heap)) || heap.Type != MEM_PRIVATE
        || heap.State != MEM_COMMIT || (heap.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || (heap.Protect & 0xff) != PAGE_READWRITE
        || !provider_image::SafeRead(control, table) || table != layout.controlVtable
        || !provider_image::SafeCopy(counts, reinterpret_cast<void*>(control + 8), sizeof(counts))
        || counts[0] != 1 || counts[1] != 1) return false;
    return true;
}

// Called only with the endpoint gate exclusively held and exact FG quiescence.
// Clear just the object word; never decrement/free a provider allocation or
// manufacture a shared_ptr. The provider's original assignment releases the old
// control block. Expected-value CAS and readback detect conflicting writers.
inline bool Reset(const Layout& layout, const ResetState& expected) noexcept
{
    __try
    {
        if (!expected.object) return true;
        if (layout.slot[1] != expected.control) return false;
        if (InterlockedCompareExchangePointer(layout.slot, nullptr, expected.object) != expected.object) return false;
        MemoryBarrier();
        return !layout.slot[0] && layout.slot[1] == expected.control;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
