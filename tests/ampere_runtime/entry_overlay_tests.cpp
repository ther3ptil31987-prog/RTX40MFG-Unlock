// ngxforward.1 (GitHub issue 115): entry_detour's `overlaid` state on real
// installed detours in EntryOverlayFixture.dll. Another hook over ours is
// simulated by writing its patch into the fixture's entries; its hook calls ours,
// as a real hook's trampoline running our relocated jump does. No NVIDIA code
// runs. All patched pages are private to this short-lived test process.
#include "entry_detour.h"
#include <cstdio>
#include <cstring>

namespace
{
using OverlayFn = int (*)(int, int);
OverlayFn gTrampolines[8]{};
template<int N> int Hook(int a, int b) { return gTrampolines[N](a, b) + 1000; }
constexpr int Expected(int a, int b) { return ((a + b) * 3) ^ 0x5A; }

unsigned gChecks = 0, gFailures = 0;
void Check(bool ok, const char* message)
{
    ++gChecks; gFailures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    fflush(stdout);
}

bool Write(void* address, const void* bytes, size_t size)
{
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(address, bytes, size);
    VirtualProtect(address, size, old, &ignored);
    return FlushInstructionCache(GetCurrentProcess(), address, size) != FALSE;
}

// The other hook's rel32 jump at `at`.
bool WriteJump(uint8_t* at, const void* destination)
{
    const intptr_t displacement = static_cast<const uint8_t*>(destination) - (at + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) return false;
    uint8_t jump[5]{0xE9};
    const int32_t value = static_cast<int32_t>(displacement);
    std::memcpy(jump + 1, &value, sizeof(value));
    return Write(at, jump, sizeof(jump));
}

HMODULE gFixture = nullptr;
uint8_t* Target(const char* name) { return reinterpret_cast<uint8_t*>(GetProcAddress(gFixture, name)); }
int Call(const char* name) { return reinterpret_cast<OverlayFn>(GetProcAddress(gFixture, name))(2, 3); }

template<int N> entry_detour::Handle Install(const char* name, entry_detour::Kind kind)
{
    entry_detour::InstallOptions options{};
    options.generation = 9;
    options.allowRelocated = true;
    void* original = nullptr;
    entry_detour::Handle handle{};
    if (!Target(name) || !entry_detour::Install(kind, gFixture, Target(name), reinterpret_cast<void*>(&Hook<N>),
            original, options, &handle) || !original) return {};
    gTrampolines[N] = reinterpret_cast<OverlayFn>(original);
    return handle;
}
}

int wmain()
{
    gFixture = LoadLibraryW(L"EntryOverlayFixture.dll");
    const auto setNext = gFixture ? reinterpret_cast<void (*)(OverlayFn)>(GetProcAddress(gFixture, "OverlaySetNext")) : nullptr;
    const auto foreignCalls = gFixture ? reinterpret_cast<long (*)()>(GetProcAddress(gFixture, "OverlayForeignCalls")) : nullptr;
    void* foreignHook = gFixture ? reinterpret_cast<void*>(GetProcAddress(gFixture, "OverlayForeignHook")) : nullptr;
    if (!setNext || !foreignCalls || !foreignHook)
    {
        printf("setup: EntryOverlayFixture.dll exports unavailable (%lu)\n", GetLastError());
        return 2;
    }
    // No hotpatch profile: these install in the relocated form.
    const auto relocated = entry_detour::Kind::eAmpereParameters;

    {
        const auto state = entry_detour::ReadSnapshot(Install<0>("OverlayTargetCurrent", relocated));
        Check(state.installed && state.current && state.method == entry_detour::Method::eRelocated && !state.overlaid,
            "relocated detour on a five-byte first instruction: current, not overlaid");
        Check(Call("OverlayTargetCurrent") == Expected(2, 3) + 1000, "current: our hook forwards through our trampoline");
    }
    {
        const auto handle = Install<1>("OverlayTargetRel32", relocated);
        setNext(&Hook<1>);
        Check(handle && WriteJump(Target("OverlayTargetRel32"), foreignHook), "another hook's rel32 jump written over ours");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(!state.current && state.overlaid && state.original == reinterpret_cast<void*>(gTrampolines[1]),
            "rel32 jump over ours: not current, overlaid, trampoline unchanged");
        const long before = foreignCalls();
        Check(Call("OverlayTargetRel32") == Expected(2, 3) + 1000 && foreignCalls() == before + 1,
            "rel32 jump over ours: the other hook, ours, then our trampoline to the target");
    }
    {
        const auto handle = Install<2>("OverlayTargetShort", relocated);
        uint8_t* entry = Target("OverlayTargetShort");
        setNext(&Hook<2>);
        const uint8_t shortJump[2]{0xEB, 0xF9};
        Check(handle && WriteJump(entry - 5, foreignHook) && Write(entry, shortJump, sizeof(shortJump)),
            "another hook's hot-patch short jump written over ours");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(!state.current && state.overlaid, "short jump over ours: not current, overlaid");
        const long before = foreignCalls();
        Check(Call("OverlayTargetShort") == Expected(2, 3) + 1000 && foreignCalls() == before + 1,
            "short jump over ours: the other hook, ours, then our trampoline to the target");
    }
    {
        const auto handle = Install<3>("OverlayTargetAbsolute", relocated);
        uint8_t absolute[14]{0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(absolute + 6, &foreignHook, sizeof(foreignHook));
        Check(handle && Write(Target("OverlayTargetAbsolute"), absolute, sizeof(absolute)),
            "another hook's 14-byte absolute jump written over ours");
        const auto state = entry_detour::ReadSnapshot(handle);
        // Our trampoline resumes at entry + 5, inside that jump: never forwarded.
        Check(state.installed && !state.current && !state.overlaid, "14-byte jump over ours: not current, not overlaid");
    }
    {
        const auto handle = Install<4>("OverlayTargetRestored", relocated);
        const uint8_t prologue[5]{0x48, 0x89, 0x5C, 0x24, 0x08};
        Check(handle && Write(Target("OverlayTargetRestored"), prologue, sizeof(prologue)),
            "the original first instruction written back over ours");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(state.installed && !state.current && !state.overlaid, "unhooked entry: not current, not overlaid");
        Check(Call("OverlayTargetRestored") == Expected(2, 3), "unhooked entry: calls no longer reach our hook");
    }
    {
        const auto handle = Install<5>("OverlayTargetOurs", relocated);
        uint8_t* entry = Target("OverlayTargetOurs");
        uint8_t ours[5]{};
        std::memcpy(ours, entry, sizeof(ours));
        setNext(&Hook<5>);
        Check(handle && WriteJump(entry, foreignHook), "another hook's jump written over ours before a rescan");
        entry_detour::InstallOptions options{};
        options.generation = 9;
        options.allowRelocated = true;
        void* original = nullptr;
        Check(!entry_detour::Install(relocated, gFixture, entry, reinterpret_cast<void*>(&Hook<5>), original, options)
            && entry_detour::ReadSnapshot(handle).failure == entry_detour::Failure::ePostWriteVerificationFailed,
            "a rescan of the covered entry records the verification failure");
        Check(Write(entry, ours, sizeof(ours)), "our jump written back");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(!state.current && state.overlaid, "our jump back after a failed verification: not current, overlaid");
        Check(Call("OverlayTargetOurs") == Expected(2, 3) + 1000, "our jump back: our hook forwards through our trampoline");
    }
    {
        const auto handle = Install<6>("OverlayTargetTail", relocated);
        uint8_t* entry = Target("OverlayTargetTail");
        const uint8_t changed = static_cast<uint8_t>(entry[10] ^ 0xFF);
        Check(handle && WriteJump(entry, foreignHook) && Write(entry + 10, &changed, 1),
            "another hook's jump plus a changed byte after our five");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(state.installed && !state.current && !state.overlaid, "changed bytes after the entry: not overlaid");
    }
    {
        // The runtime Evaluate profile (mov [rsp+8], rbx) replays the whole first instruction.
        const auto handle = Install<7>("OverlayTargetHotpatch", entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature);
        const auto installed = entry_detour::ReadSnapshot(handle);
        Check(installed.current && installed.method == entry_detour::Method::eHotpatch && !installed.overlaid,
            "hot-patch detour replaying five bytes: current, not overlaid");
        setNext(&Hook<7>);
        Check(WriteJump(Target("OverlayTargetHotpatch"), foreignHook), "another hook's rel32 jump written over our hot patch");
        const auto state = entry_detour::ReadSnapshot(handle);
        Check(!state.current && state.overlaid, "rel32 jump over our hot patch: not current, overlaid");
        const long before = foreignCalls();
        Check(Call("OverlayTargetHotpatch") == Expected(2, 3) + 1000 && foreignCalls() == before + 1,
            "rel32 jump over our hot patch: the other hook, ours, then our trampoline to the target");
    }
    printf("ENTRY_OVERLAY checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
