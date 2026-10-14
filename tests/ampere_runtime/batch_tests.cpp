#include "ampere_policy.h"
#include "ampere_diagnostics.h"
#include <iostream>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    using ampere_policy::Batch;
    Batch first;
    Check(first.Accepts(10, 1, 1, 100), "first 2x submission is admitted");
    first.Complete(10, 1, 1, 100, false);
    Check(first.failed && first.started && first.frame == 10, "first-call platform failure retains its failed interval");
    Check(!first.Accepts(10, 1, 1, 200), "same failed frame cannot retry");
    Check(!first.Accepts(9, 1, 1, 200), "stale frame cannot recover");
    Check(!first.Accepts(11, 2, 2, 200), "recovery cannot start at a partial index");
    Check(first.Accepts(11, 1, 1, 200), "next 2x frame can recover without feature recreation");
    first.Complete(11, 1, 1, 200, true);
    Check(!first.failed && first.submitted == 1 && first.frame == 11, "successful recovery clears batch failure");
    Check(!first.Accepts(11, 1, 1, 300), "completed frame cannot be submitted twice");
    Batch middle;
    middle.Complete(30, 5, 1, 100, true);
    Check(!middle.Accepts(30, 5, 2, 100), "outputs within one interval must be distinct");
    Check(!middle.Accepts(30, 3, 2, 200), "count cannot change during an interval");
    Check(!middle.Accepts(31, 5, 2, 200), "frame cannot change during an interval");
    middle.Complete(30, 5, 2, 200, false);
    Check(!middle.Accepts(30, 5, 3, 300), "remaining indices of a failed interval are blocked");
    Check(!middle.Accepts(30, 5, 1, 300), "failed MFG interval cannot restart on the same frame");
    Check(middle.Accepts(31, 3, 1, 100), "new interval can recover at a different valid count");
    for (int i = 1; i <= 3; ++i) middle.Complete(31, 3, i, 100 * i, true);
    Check(!middle.failed && middle.submitted == 3 && middle.generated == 3, "recovered MFG interval completes in order");
    Check(middle.Accepts(32, 5, 1, 100), "completed interval can switch to a higher proven count");
    Check(!middle.Accepts(32, 6, 1, 100) && !middle.Accepts(32, 0, 1, 100), "counts outside fixed program capacity remain rejected");
    Check(!middle.Accepts(32, 1, 0, 100) && !middle.Accepts(32, 1, 1, 0), "zero index and null output remain rejected");
    // Deep Rock Galactic tester log (rtx30.1): count 2, index 2 arrived with
    // index 1's output (flip-metering presentation's two-surface pool).
    Batch shared;
    shared.Complete(3919, 2, 1, 0x9C0, true);
    Check(!shared.Accepts(3919, 2, 2, 0x9C0) && shared.DuplicateOutput(3919, 2, 2, 0x9C0),
        "a shared output at the next index is refused and identified as reuse");
    Check(!shared.DuplicateOutput(3919, 2, 2, 0xA00), "a distinct output is not reuse");
    Check(!shared.DuplicateOutput(3920, 2, 2, 0x9C0) && !shared.DuplicateOutput(3919, 3, 2, 0x9C0)
        && !shared.DuplicateOutput(3919, 2, 3, 0x9C0), "frame, count and index refusals are not reuse");
    shared.Complete(3919, 2, 2, 0x9C0, true);
    Check(shared.failed && !shared.DuplicateOutput(3919, 2, 2, 0x9C0), "a failed interval reports no further reuse");
    Batch finished;
    finished.Complete(50, 1, 1, 0x100, true);
    Check(!finished.DuplicateOutput(50, 1, 1, 0x100) && !finished.DuplicateOutput(51, 1, 1, 0x100),
        "2x reusing its surface on the next frame is not reuse");
    Check(!Batch{}.DuplicateOutput(1, 2, 2, 0x100), "an unstarted interval has no reuse");
    ampere_policy::Feature latched{};
    Check(!latched.outputReuseLatched, "a new feature starts unlatched");
    ampere_policy::Feature feature{};
    feature.handle = 1; feature.owner = 2; feature.generation = 3; feature.publication = 4; feature.luid = 5;
    feature.batch.Fail(10, 1, 1);
    Check(feature.Matches(1, 2, 3, 4, 5), "batch failure retains exact feature lifetime and publication");
    Check(!feature.Matches(1, 2, 9, 4, 5) && !feature.Matches(1, 2, 3, 9, 5), "stale module and publication still block recovery");
    Check(!feature.Release(false) && feature.handle == 1, "failed native release retains ownership");
    Check(feature.Release(true) && !feature.handle && !feature.batch.started, "successful native release clears interval history");
    Check(ampere_policy::CapacityValid(1) && ampere_policy::CapacityValid(5)
        && !ampere_policy::CapacityValid(0) && !ampere_policy::CapacityValid(3), "only legacy 2x and proven six-buffer MFG contracts are admitted");
    ampere_diagnostics::Failures diagnostic;
    diagnostic.Record(173, 0xBAD00005u);
    diagnostic.Record(131);
    Check(diagnostic.Primary().code == 173 && diagnostic.Primary().ngxResult == 0xBAD00005u,
        "later guard failures retain the original NGX platform error");
    diagnostic.Recover();
    Check(!diagnostic.Primary().code && !diagnostic.Last().code && diagnostic.First().ngxResult == 0xBAD00005u,
        "recovery clears active failure while preserving first-frame evidence");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
