#include "ampere_flip_metering.h"
#include "ui_host.h"
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace ampere_flip_metering
{
namespace
{
// 0 = not configured, 1 = off, 2 = on.
std::atomic<uint32_t> gConfiguration{0};
std::atomic<bool> gObserving{false}, gActive{false};
std::atomic<uint64_t> gCalls{0}, gMetered{0}, gUnmetered{0}, gIgnored{0}, gTransitions{0};
std::atomic<uint32_t> gLastFramesPerBatch{0}, gLastVersion{0};
}

bool ContainsCapsQuery(const uint8_t* data, size_t bytes) noexcept
{
    const uint8_t pattern[]{0xB9, static_cast<uint8_t>(kFlipConfigCaps), static_cast<uint8_t>(kFlipConfigCaps >> 8),
        static_cast<uint8_t>(kFlipConfigCaps >> 16), static_cast<uint8_t>(kFlipConfigCaps >> 24)};
    if (!data || bytes < sizeof(pattern)) return false;
    for (size_t i = 0; i + sizeof(pattern) <= bytes; ++i)
        if (data[i] == pattern[0] && !memcmp(data + i, pattern, sizeof(pattern))) return true;
    return false;
}

bool ImageQueriesCaps(void* module) noexcept
{
    if (!module) return false;
    __try
    {
        const auto base = static_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const DWORD imageBytes = nt->OptionalHeader.SizeOfImage;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const DWORD start = section->VirtualAddress, size = section->Misc.VirtualSize;
            if (start >= imageBytes || size > imageBytes - start) return false;
            if (ContainsCapsQuery(base + start, size)) return true;
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Setting ReadSetting(const wchar_t* iniPath) noexcept { return standalone_ui::ReadOnOff(kSection, kKey, iniPath, Setting::eDefault); }

void Configure(bool optIn) noexcept
{
    uint32_t expected = 0;
    gConfiguration.compare_exchange_strong(expected, optIn ? 2u : 1u, std::memory_order_acq_rel);
}

bool Configured() noexcept { return gConfiguration.load(std::memory_order_acquire) != 0; }
bool OptIn() noexcept { return gConfiguration.load(std::memory_order_acquire) == 2; }

bool ReadParams(const void* params, uint32_t& version, uint32_t& framesPerBatch) noexcept
{
    version = framesPerBatch = 0;
    if (!params) return false;
    __try
    {
        uint32_t head[2]{};
        memcpy(head, params, sizeof(head));
        version = head[0];
        framesPerBatch = head[1];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { version = framesPerBatch = 0; return false; }
}

bool Observe(uint32_t version, uint32_t framesPerBatch, int32_t status) noexcept
{
    gCalls.fetch_add(1, std::memory_order_relaxed);
    gLastVersion.store(version, std::memory_order_relaxed);
    gLastFramesPerBatch.store(framesPerBatch, std::memory_order_relaxed);
    bool active = false;
    switch (Classify(version, framesPerBatch, status))
    {
    case Observation::eMetered: gMetered.fetch_add(1, std::memory_order_relaxed); active = OptIn(); break;
    case Observation::eUnmetered: gUnmetered.fetch_add(1, std::memory_order_relaxed); break;
    case Observation::eIgnored: gIgnored.fetch_add(1, std::memory_order_relaxed); break;
    }
    const bool changed = gActive.exchange(active, std::memory_order_acq_rel) != active;
    if (changed) gTransitions.fetch_add(1, std::memory_order_relaxed);
    return changed;
}

void MarkObserving() noexcept { gObserving.store(true, std::memory_order_release); }
bool Observing() noexcept { return gObserving.load(std::memory_order_acquire); }
bool Active() noexcept { return OptIn() && gActive.load(std::memory_order_acquire); }

Counters Read() noexcept
{
    Counters counters;
    counters.calls = gCalls.load(std::memory_order_relaxed);
    counters.metered = gMetered.load(std::memory_order_relaxed);
    counters.unmetered = gUnmetered.load(std::memory_order_relaxed);
    counters.ignored = gIgnored.load(std::memory_order_relaxed);
    counters.transitions = gTransitions.load(std::memory_order_relaxed);
    counters.lastFramesPerBatch = gLastFramesPerBatch.load(std::memory_order_relaxed);
    counters.lastVersion = gLastVersion.load(std::memory_order_relaxed);
    return counters;
}

#if MFG_AMPERE_FLIP_METERING_TESTING
void testing::Reset() noexcept
{
    gConfiguration.store(0); gObserving.store(false); gActive.store(false);
    gCalls.store(0); gMetered.store(0); gUnmetered.store(0); gIgnored.store(0); gTransitions.store(0);
    gLastFramesPerBatch.store(0); gLastVersion.store(0);
}
#endif
}
