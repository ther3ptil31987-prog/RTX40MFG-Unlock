#pragma once
#include <Windows.h>

// Normal CFG is compatible with the direct branches and executable pages the
// entry detours, import gateways and relays use. Strict CFG/XFG and strict
// non-CET policies are rejected: a partially usable trampoline is worse than a
// clean pass-through route.
inline bool HookMitigationsSupported() noexcept
{
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY cet{};
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg))
        && GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &cet, sizeof(cet))
        && !cfg.StrictMode && !cfg.EnableXfg && !cet.EnableUserShadowStackStrictMode
        && !cet.BlockNonCetBinaries && !cet.BlockNonCetBinariesNonEhcont;
}
