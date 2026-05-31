// Exported C entry points for host programs that LoadLibrary("dx12track.dll")
// instead of using the launcher-based injection path. Body lives here so the
// rest of the DLL stays decoupled from the public ABI.

#define DX12TRACK_BUILDING_DLL
#include "dx12track.h"

#include "EventTypes.h"
#include "JsonLog.h"
#include "Modules.h"
#include "Tracker.h"

#include <cstring>

namespace {

// In the launcher-injection flow, DllMain sends Hello once after opening the
// pipe + log via env vars. In the self-inject flow neither is open yet at
// DllMain time, so we re-emit Hello when StartLogging finally opens the log.
void SendHelloToLog() {
    using namespace dx12track;
    HelloPayload p{};
    p.protocol_version = kProtocolVersion;
    p.pid = GetCurrentProcessId();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    p.qpc_frequency = (uint64_t)f.QuadPart;
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    size_t n = wcsnlen(buf, kMaxNameChars - 1);
    memcpy(p.exe_path, buf, n * sizeof(wchar_t));
    p.exe_path[n] = 0;
    GlobalLog().Append(EventKind::Hello, 0, &p, sizeof(p));
}

} // namespace

extern "C" DX12TRACK_API BOOL Dx12Track_StartLogging(const wchar_t* jsonl_path,
                                                     BOOL capture_callstacks) {
    if (!jsonl_path || !*jsonl_path) return FALSE;

    // Flip the capture flag before opening the log so any concurrent hook that
    // checks it sees the new value at roughly the same time the log appears.
    dx12track::g_capture_callstacks = (capture_callstacks != FALSE);

    if (!dx12track::GlobalLog().Open(jsonl_path)) {
        return FALSE;
    }

    SendHelloToLog();

    if (dx12track::g_capture_callstacks) {
        // Idempotent — StartModuleTracking guards itself with an atomic flag.
        dx12track::StartModuleTracking();
    }
    return TRUE;
}

extern "C" DX12TRACK_API void Dx12Track_StopLogging(void) {
    dx12track::GlobalLog().Close();
}
