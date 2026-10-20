#include "host_unlock.h"
#include "runtime_layout.h"
#include <cstring>
#include <stdexcept>

namespace xess_host_unlock
{
namespace
{
void Write(uint8_t* address, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, DWORD protection,
    const char* name)
{
    if (memcmp(address, from.data(), from.size()) != 0) throw std::runtime_error(std::string(name)+"_unexpected_bytes");
    DWORD prior{};
    if (!VirtualProtect(address, to.size(), PAGE_EXECUTE_READWRITE, &prior))
        throw std::runtime_error(std::string(name)+"_make_writable");
    memcpy(address, to.data(), to.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, to.size()) != FALSE;
    DWORD temporary{};
    const bool restored = VirtualProtect(address, to.size(), prior, &temporary) != FALSE;
    MEMORY_BASIC_INFORMATION now{};
    const bool queried = VirtualQuery(address, &now, sizeof(now)) == sizeof(now);
    if (!flushed || !restored || !queried || now.Protect != protection || memcmp(address, to.data(), to.size()) != 0)
        throw std::runtime_error(std::string(name)+"_write_verification");
}
}

std::vector<Site> Sites(uint8_t n, bool keep2xModel, bool llGate, const xess_layout::Fields& fields)
{
    const auto low = uint8_t(fields.overrideCount), high = uint8_t(fields.overrideCount >> 8);
    const auto skip = fields.keep2xSkip;
    std::vector<Site> all{
        {"fg_override_clamp", true, 0x1A45C2, {0xC7,0x87,low,high,0,0,0x01,0,0,0}, {0xC7,0x87,low,high,0,0,n,0,0,0}},
        {"fg_default_ceiling", true, 0x1A517D, {0xBB,0x03,0,0,0}, {0xBB,n,0,0,0}},
        {"fg_reported_maximum", true, 0x20973B, {0xB8,0x01,0,0,0}, {0xB8,n,0,0,0}},
        {"fg_keep_2x_model", true, 0x1A5DE4, {0x74,skip}, {0xEB,uint8_t(skip-3)}},
        // cmp eax,1 / cmp edx,1 -> 0: a live count change to or from 1 no longer re-initializes XeFG.
        {"fg_live_count_no_reinit", true, 0x21B86B, {0x83,0xF8,0x01,0x74,0x0F,0x83,0xFA,0x01,0x74,0x0A},
            {0x83,0xF8,0x00,0x74,0x0F,0x83,0xFA,0x00,0x74,0x0A}},
        {"ll_generated_frames_gate", false, kLlGate, {0x83,0xFB,0x03,0x76,0x07}, {0x83,0xFB,n,0x76,0x07}},
    };
    if (!keep2xModel)
        std::erase_if(all, [](const Site& site) { return std::string(site.name) == "fg_keep_2x_model"; });
    if (!llGate) std::erase_if(all, [](const Site& site) { return !site.fg; });
    return all;
}

Unlock::Unlock(HMODULE fg, HMODULE ll, uint8_t generated, bool keep2xModel) : keeps2xModel(keep2xModel)
{
    if (generated < 2 || generated > 5) throw std::runtime_error("generated_count_range");
    const auto& fgLayout = xess_layout::Get(fg);
    const auto& llLayout = xess_layout::Get(ll, false);
    llGated = llLayout.addresses.contains(kLlGate);
    if (!llGated)
    {
        if (!llLayout.addresses.contains(kLlUngatedSetter)) throw std::runtime_error("ll_count_gate_unknown");
        // The witnessed ungated setter is still the one in the image (no gate added since discovery).
        llLayout.VerifyFunction(ll, kLlUngatedSetter);
    }
    sites = Sites(generated, keep2xModel, llGated, fgLayout.fields);
    for (auto& site : sites) site.rva = xess_layout::Rva(site.fg ? fg : ll, site.rva, site.fg);
    for (const auto& site : sites)
    {
        auto* address = reinterpret_cast<uint8_t*>(site.fg ? fg : ll)+site.rva;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != (site.fg ? fg : ll)
            || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
            || reinterpret_cast<uintptr_t>(address)+site.before.size()
                > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
            throw std::runtime_error(std::string(site.name)+"_ownership");
        if (memcmp(address, site.before.data(), site.before.size()) != 0)
            throw std::runtime_error(std::string(site.name)+"_unexpected_bytes");
        targets.push_back({&site, address, info.Protect});
    }
    size_t next = 0;
    try
    {
        for (; next < targets.size(); ++next)
        {
            const auto& target = targets[next];
            Write(target.address, target.site->before, target.site->after, target.protection, target.site->name);
            applied.push_back(*target.site);
        }
    }
    catch (...)
    {
        // A site whose write failed verification after the bytes changed is put back too.
        if (next < targets.size())
        {
            const auto& failed = targets[next];
            if (memcmp(failed.address, failed.site->after.data(), failed.site->after.size()) == 0)
                try { Write(failed.address, failed.site->after, failed.site->before, failed.protection, failed.site->name); }
                catch (...) {}
        }
        Restore();
        throw;
    }
}

void Unlock::Restore()
{
    for (size_t i = applied.size(); i-- > 0;)
    {
        const auto& target = targets[i];
        Write(target.address, target.site->after, target.site->before, target.protection, target.site->name);
    }
    applied.clear();
}

SceneDetour::SceneDetour(HMODULE fg, SceneDecision replacement)
{
    static constexpr uint8_t kEntry[] = {0x48,0x83,0xEC,0x48, 0xF3,0x0F,0x10,0x6A,0x08, 0x0F,0x57,0xC0};
    static constexpr uint8_t kThreshold[] = {0xF3,0x0F,0x10,0xB9}; // movss xmm7,[rcx+disp32]
    constexpr size_t kThresholdAt = 0x78;
    if (!replacement) throw std::runtime_error("scene_replacement_missing");
    const auto& layout = xess_layout::Get(fg);
    rva = layout.Rva(kSceneDecision);
    layout.VerifyFunction(fg, kSceneDecision); // unchanged since discovery: the family's witness
    const auto& body = layout.witnessed.at(rva);
    if (body.size() < kThresholdAt+8 || memcmp(body.data(), kEntry, sizeof(kEntry)) != 0)
        throw std::runtime_error("scene_entry_bytes");
    if (memcmp(body.data()+kThresholdAt, kThreshold, sizeof(kThreshold)) != 0) throw std::runtime_error("scene_threshold_field");
    memcpy(&thresholdOffset, body.data()+kThresholdAt+sizeof(kThreshold), sizeof(thresholdOffset));
    if (thresholdOffset < 0x40 || thresholdOffset > 0x400 || thresholdOffset % 4) throw std::runtime_error("scene_threshold_field");
    before.assign(std::begin(kEntry), std::end(kEntry));
    const auto target = reinterpret_cast<uint64_t>(replacement);
    after = {0x48,0xB8}; // mov rax,imm64
    for (int i = 0; i < 8; ++i) after.push_back(uint8_t(target >> (8*i)));
    after.insert(after.end(), {0xFF,0xE0}); // jmp rax
    address = reinterpret_cast<uint8_t*>(fg)+rva;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != fg
        || info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ
        || reinterpret_cast<uintptr_t>(address)+after.size() > reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)
        throw std::runtime_error("fg_scene_change_off_ownership");
    protection = info.Protect;
    try { Write(address, before, after, protection, "fg_scene_change_off"); }
    catch (...)
    {
        // Written but not verified (protection or readback): put the entry back.
        if (memcmp(address, after.data(), after.size()) == 0)
            try { Write(address, after, before, protection, "fg_scene_change_off"); } catch (...) {}
        throw;
    }
    applied = true;
}

void SceneDetour::Restore()
{
    if (!applied) return;
    Write(address, after, before, protection, "fg_scene_change_off");
    applied = false;
}
}
