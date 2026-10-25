#pragma once
#include "single_module_status.h"
#include <dxgi1_6.h>
namespace single_overlay::proxy {
using FactoryFn=HRESULT (WINAPI*)(REFIID,void**);
using Factory2Fn=HRESULT (WINAPI*)(UINT,REFIID,void**);
using ParentFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIObject*,REFIID,void**);
HRESULT FactoryCall(FactoryFn,REFIID,void**) noexcept;
HRESULT FactoryCall(Factory2Fn,UINT,REFIID,void**) noexcept;
HRESULT ParentFactoryCall(ParentFn,IDXGIObject*,REFIID,void**) noexcept;
// Streamline manual hooking: the application creates native DXGI objects and
// upgrades them with slUpgradeInterface. Owned reports this layer's proxies;
// WrapUpgraded wraps the upgraded factory in *output as if creation had
// returned it, so its swapchains get the menu. True when *output was replaced.
bool Owned(IUnknown*) noexcept;
bool WrapUpgraded(void** output) noexcept;
// diag171.1 logging: "<pointer> <kind> ours=<0|1> queryInterface=<module+0xRVA>".
// Kind is the first public interface the object answers: factory, swapchain,
// d3d12-device, d3d12-queue, adapter or other.
void Describe(IUnknown*,wchar_t* text,size_t count) noexcept;
void ReadStatus(MfgSingleModuleStatus&) noexcept;
}
