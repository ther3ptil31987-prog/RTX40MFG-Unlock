// xemfgsa.1: CPU checks of XeMFG standalone's policy (source/native/xess/standalone_policy.h).
#include "xess/standalone_policy.h"
#include "xess/xemfg_dynamic.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
using namespace xemfg_standalone;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void Ids()
{
    FrameIds ids;
    // Upscaler frames: the one after the last present.
    CHECK(ids.Next() == 1);
    ids.Presented(1);
    CHECK(ids.Next() == 2 && ids.Last() == 1);
    // Streamline tokens map after the last present and keep their spacing.
    const uint32_t first = ids.FromToken(5000);
    CHECK(first == 2 && ids.Tokens());
    CHECK(ids.FromToken(5001) == 3);
    CHECK(ids.FromToken(5003) == 5);
    ids.Presented(3);
    // A frame already presented gets no id; the next ones keep mapping.
    CHECK(ids.FromToken(5000) == 0);
    CHECK(ids.FromToken(5002) == 4);
    // Ids never go back.
    ids.Presented(2);
    CHECK(ids.Last() == 3);
    // A restarted token counter (far behind) maps right after the last present again.
    CHECK(ids.FromToken(7) == 4);
    CHECK(ids.FromToken(8) == 5);
    // Back to the upscaler path, then tokens again: a fresh offset above the last present.
    ids.Presented(10);
    ids.LeaveTokens();
    CHECK(!ids.Tokens() && ids.Next() == 11);
    CHECK(ids.FromToken(9) == 11);
    // A huge jump re-bases instead of skipping millions of ids.
    CHECK(ids.FromToken(9 + 0x200000) == 11);

    // The present's id: the marker's frame, else the oldest pending frame, else the next.
    const uint32_t pending[] = {14, 12, 13};
    CHECK(PresentId(11, 15, pending, 3) == 15);
    CHECK(PresentId(11, 0, pending, 3) == 12);
    CHECK(PresentId(11, 11, pending, 3) == 12);   // a marker for a presented frame is stale
    CHECK(PresentId(14, 0, pending, 3) == 15);

    // The ring finds, reuses the oldest slot and lists pending complete frames.
    FrameRing<4> ring;
    for (uint32_t id = 1; id <= 6; ++id) { auto& r = ring.At(id); r.depth = r.motion = id != 5; }
    CHECK(!ring.Find(1) && !ring.Find(2) && ring.Find(3) && ring.Find(6));
    uint32_t out[4]{};
    const size_t count = ring.Pending(3, out, 4);
    CHECK(count == 2);   // 4 and 6 (5 has no inputs)
}

void Sources()
{
    // Streamline always; the upscalers only after Streamline's hold.
    CHECK(SourceAdmitted(Source::eStreamline, 5000, 4990));
    CHECK(!SourceAdmitted(Source::eDlss, 5000, 4500));
    CHECK(SourceAdmitted(Source::eDlss, 5600, 4500));
    CHECK(SourceAdmitted(Source::eFsr, 5000, 0));
    CHECK(!SourceAdmitted(Source::eNone, 5000, 0));
    // The first source keeps the frame.
    FrameRecord record;
    CHECK(Claim(record, Source::eFsr));
    CHECK(Claim(record, Source::eFsr));
    CHECK(!Claim(record, Source::eXess));
    // Streamline's tags take a frame an upscaler reached first; an upscaler never takes it back.
    CHECK(Claim(record, Source::eStreamline) && record.source == Source::eStreamline);
    CHECK(!Claim(record, Source::eDlss) && record.source == Source::eStreamline);
    CHECK(std::strcmp(SourceName(Source::eStreamline), "streamline") == 0);
    // Streamline lifecycles: valid only now (or until evaluate) needs the command list XeFG copies on.
    CHECK(StreamlineValidity(0, true) == Validity::eOnlyNow);
    CHECK(StreamlineValidity(0, false) == Validity::eSkip);
    CHECK(StreamlineValidity(1, false) == Validity::eUntilPresent);
    CHECK(StreamlineValidity(2, true) == Validity::eOnlyNow);
    CHECK(StreamlineValidity(2, false) == Validity::eSkip);
    CHECK(StreamlineValidity(9, true) == Validity::eSkip);
    // Tag extents: empty = whole resource, clamped inside it.
    auto area = TagArea(0, 0, 0, 0, 1920, 1080);
    CHECK(area.x == 0 && area.width == 1920 && area.height == 1080);
    area = TagArea(100, 50, 3000, 2000, 1920, 1080);
    CHECK(area.x == 100 && area.y == 50 && area.width == 1820 && area.height == 1030);
}

void Conventions()
{
    // Streamline's normalizing scale becomes pixels of the motion vectors' extent.
    auto scale = StreamlineMotionScale(1.0f / 1280.0f, 1.0f / 720.0f, 1280, 720);
    CHECK(Near(scale.first, 1.0f) && Near(scale.second, 1.0f));
    scale = StreamlineMotionScale(-0.5f, 0.25f, 2000, 1000);
    CHECK(Near(scale.first, -1000.0f) && Near(scale.second, 250.0f));
    // XeSS: pixels pass; NDC velocity (y up) becomes half-extent pixels with y flipped.
    scale = XessMotionScale(2.0f, 3.0f, false, 1920, 1080);
    CHECK(Near(scale.first, 2.0f) && Near(scale.second, 3.0f));
    scale = XessMotionScale(1.0f, 1.0f, true, 1920, 1080);
    CHECK(Near(scale.first, 960.0f) && Near(scale.second, -540.0f));
    // FidelityFX API states as its D3D12 backend maps them; unknown combinations refused.
    uint32_t state = 1234;
    CHECK(FfxStateToD3d12(1u << 2, state) && state == 0x40);
    CHECK(FfxStateToD3d12((1u << 2) | (1u << 3), state) && state == 0xC0);
    CHECK(FfxStateToD3d12((1u << 2) | (1u << 4), state) && state == 0xAC3);
    CHECK(FfxStateToD3d12(1u << 1, state) && state == 0x8);
    CHECK(!FfxStateToD3d12((1u << 1) | (1u << 3), state));
    // The projection: left-handed perspective, near/far swapped for inverted depth, infinite far.
    float m[16]{};
    Projection(16.0f / 9.0f, 0.1f, 1000.0f, 1.0471976f, false, m);
    CHECK(Near(m[5], 1.0f / std::tan(0.5235988f)) && Near(m[0], m[5] * 9.0f / 16.0f));
    CHECK(Near(m[11], 1.0f) && Near(m[10], 1000.0f / 999.9f) && Near(m[14], -0.1f * 1000.0f / 999.9f));
    Projection(1.0f, 0.1f, 1000.0f, 1.0471976f, true, m);
    CHECK(Near(m[10], 0.1f / (0.1f - 1000.0f)) && Near(m[14], -1000.0f * m[10]));
    Projection(1.0f, 0.1f, INFINITY, 1.0f, false, m);
    CHECK(Near(m[10], 1.0f) && Near(m[14], -0.1f));
    Projection(1.0f, 0.1f, INFINITY, 1.0f, true, m);
    CHECK(Near(m[10], 0.0f) && Near(m[14], 0.1f));
    // No camera: 60 degrees, 0.1 / 10000.
    Projection(1.0f, 0, 0, 0, false, m);
    CHECK(Near(m[5], 1.0f / std::tan(0.5235988f)) && Near(m[10], 10000.0f / 9999.9f));
}

void Generation()
{
    GenerationInputs in;
    CHECK(std::strcmp(WhyNotGenerating(in), "off") == 0);
    in.enabled = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "no_inputs") == 0);
    in.depth = in.motion = true;
    CHECK(WhyNotGenerating(in) == nullptr);
    // The game's DLSS-G switch gates XeMFG once the game drives DLSS-G.
    in.dlssgSeen = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "game_frame_generation_off") == 0);
    in.gameFrameGeneration = true;
    CHECK(WhyNotGenerating(in) == nullptr);
    in.sizesMatch = false;
    CHECK(std::strcmp(WhyNotGenerating(in), "motion_vector_size") == 0);
    in.sizesMatch = true;
    in.fsrFrameGeneration = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "game_fsr_frame_generation") == 0);
}

void Rates()
{
    // xemfgsa.5: the FPS window samples after a second; a counter that goes back restarts it.
    FpsWindow fps;
    fps.Add(1000, 100, 600);
    CHECK(fps.sampleMs == 0 && fps.baseMilli == 0);
    fps.Add(1500, 115, 690);
    CHECK(fps.sampleMs == 0);
    fps.Add(2000, 130, 780);
    CHECK(fps.sampleMs == 2000 && fps.baseMilli == 30000 && fps.outputMilli == 180000);
    fps.Add(3250, 160, 960);
    CHECK(fps.sampleMs == 3250 && fps.baseMilli == 24000 && fps.outputMilli == 144000);
    fps.Add(3300, 5, 30);   // a new chain: restart, keep the last sample
    CHECK(fps.sampleMs == 3250 && fps.startBase == 5 && fps.startOutput == 30);
    fps.Add(4300, 35, 210);
    CHECK(fps.sampleMs == 4300 && fps.baseMilli == 30000 && fps.outputMilli == 180000);
    // The FPS limit counts presented frames, as XeLL does: the presented frames' interval; off; 1 FPS.
    CHECK(LimitIntervalUs(0) == 0);
    CHECK(LimitIntervalUs(120) == 8333);
    CHECK(LimitIntervalUs(117) == 8547);
    CHECK(LimitIntervalUs(60) == 16667);
    CHECK(LimitIntervalUs(1) == 1000000);
}

// xemfgsa.6: the host's Dynamic MFG driver (xemfg_dynamic.h); the controllers have their own simulations.
uint32_t RunDynamic(DynamicFrames& dynamic, const DynamicSettings& s, double gameMs, double seconds, uint32_t frames,
    int focus = 1, double* nowMs = nullptr)
{
    double now = nowMs ? *nowMs : 0.0;
    for (double end = now + seconds * 1000.0; now < end; now += gameMs) {
        DynamicFrames::Frame f;
        f.nowMs = now;
        f.frameMs = gameMs;
        f.refreshFps = 120.0;
        f.focus = focus;
        f.frames = frames;
        frames = dynamic.Update(f, s);
    }
    if (nowMs) *nowMs = now;
    return frames;
}

void Dynamic()
{
    // The target: the one set, at most the FPS limit; else the limit; else just under the refresh rate.
    DynamicSettings s;
    s.enabled = true;
    CHECK(DynamicTargetFps(s, 120.0) == 116.0);
    CHECK(DynamicTargetFps(s, 144.0) == 138.0);
    CHECK(DynamicTargetFps(s, 0.0) == 0.0);
    s.limitFps = 117;
    CHECK(DynamicTargetFps(s, 120.0) == 117.0);
    s.targetFps = 100;
    CHECK(DynamicTargetFps(s, 120.0) == 100.0);
    s.targetFps = 200;
    CHECK(DynamicTargetFps(s, 120.0) == 117.0);
    s.limitFps = 0;
    CHECK(DynamicTargetFps(s, 120.0) == 200.0);

    // A light game (100 real fps, frames free) and a 116 target: from 6X the count comes down; nearest is 1X (100 against
    // 200 at 2X), or 2X without 1X.
    s = DynamicSettings{};
    s.enabled = true;
    DynamicFrames dynamic;
    uint32_t frames = RunDynamic(dynamic, s, 10.0, 90.0, 6);
    CHECK(frames == 1 && dynamic.Active() && dynamic.TargetFps() == 116.0 && !dynamic.CapActive());
    s.allow1x = false;
    DynamicFrames no1x;
    CHECK(RunDynamic(no1x, s, 10.0, 90.0, 6) == 2);
    // A heavy game (25 real fps): up to the most frames set (4X here), never past it.
    s.allow1x = true;
    s.maxFrames = 4;
    DynamicFrames heavy;
    CHECK(RunDynamic(heavy, s, 40.0, 30.0, 2) == 4);

    // In the background the count stays as it is; back in front it decides again.
    s.maxFrames = 6;
    DynamicFrames focus;
    double now = 0.0;
    CHECK(RunDynamic(focus, s, 10.0, 30.0, 6, 0, &now) == 6);
    CHECK(RunDynamic(focus, s, 10.0, 90.0, 6, 1, &now) == 1);

    // Use target as cap: the host's limit holds the target; the fewest frames that reach it, never 1X.
    s.targetAsCap = true;
    DynamicFrames cap;
    frames = RunDynamic(cap, s, 10.0, 60.0, 6);
    CHECK(cap.CapActive() && cap.CapFps() == 116.0 && frames >= 2);
    // Switched off: everything goes, the most frames come back.
    s.enabled = false;
    CHECK(cap.Update({}, s) == 6 && !cap.Active() && cap.CapFps() == 0.0);
}
}

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "ids" || which == "all") Ids();
    if (which == "sources" || which == "all") Sources();
    if (which == "conventions" || which == "all") Conventions();
    if (which == "generation" || which == "all") Generation();
    if (which == "rates" || which == "all") Rates();
    if (which == "dynamic" || which == "all") Dynamic();
    std::printf("%s: %d checks, %d failures\n", which.c_str(), gChecks, gFailures);
    return gFailures ? 1 : 0;
}
