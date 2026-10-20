#pragma once
// entry_detour stubbed for tests that compile production sources using it: nothing is installed. ReadSnapshot(kind,
// owner) reports testHooks[kind] only for owner == testOwner (set by the test; null = no hooks). It defines the
// functions, so include it in one translation unit per executable.
#include "entry_detour.h"
#include <array>

namespace entry_detour
{
bool Install(Kind, HMODULE, void*, void*, void*& originalTrampoline, const InstallOptions&, Handle*) noexcept
{ originalTrampoline = nullptr; return false; }
bool Install(Kind, HMODULE, void*, void*, void*& originalTrampoline) noexcept { originalTrampoline = nullptr; return false; }
bool InstallForwarding(Kind, HMODULE, void*, ForwardPreCall, void*& originalTrampoline, const InstallOptions&,
    Handle*, ForwardGate, void*) noexcept
{ originalTrampoline = nullptr; return false; }
// testHandle (if set) reports testHandleHook: an exact entry such as a route-owned preset reader.
Handle testHandle{};
Snapshot testHandleHook{};
Snapshot ReadSnapshot(Handle handle) noexcept { return handle && handle == testHandle ? testHandleHook : Snapshot{}; }
Snapshot ReadSnapshot(Kind) noexcept { return {}; }
HMODULE testOwner = nullptr;
std::array<Snapshot, static_cast<size_t>(Kind::eCount)> testHooks{};
Snapshot ReadSnapshot(Kind kind, HMODULE owner) noexcept
{ return owner && owner == testOwner ? testHooks[static_cast<size_t>(kind)] : Snapshot{}; }
}
