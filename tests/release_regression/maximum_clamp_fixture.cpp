// Stand-in for a Streamline DLSS-G wrapper (sl.dlss_g.dll), not an NVIDIA
// binary: it exports slGetPluginFunction and carries exactly one recognized
// 1/3/5 maximum clamp (maximum_clamp_fixture.asm). FixtureClampPatched reports
// whether RTXMFG has replaced the clamp's cmovb.
#include <Windows.h>
#include <cstdint>
#include <cstring>
extern "C" void FixtureClamp();
extern "C" __declspec(dllexport) void* slGetPluginFunction(const char*){return nullptr;}
extern "C" __declspec(dllexport) int WINAPI FixtureClampPatched()
{
    static constexpr uint8_t kNative[3]{0x0F, 0x42, 0xD1};
    static constexpr uint8_t kPatched[3]{0x90, 0x90, 0x90};
    const auto* clamp = reinterpret_cast<const uint8_t*>(&FixtureClamp) + 7;
    return std::memcmp(clamp, kPatched, 3) == 0 ? 1 : std::memcmp(clamp, kNative, 3) == 0 ? 0 : -1;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
