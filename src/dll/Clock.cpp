#include "Clock.h"

#include <windows.h>

namespace dx12track {

namespace {

struct ClockBase {
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    ClockBase() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
};

// Function-local static: thread-safe one-time init, also from DllMain.
const ClockBase& Base() {
    static ClockBase b;
    return b;
}

} // namespace

void InitClock() { (void)Base(); }

uint64_t QpcStart()     { return (uint64_t)Base().start.QuadPart; }
uint64_t QpcFrequency() { return (uint64_t)Base().freq.QuadPart; }

uint64_t NowNs() {
    const ClockBase& b = Base();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG delta = now.QuadPart - b.start.QuadPart;
    if (delta < 0) delta = 0;
    long double ns = (long double)delta * 1e9L / (long double)b.freq.QuadPart;
    return (uint64_t)ns;
}

} // namespace dx12track
