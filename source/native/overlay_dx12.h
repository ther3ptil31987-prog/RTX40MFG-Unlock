#pragma once
#include <dxgi1_6.h>
#include <cstdint>
struct ID3D12CommandQueue;
namespace single_overlay::dx12 {
struct Session;
// provider: the XeSS-FG runtime image that owns a provider swapchain, or
// null for a system DXGI swapchain.
Session* CreateSession(IDXGISwapChain*, IUnknown*, HMODULE provider = nullptr) noexcept;
bool RetireSession(Session*) noexcept;
// The XeSS-FG swapchain the game presents through. The menu attaches to it
// with the exact provider generation and presentation queue.
bool AttachProviderSwapchain(IDXGISwapChain*, ID3D12CommandQueue*, HMODULE, uint64_t, IDXGISwapChain**) noexcept;
bool DetachProviderSwapchain(IDXGISwapChain*, uint64_t) noexcept;
void DestroySession(Session*) noexcept;
bool BeginPresent(Session*, UINT flags, bool partial) noexcept;
// The session presents the game's main output (largest area, foreground first).
bool IsPrincipal(Session*) noexcept;
void EndPresent(Session*, HRESULT, UINT flags) noexcept;
bool BeginResize(Session*) noexcept;
void EndResize(Session*, HRESULT) noexcept;
void ObserveColorSpace(Session*, DXGI_COLOR_SPACE_TYPE) noexcept;
void VerifyResizeQueues(Session*, UINT, IUnknown* const*) noexcept;
}
