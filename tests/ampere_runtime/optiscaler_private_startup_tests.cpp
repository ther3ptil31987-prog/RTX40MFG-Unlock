// optirtx30.1: when OptiScaler's private Streamline (its DLSS-G output) may
// carry the RTX 30 startup itself. Every input combination is checked against
// the rule written out independently below.
#include "optiscaler_private_streamline_policy.h"
#include <cstring>
#include <iostream>
#include <string>

unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

using optiscaler_private_streamline::Decision;
using optiscaler_private_streamline::Inputs;

int wmain()
{
    using namespace optiscaler_private_streamline;

    // GTA III DE (tester log, wmma.2): no game Streamline, private interposer
    // hooked as it mapped with the declaration armed, RTX 30 device.
    Inputs tester{};
    tester.hookedAtLoad = tester.armedAtLoad = true;
    Check(NeedsDevice(tester), "no game Streamline + early private boundary: the device is observed");
    tester.ampereDevice = true;
    Check(Decide(tester) == Decision::eAdmit, "GTA III shape: the private slSetD3DDevice takes the RTX 30 startup");
    Check(RunsStartup(Decide(tester), true), "an admitted boundary runs the startup");

    Inputs game = tester;
    game.gameStreamline = true;
    Check(Decide(game) == Decision::eRefuseGameStreamline, "a game with its own Streamline keeps the RTX 30 startup there");
    game.ampereDevice = false;
    Check(!NeedsDevice(game), "the private call observes no device when the game's Streamline owns the startup");

    Inputs late = tester;
    late.hookedAtLoad = false;
    Check(Decide(late) == Decision::eRefuseLate && !NeedsDevice(late),
        "a private interposer hooked after it mapped (its slInit may have run unhooked) is refused");
    late = tester;
    late.armedAtLoad = false;
    Check(Decide(late) == Decision::eRefuseLate && !NeedsDevice(late),
        "a declaration armed after the interposer mapped (provider may be undeclared) is refused");

    Inputs device = tester;
    device.ampereDevice = false;
    Check(Decide(device) == Decision::eRefuseDevice && !RunsStartup(Decide(device), false),
        "a device that is not the verified RTX 30 adapter runs no startup");

    Inputs repeat{};
    repeat.admittedBefore = true;
    repeat.gameStreamline = true; // set by the first admission's early boundary
    Check(NeedsDevice(repeat), "a repeated private call observes its device like the game's repeated call");
    repeat.ampereDevice = true;
    Check(Decide(repeat) == Decision::eRepeat && RunsStartup(Decision::eRepeat, true),
        "a repeated private call goes through the startup again, as the game's does");
    repeat.ampereDevice = false;
    Check(!RunsStartup(Decide(repeat), false), "a repeated call on another device forwards only");

    // Exhaustive: 32 combinations against the rule as written in the claim.
    unsigned admitted = 0;
    for (unsigned bits = 0; bits < 32; ++bits)
    {
        Inputs in{};
        in.admittedBefore = (bits & 1) != 0;
        in.gameStreamline = (bits & 2) != 0;
        in.hookedAtLoad = (bits & 4) != 0;
        in.armedAtLoad = (bits & 8) != 0;
        in.ampereDevice = (bits & 16) != 0;
        const bool admit = !in.admittedBefore && !in.gameStreamline && in.hookedAtLoad && in.armedAtLoad && in.ampereDevice;
        const bool startup = admit || (in.admittedBefore && in.ampereDevice);
        const Decision decision = Decide(in);
        admitted += decision == Decision::eAdmit;
        Check((decision == Decision::eAdmit) == admit && RunsStartup(decision, in.ampereDevice) == startup,
            "combination " + std::to_string(bits) + " (" + DecisionName(decision) + ")");
        // The device only matters where it is observed.
        if (!NeedsDevice(in))
        {
            Inputs flipped = in;
            flipped.ampereDevice = !in.ampereDevice;
            Check(Decide(flipped) == decision, "combination " + std::to_string(bits) + " does not depend on an unobserved device");
        }
    }
    Check(admitted == 1, "exactly one combination admits a new startup");
    for (const Decision decision : {Decision::eAdmit, Decision::eRepeat, Decision::eRefuseGameStreamline,
             Decision::eRefuseLate, Decision::eRefuseDevice})
        Check(std::strcmp(DecisionName(decision), "unknown") != 0, std::string("decision is named ") + DecisionName(decision));

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
