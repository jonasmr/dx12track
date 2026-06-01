#include <windows.h>

#include <cstdio>
#include <string>

#include "Diag.h"
#include "EventTypes.h"
#include "Hooks.h"
#include "JsonLog.h"
#include "Modules.h"
#include "PipeClient.h"
#include "Tracker.h"

namespace {

std::wstring EnvW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring v; v.resize(n);
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    if (got + 1 != n) return {};
    v.resize(got);
    return v;
}

void SendHello() {
    dx12track::HelloPayload p{};
    p.protocol_version = dx12track::kProtocolVersion;
    p.pid = GetCurrentProcessId();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    p.qpc_frequency = (uint64_t)f.QuadPart;
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    size_t n = wcsnlen(buf, dx12track::kMaxNameChars - 1);
    memcpy(p.exe_path, buf, n * sizeof(wchar_t));
    p.exe_path[n] = 0;

    dx12track::GlobalPipe().Send(dx12track::EventKind::Hello, &p, sizeof(p));
    dx12track::GlobalLog().Append(dx12track::EventKind::Hello, 0, &p, sizeof(p));
}

void OnAttach() {
    std::wstring pipe_name  = EnvW(L"DX12TRACK_PIPE");
    std::wstring json_path  = EnvW(L"DX12TRACK_JSON");
    std::wstring callstacks = EnvW(L"DX12TRACK_CALLSTACKS");
    std::wstring verbose    = EnvW(L"DX12TRACK_VERBOSE");
    dx12track::g_capture_callstacks = (callstacks == L"1");
    dx12track::g_verbose            = (verbose    == L"1");

    if (!pipe_name.empty()) {
        dx12track::GlobalPipe().Connect(pipe_name, 5000);
    }
    if (!json_path.empty()) {
        dx12track::GlobalLog().Open(json_path);
    }

    SendHello();

    // First diagnostic — confirms DLL was loaded and reached OnAttach, the
    // log file is open, and the env vars came through. If the user sees a
    // hello but no diagnostics at all when --verbose is on, the env var
    // didn't propagate to the child.
    dx12track::DiagF("dx12track DllMain attached, pid=%lu, pipe=%ls, json=%ls, callstacks=%d",
        GetCurrentProcessId(),
        pipe_name.empty() ? L"(none)" : pipe_name.c_str(),
        json_path.empty() ? L"(none)" : json_path.c_str(),
        dx12track::g_capture_callstacks ? 1 : 0);

    // Module tracking only when callstacks are on — the module metadata is
    // dead weight unless we're going to use it for symbolication later.
    if (dx12track::g_capture_callstacks) {
        dx12track::StartModuleTracking();
    }

    if (!dx12track::InstallExportHooks()) {
        dx12track::DiagF("InstallExportHooks() returned FALSE — tracking will be dead");
    } else {
        dx12track::DiagF("InstallExportHooks() returned TRUE");
    }
}

void OnDetach() {
    dx12track::GoodbyePayload p{}; p.exit_code = 0;
    dx12track::GlobalPipe().Send(dx12track::EventKind::Goodbye, &p, sizeof(p));
    dx12track::GlobalLog().Append(dx12track::EventKind::Goodbye, 0, &p, sizeof(p));

    dx12track::StopModuleTracking();
    dx12track::RemoveAllHooks();
    dx12track::GlobalPipe().Close();
    dx12track::GlobalLog().Close();
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            OnAttach();
            break;
        case DLL_PROCESS_DETACH:
            OnDetach();
            break;
        default:
            break;
    }
    return TRUE;
}
