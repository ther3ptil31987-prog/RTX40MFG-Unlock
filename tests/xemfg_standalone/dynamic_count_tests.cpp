// xemfgsa.6: our OptiScaler fork's simulation, copied from build/v1.5.1-dynamic-20261014/optiscaler-tests for the ported
// controllers (source/native/xess/xefg_dynamic_count.h, xefg_dynamic_cap.h). "OptiScaler" below is the host that
// drives them; under XeMFG standalone that is xess/xemfg_dynamic.h. Was:
// Board #386: simulation tests for OptiScaler's XeFG Dynamic MFG count (framegen/xefg/XeFG_DynamicCount.h).
// A synthetic game: render time R ms per real frame plus g ms per interpolated frame (gGenMs), +-3% deterministic
// jitter. The waits passed are what OptiScaler sees: its low latency / FPS limiter sleep and the present call.
// Caps (the target is not a limit; frames are capped only by the user's FPS limit, V-Sync or an outside limiter):
//  - Limiter: OptiScaler's FPS limit through FPS limiter / XeLL, the sleep is the slack;
//  - Reflex: OptiScaler's FPS limit through Reflex, GPU bound: the sleep also holds the CPU (8 ms) while the GPU works;
//  - VSync: V-Sync at the refresh rate, the game waits in the present call;
//  - VSyncHidden: V-Sync at the refresh rate, the game waits outside OptiScaler (a waitable object), nothing seen;
//  - External: V-Sync or an outside limiter, the game's own low latency sleep still reports ~0.5 ms;
//  - Outside: an outside limiter (refresh 240), ~0.5 ms seen;
//  - None: no cap, the presented rate may go above the target;
//  - GpuBound: no cap, GPU bound: the present call holds the CPU (8 ms) while the GPU works.
#define _CRT_SECURE_NO_WARNINGS // getenv, for the SIM_ROUNDING exploration switch
#include "xess/xefg_dynamic_count.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>

namespace
{
int checks = 0;
int failures = 0;
void Check(bool value, const std::string& name)
{
    ++checks;
    if (!value)
    {
        ++failures;
        std::printf("    FAILED: %s\n", name.c_str());
    }
}

enum class Cap
{
    None,
    Limiter,
    Reflex,
    VSync,
    VSyncHidden,
    External, // held by V-Sync or an outside limiter while the game's low latency sleep still reports ~0.5 ms
    Outside,
    GpuBound
};

struct Result
{
    int finalCount = 0;
    int changes = 0;
    int undos = 0;
    int ups = 0;
    int tries = 0;
    int capped = 0;
    int undoFrom[8] {}; // failed step downs by the count they were from
    double belowShare = 0.0;  // share of time the presented rate was under 93% of the target
    double lockedShare = 0.0; // share of time it was within 3% of the target
    double meanCount = 0.0;
    double reachS = 0.0;     // when the settled count was first reached
    double nearShare = 0.0;  // share of time within 10% of the target
    double overShare = 0.0;  // share of time more than 20% over it
};

// Frame-time noise: +-gNoise uniform, and every gSpikeEvery-th frame 40% longer (0 = none).
double gNoise = 0.03;
int gSpikeEvery = 0;
// Frame generation cost per interpolated frame; OptiScaler's FPS limit when it isn't the cap (0 = none).
double gGenMs = 0.5;
double gFgBaseMs = 0.0; // frame generation's own cost, paid whenever it runs (not at 1X)
int gMinCount = 1;      // 0 allows 1X (frame generation off)
bool gVerbose = false; // print every change
double gLimitFps = 0.0;
// Dynamic MFG's rounding (0 down, 1 nearest = default, 2 up); FG switched off and on every this many seconds (0 = never).
int gRounding = 1;
double gRestartEverySec = 0.0;

Result Run(double seconds, double targetFps, int maxCount, int startCount, Cap cap,
           const std::function<double(double)>& renderMs, double hitchEverySec = 0.0, double capFps = 0.0)
{
    if (capFps <= 0.0)
        capFps = targetFps;
    constexpr double cpuMs = 8.0;
    XeFG_DynamicCount controller;
    Result r;
    int count = startCount;
    int inFlight[4] {};
    long long frameIndex = 0;
    double now = 0.0, below = 0.0, locked = 0.0, weighted = 0.0, near = 0.0, over = 0.0;
    double settled[8] {}; // time at each count over the last 20 s
    double firstAt[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    firstAt[startCount] = 0.0;
    unsigned seed = 12345;
    double nextHitch = hitchEverySec;
    double nextRestart = gRestartEverySec;
    while (now < seconds * 1000.0)
    {
        seed = seed * 1103515245u + 12345u;
        double jitter = 1.0 + gNoise * (static_cast<int>((seed >> 16) % 2001) - 1000) / 1000.0;
        if (gSpikeEvery > 0 && frameIndex % gSpikeEvery == gSpikeEvery - 1)
            jitter *= 1.4;
        const double natural = (renderMs(now / 1000.0) + (count >= 1 ? gFgBaseMs : 0.0) + count * gGenMs) * jitter;
        // The limiter paces the real frames for the count of 3 frames ago (frames in flight)
        inFlight[frameIndex % 4] = count;
        const int limitedCount = frameIndex >= 3 ? inFlight[(frameIndex - 3) % 4] : count;
        ++frameIndex;
        const double capFrame =
            cap == Cap::None || cap == Cap::GpuBound ? 0.0 : 1000.0 * (limitedCount + 1) / capFps;
        const double frame = std::max(natural, capFrame);
        double sleep = 0.0;
        if (cap == Cap::Limiter || cap == Cap::VSync)
            sleep = frame - natural;
        else if (cap == Cap::Reflex || cap == Cap::GpuBound)
            sleep = std::max(frame - cpuMs, 0.0);
        else if (cap == Cap::External || cap == Cap::Outside)
            sleep = 0.5;
        double reported = frame;
        if (hitchEverySec > 0.0 && now >= nextHitch * 1000.0)
        {
            reported = 400.0;
            nextHitch += hitchEverySec;
        }
        now += reported;
        // What OptiScaler passes: the display refresh (a V-Sync cap) and its own FPS limit.
        const double refreshFps =
            cap == Cap::VSync || cap == Cap::VSyncHidden || cap == Cap::External ? capFps : 240.0;
        const double limitFps = cap == Cap::Limiter || cap == Cap::Reflex ? capFps : gLimitFps;
        if (gRestartEverySec > 0.0 && now >= nextRestart * 1000.0)
        {
            controller.Restart();
            nextRestart += gRestartEverySec;
        }
        const int next =
            controller.Update({ now, reported, sleep, targetFps, refreshFps, limitFps, maxCount, count, gRounding, gMinCount });
        if (next != count)
        {
            if (gVerbose)
                std::printf("      %7.2f s  %dx -> %dx  reason %d  presented %.1f  lower %.1f  target %.1f  cost %.2f ms  "
                            "wait %.2f  backoff %.0f\n",
                            now / 1000.0, count + 1, next + 1, static_cast<int>(controller.LastReason()),
                            controller.PresentedFps(), controller.LowerFps(), controller.TargetFps(),
                            controller.FrameCostMs(), controller.WaitShare(), controller.BackoffMs());
            ++r.changes;
            if (controller.LastReason() == XeFG_DynamicCount::Reason::Undo)
            {
                ++r.undos;
                ++r.undoFrom[next]; // an undo goes back to the count the step was from
            }
            if (controller.LastReason() == XeFG_DynamicCount::Reason::Up)
                ++r.ups;
            if (controller.LastReason() == XeFG_DynamicCount::Reason::Probe)
                ++r.tries;
            if (controller.LastReason() == XeFG_DynamicCount::Reason::Capped)
                ++r.capped;
            count = next;
            if (firstAt[count] < 0.0)
                firstAt[count] = now;
        }
        if (now > 3000.0 && reported < 250.0)
        {
            // Measured against the target, or a cap below it
            const double wanted = capFrame > 0.0 ? std::min(targetFps, capFps) : targetFps;
            const double presented = 1000.0 * (count + 1) / frame;
            if (presented < wanted * 0.93)
                below += frame;
            if (std::abs(presented / wanted - 1.0) <= 0.03)
                locked += frame;
            if (std::abs(presented / wanted - 1.0) <= 0.10)
                near += frame;
            if (presented > wanted * 1.2)
                over += frame;
            weighted += frame;
            r.meanCount += count * frame;
            if (now > seconds * 1000.0 - 20000.0)
                settled[count] += frame;
        }
    }
    // The count held longest over the last 20 s, not one being tried at the end
    r.finalCount = static_cast<int>(std::max_element(settled, settled + 8) - settled);
    r.reachS = firstAt[r.finalCount] / 1000.0;
    r.belowShare = weighted > 0 ? below / weighted : 0.0;
    r.lockedShare = weighted > 0 ? locked / weighted : 0.0;
    r.nearShare = weighted > 0 ? near / weighted : 0.0;
    r.overShare = weighted > 0 ? over / weighted : 0.0;
    r.meanCount = weighted > 0 ? r.meanCount / weighted : 0.0;
    return r;
}

void Print(const char* name, const Result& r)
{
    std::printf("  %-46s final %dx at %.1f s, changes %d (up %d, try %d, undo %d, capped %d), at target %.1f%%, "
                "under %.1f%%, mean %.2fx\n",
                name, r.finalCount + 1, r.reachS, r.changes, r.ups, r.tries, r.undos, r.capped,
                r.lockedShare * 100.0, r.belowShare * 100.0, r.meanCount + 1.0);
}
} // namespace

int main()
{
    // Exploration: SIM_ROUNDING=0|2 runs every case with another default rounding (expectations assume nearest)
    if (const char* e = std::getenv("SIM_ROUNDING"))
        gRounding = std::atoi(e);
    if (const char* e = std::getenv("SIM_MINCOUNT"))
        gMinCount = std::atoi(e);
    const int defaultMinCount = gMinCount;
    const int defaultRounding = gRounding;

    auto constant = [](double ms) { return [ms](double) { return ms; }; };

    // Held at 120 by the limiter, 50 fps game: 2x gives ~96, 3x ~143 -> 3x, locked at 120.
    auto r = Run(60, 120, 5, 5, Cap::Limiter, constant(20.0));
    Print("limiter 120, 50 fps game", r);
    Check(r.finalCount == 2 && r.undos <= 1 && r.lockedShare > 0.95, "limiter: 3x, locked at the target");

    // Held at 120, load changes: 125 fps -> 33 fps -> 83 fps.
    // Rounding to nearest holds 3x (~99, 82%) through the heavy part, as NVIDIA's would; its 3x -> 2x tries fail
    // there, and once the game is lighter 3x presents 15% more, which lifts their back-off.
    r = Run(90, 120, 5, 5, Cap::Limiter, [](double t) { return t < 30 ? 8.0 : t < 60 ? 30.0 : 12.0; });
    Print("limiter 120, 125 -> 33 -> 83 fps", r);
    Check(r.finalCount == 1 && r.undos <= 3, "back to 2x when the load drops");
    // Rounding up keeps the target across the load changes (4x in the heavy part).
    gRounding = 2;
    r = Run(90, 120, 5, 5, Cap::Limiter, [](double t) { return t < 30 ? 8.0 : t < 60 ? 30.0 : 12.0; });
    Print("limiter 120, 125 -> 33 -> 83 fps, up", r);
    Check(r.finalCount == 1 && r.belowShare < 0.05 && r.lockedShare > 0.9, "up: locked at the target across load changes");
    gRounding = defaultRounding;

    // Held at 120 by the limiter, 100 fps game: 2x on measured slack, no undo.
    r = Run(60, 120, 5, 5, Cap::Limiter, constant(10.0));
    Print("limiter 120, 100 fps game", r);
    Check(r.finalCount == 1 && r.undos == 0 && r.lockedShare > 0.9, "limited: steps down to 2x on measured slack");

    // Reflex, GPU bound 34 fps game (Subnautica-like): 4x holds 120, its sleep promises more -> few undos.
    r = Run(300, 120, 5, 3, Cap::Reflex, constant(28.0));
    Print("reflex 120, GPU bound 34 fps (5 min)", r);
    Check(r.finalCount == 3 && r.undos <= 10, "reflex: stays at 4x with back-off on the overstated sleep");
    Check(r.belowShare < 0.03 && r.lockedShare > 0.95, "reflex: locked at the target");

    // Subnautica 2 (14 Oct, dynamic3): Reflex cap 117, GPU bound ~26 real fps at 6x (R 36 ms). 5x needs 23.4 and
    // gets ~26.3: it must leave 6x and hold 5x; 4x needs 29.3 and gets ~26.7, so its tries are undone.
    r = Run(300, 117, 5, 5, Cap::Reflex, constant(36.0));
    Print("reflex 117, Subnautica-like 26 fps (5 min)", r);
    Check(r.finalCount == 4 && r.meanCount > 3.8 && r.meanCount < 4.3, "subnautica-like: holds 5x, not 6x");
    Check(r.lockedShare > 0.95 && r.undos <= 10, "subnautica-like: locked, failed 4x tries back off");

    // Reflex, GPU bound, the game gets lighter after 150 s (34 -> 53 fps): reaches 3x.
    r = Run(300, 120, 5, 3, Cap::Reflex, [](double t) { return t < 150 ? 28.0 : 18.0; });
    Print("reflex 120, 34 -> 53 fps", r);
    Check(r.finalCount == 2 && r.lockedShare > 0.95, "reflex: steps down once the game is lighter");

    // V-Sync at 120, 100 fps game: the wait shows in the present call -> 2x.
    r = Run(60, 120, 5, 5, Cap::VSync, constant(10.0));
    Print("v-sync 120 (present wait), 100 fps game", r);
    Check(r.finalCount == 1 && r.undos == 0, "v-sync: steps down to 2x");

    // V-Sync at 120 waited outside OptiScaler, 100 fps game: tries reach 2x.
    r = Run(90, 120, 5, 5, Cap::VSyncHidden, constant(10.0));
    Print("v-sync 120 (hidden wait), 100 fps game", r);
    Check(r.finalCount == 1 && r.undos == 0, "hidden v-sync: tries reach 2x");

    // No low latency input, so no cap: 50 fps game, 2x gives ~98 (18 under 116), 3x ~143 (27 over): nearest is 2x
    // (dynamic15; dynamic14 kept 3x).
    r = Run(60, 116, 5, 5, Cap::None, constant(20.0));
    Print("uncapped 116, 50 fps game", r);
    Check(r.finalCount == 1 && r.undos <= 2, "uncapped: nearest is 2x");

    // Hitches (400 ms every 3 s) are ignored.
    r = Run(60, 120, 5, 5, Cap::Limiter, constant(20.0), 3.0);
    Print("limiter 120, 50 fps game, hitches", r);
    Check(r.finalCount == 2 && r.undos <= 1, "hitches don't move the count");

    // Tester report (beta.3 + dynamic5): with V-Sync or a limiter at the target, no step down from 4x-6x.
    // Held at 117 by an outside limiter, the game's own low latency sleep is short: 100 fps game reaches 2x.
    r = Run(120, 117, 5, 5, Cap::External, constant(10.0));
    Print("outside limiter 117 = target, 100 fps game", r);
    Check(r.finalCount == 1 && r.lockedShare > 0.9, "outside limiter at the target: steps down to 2x");

    // V-Sync at 144 above the auto target 138: the presented rate sits above the target, 100 fps game.
    r = Run(120, 138, 5, 5, Cap::External, constant(10.0), 0.0, 144);
    Print("v-sync 144 above target 138, 100 fps game", r);
    Check(r.finalCount == 1, "a cap above the target: steps down to 2x");

    // Outside limiter 117, Subnautica-like (5x holds 117, 4x doesn't): leaves 6x, holds 5x, failed 4x tries back off.
    r = Run(300, 117, 5, 5, Cap::External, constant(36.0));
    Print("outside limiter 117, Subnautica-like (5 min)", r);
    Check(r.finalCount == 4 && r.meanCount > 3.8 && r.undos <= 10, "outside limiter: holds 5x");

    // Uncapped, a game at ~38 real fps: 3x gives ~115 for a 116 target (within the 3% up band), 4x ~151.
    r = Run(60, 116, 5, 5, Cap::None, constant(25.0));
    Print("uncapped 116, 38 fps game", r);
    Check(r.finalCount == 2 && r.undos <= 3, "uncapped: comes down to 3x when 3x reaches the target");

    // Tester (Riven, RTX 30, dynamic6): 90 fps without frame generation, ~59 real fps at 2x (2x ~118 presented):
    // generating costs ~6 ms per real frame plus ~1.5 ms per extra frame, so one step less raises the real rate.
    // "It should be able to get back to 2x super often, never does."
    gGenMs = 1.5;
    r = Run(60, 116, 5, 5, Cap::None, constant(15.5));
    Print("tester: uncapped 116, 2x gives 118", r);
    Check(r.finalCount == 1 && r.undos == 0, "tester uncapped: 2x");

    // 2x 118 is 20 under 138, 3x 162 is 24 over: nearest is 2x (dynamic14 kept 3x)
    r = Run(120, 138, 5, 5, Cap::None, constant(15.5));
    Print("tester: uncapped 138, 2x gives 118, 3x 162", r);
    Check(r.finalCount == 1 && r.undos <= 2, "tester uncapped 138: nearest is 2x");

    r = Run(60, 116, 5, 5, Cap::VSync, constant(15.5), 0.0, 120);
    Print("tester: v-sync 120 (present wait), target 116", r);
    Check(r.finalCount == 1 && r.undos == 0, "tester v-sync 120: 2x");

    r = Run(90, 116, 5, 5, Cap::VSyncHidden, constant(15.5), 0.0, 120);
    Print("tester: v-sync 120 (hidden wait), target 116", r);
    Check(r.finalCount == 1 && r.undos == 0, "tester hidden v-sync 120: 2x");

    r = Run(90, 138, 5, 5, Cap::VSync, constant(15.5), 0.0, 144);
    Print("tester: v-sync 144 (present wait), target 138", r);
    Check(r.finalCount == 2 && r.undos <= 4, "tester v-sync 144: 3x");

    r = Run(90, 138, 5, 5, Cap::VSyncHidden, constant(15.5), 0.0, 144);
    Print("tester: v-sync 144 (hidden wait), target 138", r);
    Check(r.finalCount == 2 && r.undos <= 4, "tester hidden v-sync 144: 3x");

    // A cap below the target: an outside limiter at 120 with a 138 target. Nothing shows it at 6x; a step down that
    // keeps the rate shows it, the rate held counts as the target, and 2x (118) holds it.
    r = Run(120, 138, 5, 5, Cap::Outside, constant(15.5), 0.0, 120);
    Print("tester: outside limiter 120 under target 138", r);
    Check(r.finalCount == 1 && r.belowShare < 0.03, "a cap under the target: 2x");

    // "If OptiScaler's limiter doesn't match V-Sync": FPS limit 160 (so target 160), V-Sync at 144 -> 3x.
    gLimitFps = 160;
    r = Run(120, 160, 5, 5, Cap::VSync, constant(15.5), 0.0, 144);
    Print("tester: limit 160 over v-sync 144", r);
    Check(r.finalCount == 2, "an FPS limit above v-sync: 3x");
    r = Run(120, 160, 5, 5, Cap::VSyncHidden, constant(15.5), 0.0, 144);
    Print("tester: limit 160 over v-sync 144 (hidden wait)", r);
    Check(r.finalCount == 2, "an FPS limit above hidden v-sync: 3x");
    gLimitFps = 0;
    gGenMs = 0.5;

    // Tester's OptiScaler.log (Riven, RTX 30, dynamic6, 15 Oct): FPS limit 156.65 is the target, V-Sync on. Real
    // frame time 16.3 ms at 2x, 22.5 at 3x, 27.2 at 4x, 35.3 at 5x: each generated frame costs ~6 ms, so 2x gives
    // ~123, 3x ~134, 4x ~141, 5x ~146, 6x ~149 and the target is out of reach. dynamic6 climbed to 6x and stayed.
    // A frame that earns under 30% of its free share isn't worth it: 2x, from either end.
    gGenMs = 6.0;
    r = Run(180, 156.6, 5, 1, Cap::Limiter, constant(10.3), 0.0, 156.6);
    Print("tester log: limit 156.6 out of reach, from 2x", r);
    Check(r.finalCount == 1 && r.meanCount < 1.3, "costly frames: stays at 2x");
    r = Run(180, 156.6, 5, 5, Cap::Limiter, constant(10.3), 0.0, 156.6);
    Print("tester log: limit 156.6 out of reach, from 6x", r);
    Check(r.finalCount == 1, "costly frames: comes down to 2x from 6x");
    r = Run(180, 157, 5, 5, Cap::None, constant(10.3));
    Print("tester log: uncapped 157 out of reach, from 6x", r);
    Check(r.finalCount == 1, "costly frames uncapped: comes down to 2x");
    gGenMs = 0.5;

    // Tester's second log (dynamic7): "a lot better, a bit slow to drop down". Target = FPS limit 156.6 through
    // XeLL, GPU bound (the sleep holds the CPU, "one step less" reads 250-1100 fps), 2x ~110, 3x ~152-157 against the
    // 151.9 bar, noisy frame times (LogLevel 0). From 6x it took ~23 s to reach 3x: every dip under the bar
    // restarted the step-down hold.
    gGenMs = 0.9;
    gNoise = 0.08;
    gSpikeEvery = 15;
    r = Run(180, 156.6, 5, 5, Cap::Reflex, constant(16.8), 0.0, 156.6);
    Print("tester log 2: limit 156.6 = target, noisy, from 6x", r);
    Check(r.finalCount == 2 && r.reachS < 12.0 && r.undos <= 8, "target at the cap: 6x to 3x within 12 s");
    const int undosNoRestart = r.undos;

    // Tester's third log (dynamic8): FG switched off and on 5 times in 4 min, and each time the controller forgot its
    // failed 3x -> 2x tries and started them again. It now keeps what it learned.
    gRestartEverySec = 25.0;
    r = Run(180, 156.6, 5, 5, Cap::Reflex, constant(16.8), 0.0, 156.6);
    Print("tester log 3: same, FG off and on every 25 s", r);
    Check(r.finalCount == 2 && r.undos <= undosNoRestart + 1, "FG off and on: failed tries aren't repeated");
    gRestartEverySec = 0.0;

    // Tester's third log: "it should have some leeway: target 120 but output only 110, don't up the multiplier".
    // Rounding to nearest (NVIDIA's default) holds 3x from 79% of the target: no up steps to 4x and back.
    r = Run(180, 156.6, 5, 5, Cap::Reflex, constant(16.8), 0.0, 156.6);
    Print("tester log 3: rounding nearest", r);
    // Its 2x tries (~107 for the 112 its band needs) fall short: a few in 3 min.
    Check(r.finalCount == 2 && r.ups == 0 && r.undos <= 4, "nearest: 3x holds without up steps");
    gNoise = 0.03;
    gSpikeEvery = 0;
    gGenMs = 0.5;

    // Rounding modes without a cap, target 120, 2x gives ~95 (25 under), 3x ~139 (19 over). Nearest goes to 3x
    // (dynamic14's band held 2x from 71%), down keeps 2x (the highest at or below it), up goes to 3x (the lowest at
    // or above it).
    r = Run(60, 120, 5, 1, Cap::None, constant(20.55));
    Print("target 120, 2x gives 95, nearest, from 2x", r);
    Check(r.finalCount == 2 && r.changes == 1, "nearest: 95 for 120 goes to 3x (139)");
    gRounding = 0;
    r = Run(60, 120, 5, 1, Cap::None, constant(20.55));
    Print("target 120, 2x gives 95, down, from 2x", r);
    Check(r.finalCount == 1 && r.changes == 0, "down: 95 for 120 holds 2x");
    gRounding = 2;
    r = Run(60, 120, 5, 1, Cap::None, constant(20.55));
    Print("target 120, 2x gives 95, up, from 2x", r);
    Check(r.finalCount == 2, "up: 95 for 120 goes to 3x");
    gRounding = defaultRounding;
    // The tester's own example: target 120, 2x gives 110 holds 2x in every mode.
    r = Run(60, 120, 5, 5, Cap::None, constant(17.7));
    Print("target 120, 2x gives 110, nearest, from 6x", r);
    Check(r.finalCount == 1, "nearest: 110 for 120 comes down to 2x");

    // Subnautica 2 run (15 Oct, dynamic11): FPS limit 116 = target through Reflex, GPU bound (the waits promise far
    // more than the GPU gives), each frame ~4 ms. The load swings every 20 s between 3X enough (~104-114) and 4X
    // needed; 2X (~80-90) never holds. Each lighter spell's 4X -> 3X held and cleared the shared wait, so a doomed
    // 3X -> 2X followed 3 s later (13 of 20 undos in 8 min). Each step down now keeps its own wait.
    gGenMs = 4.0;
    gVerbose = std::getenv("SIM_TRACE") != nullptr;
    // Mostly light (3X ~114 holds) with short heavy spells (6 s of 30 s, 4X needed): each light spell's 4X -> 3X holds.
    r = Run(300, 116, 5, 3, Cap::Reflex, [](double t) { return std::fmod(t, 30.0) < 24.0 ? 18.0 : 26.0; }, 0.0, 116);
    gVerbose = false;
    Print("subnautica run: short heavy spells, 2x never", r);
    std::printf("    failed steps from 4x %d, from 3x %d\n", r.undoFrom[3], r.undoFrom[2]);
    Check(r.undoFrom[2] <= 6, "3X -> 2X keeps its own wait when 4X -> 3X holds");
    Check(r.undoFrom[3] <= 2, "after an up step that helped, no step back down at the same load");
    gGenMs = 0.5;

    // 1X (frame generation off), as NVIDIA's Dynamic MFG goes down to. Frame generation costs 3 ms per real frame
    // plus 1 ms per generated frame here; turning it off saves all of it.
    gMinCount = 0;
    gFgBaseMs = 3.0;
    gGenMs = 1.0;
    // 125 fps without frame generation, uncapped target 116: 1X.
    r = Run(90, 116, 5, 5, Cap::None, constant(8.0));
    Print("1x: uncapped 116, 125 fps without FG, from 6x", r);
    Check(r.finalCount == 0, "1x: a game over the target turns frame generation off");
    gMinCount = 1;
    r = Run(90, 116, 5, 5, Cap::None, constant(8.0));
    Print("1x not allowed: same game", r);
    Check(r.finalCount == 1, "1x not allowed: stays at 2x");
    gMinCount = 0;
    // FPS limit 120 = target, 150 fps without frame generation: 1X held at the limit.
    r = Run(90, 120, 5, 5, Cap::Limiter, constant(6.67));
    Print("1x: limit 120, 150 fps without FG, from 6x", r);
    Check(r.finalCount == 0 && r.lockedShare > 0.8, "1x: frame generation off at the limit");
    // The tester's GTA5E with NVIDIA: target 137, 92 fps without frame generation, at 1X: nearest holds it (67%).
    r = Run(60, 137, 5, 0, Cap::None, constant(10.87));
    Print("1x: target 137, 92 fps without FG, from 1x", r);
    Check(r.finalCount == 0 && r.changes == 0, "1x: 92 for 137 holds 1x, as NVIDIA's did");
    // Costly frame generation (the RTX 30 tester: 90 fps without, ~118 at 2X): out of reach at 156.6, and 1X would
    // lose 23% of the rate where a frame has to keep 85%: stays at 2X.
    gFgBaseMs = 4.4;
    gGenMs = 1.5;
    r = Run(120, 156.6, 5, 1, Cap::Limiter, constant(11.1), 0.0, 156.6);
    Print("1x: costly FG, 156.6 out of reach, from 2x", r);
    Check(r.finalCount == 1, "1x: not when it loses too much of the rate");
    gMinCount = defaultMinCount;
    gFgBaseMs = 0.0;
    gGenMs = 0.5;

    // Out of reach but each frame is cheap: a 25 fps game, 6x gives ~144 for 160; 5x (120) loses too much.
    r = Run(300, 160, 5, 5, Cap::None, constant(40.0));
    Print("heavy game, 160 out of reach (5 min)", r);
    Check(r.finalCount == 5 && r.undos <= 10, "cheap frames out of reach: stays at 6x, tries back off");
    r = Run(300, 160, 5, 1, Cap::None, constant(40.0));
    Print("heavy game, 160 out of reach, from 2x (5 min)", r);
    Check(r.finalCount == 5, "cheap frames out of reach: climbs to 6x");

    // Uncapped and GPU bound: the present call holds the CPU, which promises more than the GPU gives; without a cap
    // the whole frame time counts. 34 fps game, target 116: 3x 103 (13 under) is nearer than 4x 136 (20 over).
    r = Run(300, 116, 5, 5, Cap::GpuBound, constant(28.0));
    Print("uncapped GPU bound 116, 34 fps game (5 min)", r);
    Check(r.finalCount == 2 && r.undos <= 2, "GPU bound: nearest is 3x, no failed tries");

    // Real frame-time noise (+-15%, every 15th frame 40% longer), uncapped 50 fps game: still comes down to 2x.
    gNoise = 0.15;
    gSpikeEvery = 15;
    r = Run(60, 116, 5, 5, Cap::None, constant(20.0));
    Print("uncapped 116, 50 fps game, noisy", r);
    Check(r.finalCount == 1, "noisy frame times: comes down to the nearest, 2x");
    r = Run(120, 117, 5, 5, Cap::Limiter, constant(10.0));
    Print("limiter 117, 100 fps game, noisy", r);
    Check(r.finalCount == 1, "noisy frame times under a limiter: comes down to 2x");
    gGenMs = 1.5;
    r = Run(120, 116, 5, 5, Cap::VSyncHidden, constant(15.5), 0.0, 120);
    Print("tester: hidden v-sync 120, noisy", r);
    Check(r.finalCount == 1, "noisy frame times under hidden v-sync: comes down to 2x");
    gGenMs = 0.5;
    gNoise = 0.03;
    gSpikeEvery = 0;

    // dynamic15, without a cap: the count expected nearest what the rounding asks for, wherever it came from.
    // Users on dynamic14: "unwilling to step up or down, runs far from the target". Its band kept 3x at 123% of the
    // target from above and 2x at 84% from below for the same game.
    {
        // How far a presented rate is from what the rounding asks for (the header's Miss)
        const auto miss = [](double fps, double target, int rounding)
        {
            const double x = fps / target;
            if (rounding == 2)
                return x >= 0.97 ? x - 0.97 : 1.0 + 0.97 - x;
            if (rounding == 0)
                return x <= 1.03 ? 1.03 - x : 1.0 + x - 1.03;
            return std::abs(x - 1.0);
        };
        const auto presentedAt = [](int count, double renderMs) { return 1000.0 * (count + 1) / (renderMs + count * gGenMs); };
        const char* names[] = { "down", "nearest", "up" };

        for (int rounding = 0; rounding < 3; ++rounding)
        {
            gRounding = rounding;
            double worst = 0.0;
            int differ = 0;

            for (double base : { 25.0, 30.0, 35.0, 40.0, 45.0, 50.0, 55.0, 60.0, 70.0, 80.0 })
            {
                const double renderMs = 1000.0 / base;
                double best = 1e9;
                for (int n = 1; n <= 5; ++n)
                    best = std::min(best, miss(presentedAt(n, renderMs), 116.0, rounding));

                const Result low = Run(60, 116, 5, 1, Cap::None, constant(renderMs));
                const Result high = Run(60, 116, 5, 5, Cap::None, constant(renderMs));
                worst = std::max(worst, miss(presentedAt(low.finalCount, renderMs), 116.0, rounding) - best);
                worst = std::max(worst, miss(presentedAt(high.finalCount, renderMs), 116.0, rounding) - best);
                differ += low.finalCount != high.finalCount;
            }

            std::printf("  %-46s worst %.3f past the best count, %d of 10 games settle apart from 2x and 6x\n",
                        (std::string("uncapped 116, 25-80 fps games, ") + names[rounding]).c_str(), worst, differ);
            // Inside the margin (a step has to land 3% of the target nearer)
            Check(worst <= 0.035 && differ <= 1,
                  std::string("uncapped ") + names[rounding] + ": the best count from 2x and from 6x");
        }
        gRounding = defaultRounding;

        // What each rounding means, 55 fps game, target 116: 2x 107 (92%), 3x 156 (135%)
        r = Run(60, 116, 5, 5, Cap::None, constant(18.2));
        Print("uncapped 116, 55 fps game, nearest, from 6x", r);
        Check(r.finalCount == 1, "nearest: 2x (107) rather than 3x (156)");
        gRounding = 2;
        r = Run(60, 116, 5, 1, Cap::None, constant(18.2));
        Print("uncapped 116, 55 fps game, up, from 2x", r);
        Check(r.finalCount == 2, "up: 3x, the lowest at or above the target");
        r = Run(60, 116, 5, 5, Cap::None, constant(15.5));
        Print("uncapped 116, 64 fps game, up, from 6x", r);
        Check(r.finalCount == 1, "up: 2x (127) reaches the target, no more");
        gRounding = 0;
        r = Run(60, 116, 5, 5, Cap::None, constant(22.2));
        Print("uncapped 116, 45 fps game, down, from 6x", r);
        Check(r.finalCount == 1, "down: 2x (88), the highest at or below the target");
        gRounding = defaultRounding;

        // Load swing: 70 -> 25 -> 70 fps over 4 min. dynamic14: 44% of the time within 10% of the target, 9.5% more
        // than 20% over it.
        r = Run(240, 116, 5, 1, Cap::None,
                [](double t) { return 1000.0 / (47.5 + 22.5 * std::cos(t / 240.0 * 2.0 * 3.14159265358979)); });
        Print("uncapped 116, 70 -> 25 -> 70 fps (4 min)", r);
        std::printf("    within 10%% of the target %.1f%%, over 120%% %.1f%%\n", r.nearShare * 100.0, r.overShare * 100.0);
        Check(r.nearShare > 0.55 && r.overShare < 0.02 && r.changes <= 14, "load swing: nearer the target");

        // Back from a heavy spell: 50 -> 30 -> 50 fps. dynamic14 stayed at 3x (143) after it.
        r = Run(180, 116, 5, 1, Cap::None, [](double t) { return t < 60 ? 20.0 : t < 120 ? 1000.0 / 30.0 : 20.0; });
        Print("uncapped 116, 50 -> 30 -> 50 fps", r);
        Check(r.finalCount == 1, "back from a heavy spell: 2x again");

        // GPU bound, 1 ms per generated frame, 45 fps game: 3x 124 (107%) is nearer than 2x 86 (74%), and its frame
        // earns its share. dynamic14 held 2x.
        gGenMs = 1.0;
        r = Run(120, 116, 5, 1, Cap::GpuBound, constant(22.2));
        Print("uncapped GPU bound 116, 1 ms frames, from 2x", r);
        Check(r.finalCount == 2 && r.undos == 0, "GPU bound: up to the nearest, 3x");
        gGenMs = 0.5;
    }

    // A 2x maximum never changes; no target keeps the count.
    r = Run(20, 120, 1, 1, Cap::Limiter, constant(30.0));
    Check(r.changes == 0 && r.finalCount == 1, "a 2x maximum stays 2x");
    r = Run(20, 0, 5, 4, Cap::None, constant(30.0));
    Check(r.changes == 0 && r.finalCount == 4, "no target keeps the count");

    if (failures > 0)
    {
        std::printf("FAIL: %d of %d checks\n", failures, checks);
        return 1;
    }

    std::printf("PASS: XeFG dynamic count (%d checks)\n", checks);
    return 0;
}
