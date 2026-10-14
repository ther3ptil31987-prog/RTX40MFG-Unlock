#include <Windows.h>
extern "C" __declspec(dllexport) int NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl() { return 1; }
#if FIXTURE_SIBLING
extern "C" __declspec(dllexport) int NVSDK_NGX_DirectSR_Create() { return 1; }
#endif
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        wchar_t name[256]{};
        if (GetEnvironmentVariableW(L"MFG_SELECTION_FIXTURE_EVENT", name, 256))
            if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name))
            { SetEvent(event); CloseHandle(event); }
    }
    return TRUE;
}
