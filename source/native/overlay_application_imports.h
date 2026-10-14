#pragma once
#include <Windows.h>

namespace single_overlay::application_imports {
// Loaded application renderer DLLs only. No loading, graphics creation, entry
// patching, waiting, or work in loader-notification callbacks.
void ArmModule(HMODULE module) noexcept;
void ArmStartupDependencies() noexcept;
// The local application DLL containing an slInit call site, when it statically
// imports slInit from this exact interposer image (a renderer DLL that hosts
// Streamline and resolves its factory exports itself). Pinned for the process
// lifetime before it is returned; nullptr for the executable or anything else.
HMODULE StreamlineHost(const void* caller, HMODULE interposer) noexcept;
}
