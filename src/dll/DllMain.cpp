#include <windows.h>

#include <cstdio>
#include <string>

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
    dx12track::g_capture_callstacks = (callstacks == L"1");

    if (!pipe_name.empty()) {
        dx12track::GlobalPipe().Connect(pipe_name, 5000);
    }
    if (!json_path.empty()) {
        dx12track::GlobalLog().Open(json_path);
    }

    SendHello();

    // Module tracking only when callstacks are on — the module metadata is
    // dead weight unless we're going to use it for symbolication later.
    if (dx12track::g_capture_callstacks) {
        dx12track::StartModuleTracking();
    }

    if (!dx12track::InstallExportHooks()) {
        // Hook installation failed — keep the DLL loaded, but tracking is dead.
        // (We could OutputDebugStringW here for visibility.)
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
