#include "ampere_policy.h"
#include <algorithm>
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    using ampere_policy::DecideOptions;
    using Mode = ampere_policy::OptionsMode;
    // Follow game: the menu stores multiplier 2 for Follow game.
    auto d = DecideOptions(true, 2, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2 && !d.gameDynamicTarget,
        "Oblivion: Follow game keeps the game's 3x instead of the stored 2x");
    d = DecideOptions(true, 2, false, Mode::eOn, 3, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 3, "Follow game keeps the game's fixed 4x");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 6, true);
    Check(d.mode == Mode::eDynamic && d.generatedFrames == 5 && d.gameDynamicTarget,
        "Follow game keeps the game's Dynamic request and its target");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 5 && !d.gameDynamicTarget,
        "game Dynamic without full-capacity proof becomes fixed at the game's count");
    d = DecideOptions(true, 2, false, Mode::eDynamic, 5, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game Dynamic on an uncertified route stays 2x");
    d = DecideOptions(true, 2, false, Mode::eOn, 5, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game 6x is limited to the 2x safe maximum");
    d = DecideOptions(true, 2, false, Mode::eOn, 7, 6, false);
    Check(d.generatedFrames == 5, "game count above the six-buffer program is limited to 6x");
    d = DecideOptions(true, 2, false, Mode::eOn, 0, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "game count 0 is submitted as 2x");
    d = DecideOptions(true, 2, false, Mode::eOff, 2, 6, true);
    Check(d.mode == Mode::eOff && !d.gameDynamicTarget, "game Off stays Off");
    d = DecideOptions(true, 1, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2, "Follow game ignores a stale stored multiplier of 1");
    d = DecideOptions(true, 6, true, Mode::eOn, 2, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 2 && !d.gameDynamicTarget,
        "Follow game ignores a stale stored Dynamic choice");
    // Fixed and Dynamic menu choices replace the game's request.
    d = DecideOptions(false, 4, false, Mode::eDynamic, 5, 6, true);
    Check(d.mode == Mode::eOn && d.generatedFrames == 3 && !d.gameDynamicTarget, "fixed 4x replaces game Dynamic");
    d = DecideOptions(false, 4, false, Mode::eOn, 1, 2, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 1, "fixed 4x on a 2x safe maximum submits 2x");
    d = DecideOptions(false, 1, false, Mode::eOn, 2, 6, false);
    Check(d.mode == Mode::eOff, "fixed 1x is Off");
    d = DecideOptions(false, 4, false, Mode::eOff, 2, 6, false);
    Check(d.mode == Mode::eOff, "game Off overrides a fixed choice");
    d = DecideOptions(false, 6, true, Mode::eOn, 1, 6, true);
    Check(d.mode == Mode::eDynamic && d.generatedFrames == 5 && !d.gameDynamicTarget,
        "menu Dynamic uses the menu target");
    d = DecideOptions(false, 6, true, Mode::eOn, 1, 6, false);
    Check(d.mode == Mode::eOn && d.generatedFrames == 5, "menu Dynamic without proof stays fixed");
    // Fixed and Dynamic choices match the previous submission exactly.
    bool same = true, bounded = true;
    for (uint32_t safe = 2; safe <= 6; ++safe)
        for (uint32_t multiplier = 0; multiplier <= 7; ++multiplier)
            for (int dynamic = 0; dynamic < 2; ++dynamic)
                for (int supported = 0; supported < 2; ++supported)
                    for (Mode game : {Mode::eOff, Mode::eOn, Mode::eDynamic})
                    {
                        const auto fixed = DecideOptions(false, multiplier, dynamic, game, 3, safe, supported);
                        const uint32_t effective = std::clamp(multiplier, 1u, safe);
                        const Mode previous = multiplier == 1 || game == Mode::eOff ? Mode::eOff
                            : dynamic && supported ? Mode::eDynamic : Mode::eOn;
                        same = same && fixed.mode == previous && !fixed.gameDynamicTarget
                            && fixed.generatedFrames == std::max(effective, 2u) - 1u;
                        for (uint32_t frames = 0; frames <= 7; ++frames)
                        {
                            const auto follow = DecideOptions(true, multiplier, dynamic, game, frames, safe, supported);
                            bounded = bounded && follow.generatedFrames >= 1
                                && follow.generatedFrames <= std::max(safe, 2u) - 1u
                                && follow.gameDynamicTarget == (follow.mode == Mode::eDynamic)
                                && (follow.mode != Mode::eDynamic || supported)
                                && (follow.mode == Mode::eOff) == (game == Mode::eOff);
                        }
                    }
    Check(same, "fixed and menu Dynamic decisions match the previous submission for every input");
    Check(bounded, "Follow game never exceeds the safe maximum or admits unproven Dynamic");
    // The maximum the game is offered (numFramesToGenerateMax).
    using ampere_policy::AdvertisedGeneratedFrames;
    Check(AdvertisedGeneratedFrames(5, 4) == 3,
        "Where Winds Meet: a 4X title is offered 4x, not the 6x the options path would clamp");
    Check(AdvertisedGeneratedFrames(5, 6) == 5, "a 6X title is offered 6x");
    Check(AdvertisedGeneratedFrames(5, 2) == 1, "a title without an NVIDIA MFG override is offered 2x");
    Check(AdvertisedGeneratedFrames(1, 6) == 1, "a legacy 2x wrapper is offered 2x on any title");
    Check(AdvertisedGeneratedFrames(0, 6) == 1, "no startup capacity is offered 2x");
    bool matches = true;
    for (uint32_t policy = 0; policy <= 7; ++policy)
    {
        const uint32_t offered = AdvertisedGeneratedFrames(5, policy);
        // Once certified, Follow game must submit what the game was offered.
        const auto follow = DecideOptions(true, 2, false, Mode::eOn, offered, std::clamp(policy, 2u, 6u), false);
        matches = matches && offered >= 1 && offered <= 5 && follow.generatedFrames == offered;
    }
    Check(matches, "every offered maximum is submitted unclamped once the route is certified");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
