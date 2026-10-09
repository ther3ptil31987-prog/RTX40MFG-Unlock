#pragma once
#include "game_profile.h"
#include <array>
#include <cstdint>
#include <string>
namespace witcher_dots::profile {
// A witcher3.exe without an exact profile (a newer game update): every site
// DOTS reads, hooks or patches is found by structure instead of by RVA. Each
// step must find exactly one candidate, or discovery fails and DOTS stays off
// (the game then reports no LSS and greys out Path Traced Hair). The result is
// an ordinary GameProfile that ValidateMapped checks like an exact one.
struct Discovered {
    GameProfile profile{};
    std::array<Patch,2> gates{};          // RTAO hair variant, logical LSS getter (last)
    uint32_t lssByte{};                   // the renderer's LSS capability byte
    uint32_t versionMs{},versionLs{};     // the executable's file version
    std::array<uint8_t,32> builderShape{}; // normalized hair builder code
    uint32_t microseconds{};              // discovery time
    std::array<uint32_t,9> phases{};      // elapsed microseconds after steps 1-9
    char label[96]{};
};
// Pure discovery on a mapped image (loaded or unrelocated); no global state.
bool Discover(HMODULE image,Discovered& out,std::string& error) noexcept;
// The running game's discovered profile, found once per process (null: error says why).
const GameProfile* DiscoveredProfile(HMODULE image,std::string& error) noexcept;
// The sites behind DiscoveredProfile once it succeeded (null otherwise).
const Discovered* DiscoveredSites() noexcept;
}
