#include <Windows.h>
#include <sl.h>
// Dying Light 2 / The Beast renderer model (rd3d12_x64_rwdi.dll): no DXGI or
// D3D12 imports. It resolves the interposer's factory exports through
// GetProcAddress before slInit, calls slInit, then creates factories from its
// own image. RendererHostFixture imports slInit statically, as the game does;
// RendererDynamicFixture resolves it, so it must not qualify as the host.
namespace {
FARPROC factories[3]{};
HMODULE Interposer(){return LoadLibraryA("sl.interposer.dll");} // The loaded interposer, by base name.
}
extern "C" __declspec(dllexport) BOOL WINAPI HostResolveFactories(){
    const HMODULE interposer=Interposer();
    if(!interposer)return FALSE;
    factories[0]=GetProcAddress(interposer,"CreateDXGIFactory");
    factories[1]=GetProcAddress(interposer,"CreateDXGIFactory1");
    factories[2]=GetProcAddress(interposer,"CreateDXGIFactory2");
    return factories[0]&&factories[1]&&factories[2];
}
extern "C" __declspec(dllexport) void* WINAPI HostFactoryEntry(unsigned api){
    return api<3?reinterpret_cast<void*>(factories[api]):nullptr;
}
// Volatile results keep this module's return address (no tail dispatch).
extern "C" __declspec(dllexport) sl::Result WINAPI HostInit(){
    sl::Preferences preferences{};
#if FIXTURE_STATIC_SLINIT
    sl::Result volatile result=slInit(preferences,sl::kSDKVersion);
#else
    using Init=sl::Result(*)(const sl::Preferences&,uint64_t);
    const auto init=reinterpret_cast<Init>(GetProcAddress(Interposer(),"slInit"));
    sl::Result volatile result=init?init(preferences,sl::kSDKVersion):sl::Result::eErrorNotInitialized;
#endif
    return result;
}
extern "C" __declspec(dllexport) HRESULT WINAPI HostCreateFactory(unsigned api,UINT flags,REFIID iid,void** out){
    using Fn=HRESULT(WINAPI*)(REFIID,void**);
    using Fn2=HRESULT(WINAPI*)(UINT,REFIID,void**);
    if(api>2||!factories[api])return E_FAIL;
    HRESULT volatile result=api<2?reinterpret_cast<Fn>(factories[api])(iid,out)
        :reinterpret_cast<Fn2>(factories[api])(flags,iid,out);
    return result;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
