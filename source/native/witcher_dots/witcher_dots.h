#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
namespace witcher_dots {
enum class Stage : uint32_t { Disabled,WaitingForDevice,Preparing,UnsupportedGame,UnsupportedGpu,NativeLss,Failed,Active,UnsupportedDriver };
struct Snapshot {
    bool applicable{},requested{},environmentOverride{},shaderReady{};
    Stage stage{};
    uint32_t processId{},deviceId{},driverVersion{};
    LUID luid{};
    uint64_t prebuilds{},builds{},updates{},rejected{},shaderLibraries{},instanceCopies{},geometryBytes{};
    uint64_t lastBuildAgeMs{UINT64_MAX},lastHairAgeMs{UINT64_MAX},evictions{};
    uint64_t poolAllocations{},poolReleases{},poolReturns{},fullRebuilds{},hairBlasBytes{},hairScratchBytes{};
    uint64_t buildMicroseconds{},copyMicroseconds{}; // time in DOTS's own hook work since launch
    uint64_t buildLockWaitMicroseconds{},buildLockHeldMicroseconds{}; // parts of buildMicroseconds
    uint64_t copyCalls{},instancesScanned{};          // game TLAS instance copies DOTS inspected
    uint64_t vramUsage{},vramBudget{},sharedUsage{}; // this process on the game's adapter
    uint32_t liveOwners{},hairInstances{};
    bool crashReportRequested{},crashReportArmed{},gameHairTraced{},memoryKnown{};
    char reason[256]{};
};
// Before the renderer creates its device: enables DRED when DOTS is requested.
void BeforeDeviceCreate(const void* caller) noexcept;
void ObserveDevice(IUnknown* object,const void* caller) noexcept;
Snapshot ReadSnapshot() noexcept;
const char* StageText(Stage stage) noexcept;
// Backend line for the overlay: the stage, or once prepared, whether converted
// hair is currently in the scene.
const char* ActivityText(const Snapshot& snapshot) noexcept;
bool SetCrashReportRequested(bool requested) noexcept;
bool GameProcess() noexcept;
}
