// xemfgsa.6: our OptiScaler fork's simulation, copied from build/v1.5.1-dynamic-20261014/optiscaler-tests for the ported
// controllers (source/native/xess/xefg_dynamic_count.h, xefg_dynamic_cap.h). "OptiScaler" below is the host that
// drives them; under XeMFG standalone that is xess/xemfg_dynamic.h. Was:
// Board #427: simulation of OptiScaler's XeFG Dynamic MFG with "Use target as cap" (framegen/xefg/XeFG_DynamicCap.h,
// beta.2's dynamic5 controller), against dynamic12's XeFG_DynamicCount under the same cap (limitFps = target).
// A synthetic game: render time R ms per real frame plus g ms per interpolated frame, +-3% deterministic jitter.
// The cap is OptiScaler's FPS limit at the target; the limiter paces the real frames for the count of 3 frames ago.
//  - Limiter: FPS limiter / XeLL, the sleep is the slack;
//  - Reflex: Reflex, GPU bound: the sleep also holds the CPU while the GPU works, and the CPU part left over swings
//    (gCpuMinMs..gCpuMaxMs), so the reachable rate it implies is overstated and noisy (Subnautica 2 log: waits 96-99%).
// Measured after 3 s: share of time within 3% of the target ("locked"), more than 3% over, more than 7% under.
#define _CRT_SECURE_NO_WARNINGS
#include "xess/xefg_dynamic_cap.h"
#include "xess/xefg_dynamic_count.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <type_traits>

namespace
{
int checks = 0;
int failures = 0;
bool gVerbose = false;
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
    Limiter,
    Reflex
};

struct Result
{
    int finalCount = 0;
    int changes = 0;
    int undos = 0;
    double lockedShare = 0.0;
    double overShare = 0.0;
    double underShare = 0.0;
    double meanCount = 0.0;
};

double gNoise = 0.03;
double gGenMs = 0.5;
double gCpuMinMs = 0.5;
double gCpuMaxMs = 8.0;

template <class Controller>
Result Run(double seconds, double targetFps, int maxCount, int startCount, Cap cap,
           const std::function<double(double)>& renderMs)
{
    Controller controller;
    Result r;
    int count = startCount;
    int inFlight[4] {};
    long long frameIndex = 0;
    double now = 0.0, locked = 0.0, over = 0.0, under = 0.0, weighted = 0.0;
    double settled[8] {};
    unsigned seed = 12345;
    while (now < seconds * 1000.0)
    {
        seed = seed * 1103515245u + 12345u;
        const double jitter = 1.0 + gNoise * (static_cast<int>((seed >> 16) % 2001) - 1000) / 1000.0;
        seed = seed * 1103515245u + 12345u;
        const double cpuMs = gCpuMinMs + (gCpuMaxMs - gCpuMinMs) * ((seed >> 16) % 1001) / 1000.0;
        const double natural = (renderMs(now / 1000.0) + count * gGenMs) * jitter;
        inFlight[frameIndex % 4] = count;
        const int limitedCount = frameIndex >= 3 ? inFlight[(frameIndex - 3) % 4] : count;
        ++frameIndex;
        const double capFrame = 1000.0 * (limitedCount + 1) / targetFps;
        const double frame = std::max(natural, capFrame);
        const double sleep = cap == Cap::Limiter ? frame - natural : std::max(frame - cpuMs, 0.0);
        now += frame;

        int next = count;
        bool undo = false;
        if constexpr (std::is_same_v<Controller, XeFG_DynamicCap>)
        {
            next = controller.Update({ now, frame, sleep, targetFps, maxCount, count });
            undo = controller.LastReason() == XeFG_DynamicCap::Reason::Undo;
        }
        else
        {
            // dynamic12 under the cap: rounding nearest, 1X allowed (its defaults), the cap passed as the FPS limit
            next = controller.Update({ now, frame, sleep, targetFps, 240.0, targetFps, maxCount, count, 1, 0 });
            undo = controller.LastReason() == XeFG_DynamicCount::Reason::Undo;
        }
        if (next != count)
        {
            if (gVerbose)
                std::printf("      %7.2f s  %dx -> %dx  reason %d\n", now / 1000.0, count + 1, next + 1,
                            static_cast<int>(controller.LastReason()));
            ++r.changes;
            r.undos += undo ? 1 : 0;
            count = next;
        }
        if (now > 3000.0)
        {
            const double presented = 1000.0 * (count + 1) / frame;
            if (std::abs(presented / targetFps - 1.0) <= 0.03)
                locked += frame;
            else if (presented > targetFps)
                over += frame;
            if (presented < targetFps * 0.93)
                under += frame;
            weighted += frame;
            r.meanCount += count * frame;
            if (now > seconds * 1000.0 - 20000.0)
                settled[count] += frame;
        }
    }
    r.finalCount = static_cast<int>(std::max_element(settled, settled + 8) - settled);
    r.lockedShare = locked / weighted;
    r.overShare = over / weighted;
    r.underShare = under / weighted;
    r.meanCount = r.meanCount / weighted;
    return r;
}

void Print(const char* who, const char* name, const Result& r)
{
    std::printf("  %-9s %-44s final %dx, changes %3d (undo %3d), locked %5.1f%%, over %4.1f%%, under %4.1f%%, "
                "mean %.2fx\n",
                who, name, r.finalCount + 1, r.changes, r.undos, r.lockedShare * 100.0, r.overShare * 100.0,
                r.underShare * 100.0, r.meanCount + 1.0);
}

struct Case
{
    const char* name;
    double seconds;
    double target;
    int maxCount;
    Cap cap;
    double genMs;
    std::function<double(double)> render;
};

Result Both(const Case& c)
{
    gGenMs = c.genMs;
    const auto capped = Run<XeFG_DynamicCap>(c.seconds, c.target, c.maxCount, c.maxCount, c.cap, c.render);
    const auto d12 = Run<XeFG_DynamicCount>(c.seconds, c.target, c.maxCount, c.maxCount, c.cap, c.render);
    Print("cap", c.name, capped);
    Print("dynamic12", c.name, d12);
    return capped;
}
} // namespace

int main()
{
    gVerbose = std::getenv("SIM_VERBOSE") != nullptr;
    auto constant = [](double ms) { return [ms](double) { return ms; }; };

    // Subnautica 2 run at 20:00 (dynamic13, cap 120, Reflex, GPU bound): 2X ~87, 3X ~108-115 -> R 18.8 ms + 4.1 ms
    // per generated frame. 4X reaches 120, 3X doesn't.
    auto r = Both({ "subnautica 20:00: reflex cap 120, 3x ~111", 300, 120, 5, Cap::Reflex, 4.1, constant(18.8) });
    Check(r.finalCount == 3 && r.lockedShare > 0.9, "subnautica: holds 4x at the cap");
    r = Both({ "subnautica 20:00: reflex cap 116", 300, 116, 5, Cap::Reflex, 4.1, constant(18.8) });
    Check(r.lockedShare > 0.9, "subnautica 116: locked at the cap");
    // beta.2's controller as shipped: 3x at 96% of the target passed its 93% check, then Up took it back (110 changes)
    Check(r.changes <= 25, "subnautica 116: no 4x <-> 3x see-saw");
    // The real game's load moves: 3x reaches 108-120 over time
    r = Both({ "subnautica, load 17-20.5 ms, reflex cap 120", 300, 120, 5, Cap::Reflex, 4.1,
               [](double t) { return 18.75 + 1.75 * std::sin(t * 6.2832 / 40.0); } });
    Check(r.lockedShare > 0.85 && r.underShare < 0.05 && r.changes <= 60, "subnautica moving load: locked");

    r = Both({ "limiter 120, 50 fps game", 60, 120, 5, Cap::Limiter, 0.5, constant(20.0) });
    Check(r.finalCount == 2 && r.lockedShare > 0.95, "limiter: 3x, locked");

    r = Both({ "limiter 120, 125 -> 33 -> 83 fps", 90, 120, 5, Cap::Limiter, 0.5,
               [](double t) { return t < 30 ? 8.0 : t < 60 ? 30.0 : 12.0; } });
    Check(r.finalCount == 1 && r.lockedShare > 0.85 && r.underShare < 0.1, "load changes: locked, back to 2x");

    r = Both({ "reflex 120, 125 -> 33 -> 83 fps, 2 ms/frame", 90, 120, 5, Cap::Reflex, 2.0,
               [](double t) { return t < 30 ? 8.0 : t < 60 ? 30.0 : 12.0; } });
    Check(r.lockedShare > 0.8 && r.underShare < 0.15, "reflex load changes: mostly locked");

    r = Both({ "limiter 120, 80 fps game", 60, 120, 5, Cap::Limiter, 0.5, constant(12.5) });
    Check(r.finalCount == 1 && r.lockedShare > 0.9, "light game: 2x locked");

    r = Both({ "reflex 120, heavy 25 fps game, 3 ms/frame", 120, 120, 5, Cap::Reflex, 3.0, constant(40.0) });
    Check(r.finalCount == 5, "heavy game: 6x");

    std::printf("%s: XeFG dynamic cap (%d checks, %d failed)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
