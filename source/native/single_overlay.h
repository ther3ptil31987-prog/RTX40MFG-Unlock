#pragma once
#include "single_module.h"

namespace single_overlay
{
void ArmFactoryGateway() noexcept;
// The existing backend boundary rechecks application imports only.
void BeforeStreamlineInit() noexcept;
void InstallKnownModules() noexcept;
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC original) noexcept;
// Streamline manual hooking (slUpgradeInterface). OwnsInterface: the object
// passed in is already this layer's proxy (checked before the upgrade).
// WrapUpgradedInterface: wraps the upgraded DXGI factory in *output so its
// swapchains get the menu; true when *output was replaced.
bool OwnsInterface(void* object) noexcept;
bool WrapUpgradedInterface(void** output) noexcept;
void ReadStatus(MfgSingleModuleStatus& status) noexcept;
}
