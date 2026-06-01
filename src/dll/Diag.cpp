#include "Diag.h"

#include "EventTypes.h"
#include "JsonLog.h"
#include "PipeClient.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace dx12track {

bool g_verbose = false;

namespace {
uint64_t NowNs() {
    static LARGE_INTEGER freq{}, start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    long double ns = (long double)(now.QuadPart - start.QuadPart) * 1e9L /
                     (long double)freq.QuadPart;
    return (uint64_t)ns;
}
} // namespace

void DiagF(const char* fmt, ...) {
    if (!g_verbose) return;

    DiagnosticPayload p{};
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(p.message, sizeof(p.message), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) p.message[kMaxDiagnosticChars - 1] = 0;

    GlobalPipe().Send(EventKind::Diagnostic, &p, sizeof(p));
    GlobalLog().Append(EventKind::Diagnostic, NowNs(), &p, sizeof(p));
}

} // namespace dx12track
