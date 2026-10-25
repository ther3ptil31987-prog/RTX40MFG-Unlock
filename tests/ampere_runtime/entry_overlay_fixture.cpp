// ngxforward.1: the "other hook" EntryOverlayTests installs over our entry detours.
// It lives beside the targets so its rel32 jump always reaches.
#include <Windows.h>

using OverlayFn = int (*)(int, int);

namespace
{
OverlayFn gNext = nullptr;
volatile long gForeignCalls = 0;
}

extern "C" void OverlaySetNext(OverlayFn next) { gNext = next; }
extern "C" long OverlayForeignCalls() { return gForeignCalls; }
extern "C" int OverlayForeignHook(int a, int b)
{
    InterlockedIncrement(&gForeignCalls);
    return gNext(a, b);
}
