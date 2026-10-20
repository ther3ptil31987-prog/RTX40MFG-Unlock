// Another module's DXGI adapter queries (stands in for Streamline, NGX or the
// driver). The counter after each call keeps them out of tail position, so the
// adapter method returns into this DLL, never into the test executable.
#include <dxgi1_6.h>

static volatile long gCalls = 0;

extern "C" __declspec(dllexport) HRESULT ProbeGetDesc(IDXGIAdapter* adapter, DXGI_ADAPTER_DESC* desc)
{
    const HRESULT result = adapter->GetDesc(desc);
    ++gCalls;
    return result;
}

extern "C" __declspec(dllexport) HRESULT ProbeGetDesc1(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1* desc)
{
    const HRESULT result = adapter->GetDesc1(desc);
    ++gCalls;
    return result;
}

extern "C" __declspec(dllexport) HRESULT ProbeGetDesc3(IDXGIAdapter4* adapter, DXGI_ADAPTER_DESC3* desc)
{
    const HRESULT result = adapter->GetDesc3(desc);
    ++gCalls;
    return result;
}
