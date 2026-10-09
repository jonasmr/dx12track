#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "EtwMonitor.h"
#include "Injector.h"
#include "Model.h"
#include "PipeServer.h"
#include "Renderer.h"

namespace fs = std::filesystem;

namespace {

// --etw: how often to ask the D3D12 provider for a rundown (capture state).
// Pipeline-state driver sizes are only reported by rundowns.
constexpr uint32_t kEtwRundownIntervalMs = 10'000;

void PrintUsage() {
    fwprintf(stderr,
        L"Usage: dx12track.exe [-o <log.jsonl>] [--callstacks] [--verbose] [--debugger] [--etw] [--] <target.exe> [args...]\n"
        L"  -o <path>     path to JSON-Lines log file written by the DLL\n"
        L"                (default: dx12track.jsonl in the launcher's cwd)\n"
        L"  --callstacks  capture a callstack on every object creation and\n"
        L"                log loaded-module metadata for offline symbolication\n"
        L"  --verbose     emit injection / hook-install / per-fire diagnostics\n"
        L"                into the JSONL as \"diag\" events (for debugging\n"
        L"                why tracking might not be picking up a target)\n"
        L"  --debugger    block in DllMain with a MessageBox prompting you to\n"
        L"                attach a debugger. After you click OK, the DLL hits\n"
        L"                __debugbreak() so the debugger can step through the\n"
        L"                rest of injection/hook install.\n"
        L"  --etw         also consume D3D12/DxgKrnl ETW events (DxTimingCapture-\n"
        L"                Library): per-object VRAM/system-memory location, driver\n"
        L"                memory sizes, residency counters. Writes a sidecar log\n"
        L"                <log>.etw.jsonl. Needs admin or 'Performance Log Users'.\n"
        L"  --dump-console <path>\n"
        L"                debugging aid: also write the console frame as text to\n"
        L"                <path> (once per second and at exit)\n");
}

// Build a CreateProcessW lpCommandLine that quotes the target exe and appends
// the remaining arguments verbatim (already user-quoted on the launcher's cmdline).
std::wstring BuildCommandLine(const std::wstring& target,
                               const std::vector<std::wstring>& extra) {
    std::wstring s;
    s.push_back(L'"'); s.append(target); s.push_back(L'"');
    for (auto& a : extra) { s.push_back(L' '); s.append(a); }
    return s;
}

std::wstring DllPathBesideExe() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path p(self);
    p.replace_filename(L"dx12track.dll");
    return p.wstring();
}

// <dir>\run.jsonl -> <dir>\run.etw.jsonl ; anything else -> <path>.etw.jsonl
std::wstring EtwSidecarPath(const std::wstring& jsonl_path) {
    fs::path p(jsonl_path);
    if (_wcsicmp(p.extension().c_str(), L".jsonl") == 0)
        p.replace_extension(L".etw.jsonl");
    else
        p += L".etw.jsonl";
    return p.wstring();
}

// The exited child's ExitTime (FILETIME) mapped onto the QPC timeline by
// sampling both clocks now. Accuracy is ~1 ms, plenty to tell the app's last
// ETW events from its process-teardown ones.
bool ChildExitQpc(HANDLE process, uint64_t* exit_qpc) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    FILETIME now_ft{};
    LARGE_INTEGER now_qpc{}, freq{};
    GetSystemTimePreciseAsFileTime(&now_ft);
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&freq);
    auto to_u64 = [](const FILETIME& f) {
        return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    const uint64_t now_100ns = to_u64(now_ft), exit_100ns = to_u64(exited);
    if (!exit_100ns || exit_100ns > now_100ns) return false;
    const long double ago_ticks =
        (long double)(now_100ns - exit_100ns) * (long double)freq.QuadPart / 1e7L;
    const long double q = (long double)now_qpc.QuadPart - ago_ticks;
    if (q <= 0) return false;
    *exit_qpc = (uint64_t)q;
    return true;
}

// Ctrl+C / console close while --etw runs: stop our ETW session so it doesn't
// outlive us (real-time sessions persist after their controller dies), then
// let the default handling proceed.
BOOL WINAPI EtwCtrlHandler(DWORD) {
    dx12track::etw::Monitor::EmergencyStop();
    return FALSE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring jsonl_path = L"dx12track.jsonl";
    std::vector<std::wstring> child_args;
    std::wstring target;
    std::wstring dump_console_path;
    bool callstacks    = false;
    bool verbose       = false;
    bool wait_debugger = false;
    bool use_etw       = false;

    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-o" && i + 1 < argc) {
            jsonl_path = argv[++i];
        } else if (a == L"--callstacks" || a == L"-c") {
            callstacks = true;
        } else if (a == L"--verbose" || a == L"-v") {
            verbose = true;
        } else if (a == L"--debugger") {
            wait_debugger = true;
        } else if (a == L"--etw") {
            use_etw = true;
        } else if (a == L"--dump-console" && i + 1 < argc) {
            dump_console_path = argv[++i];
        } else if (a == L"--") {
            ++i; break;
        } else if (!a.empty() && a[0] == L'-') {
            fwprintf(stderr, L"Unknown flag: %ls\n", a.c_str());
            PrintUsage();
            return 1;
        } else {
            target = a; ++i; break;
        }
    }
    for (; i < argc; ++i) child_args.emplace_back(argv[i]);

    if (target.empty()) { PrintUsage(); return 1; }

    // Resolve to absolute path so the child process and JSONL log are unambiguous.
    target = fs::absolute(target).wstring();
    jsonl_path = fs::absolute(jsonl_path).wstring();
    if (!dump_console_path.empty())
        dump_console_path = fs::absolute(dump_console_path).wstring();

    const std::wstring dll_path = DllPathBesideExe();
    if (!fs::exists(dll_path)) {
        fwprintf(stderr, L"dx12track.dll not found next to launcher: %ls\n",
            dll_path.c_str());
        return 2;
    }
    if (!fs::exists(target)) {
        fwprintf(stderr, L"Target not found: %ls\n", target.c_str());
        return 2;
    }

    // 1) Create the pipe server, get its name to hand to the child.
    dx12track::PipeServer pipe;
    std::wstring pipe_name;
    if (!pipe.Create(&pipe_name)) return 3;

    // 2) Set inheritable env vars so the child's CRT sees them in DllMain.
    SetEnvironmentVariableW(L"DX12TRACK_PIPE", pipe_name.c_str());
    SetEnvironmentVariableW(L"DX12TRACK_JSON", jsonl_path.c_str());
    SetEnvironmentVariableW(L"DX12TRACK_CALLSTACKS", callstacks    ? L"1" : L"0");
    SetEnvironmentVariableW(L"DX12TRACK_VERBOSE",    verbose       ? L"1" : L"0");
    SetEnvironmentVariableW(L"DX12TRACK_WAIT_DEBUGGER",
                            wait_debugger ? L"1" : L"0");

    // 3) CreateProcess(CREATE_SUSPENDED).
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = BuildCommandLine(target, child_args);
    // CreateProcessW may modify the cmdline buffer.
    std::wstring mutable_cmdline = cmdline;
    fs::path target_dir = fs::path(target).parent_path();

    if (!CreateProcessW(target.c_str(), mutable_cmdline.data(),
                        nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED,
                        nullptr,
                        target_dir.empty() ? nullptr : target_dir.c_str(),
                        &si, &pi)) {
        fwprintf(stderr, L"CreateProcessW failed: %lu\n", GetLastError());
        return 4;
    }
    fwprintf(stdout, L"Started %ls (pid %lu), suspended.\n",
        target.c_str(), pi.dwProcessId);

    // 4) Inject the DLL. With --debugger the DLL blocks in a MessageBox so
    //    the remote LoadLibraryW won't return until the user clicks OK — use
    //    INFINITE so we don't time out waiting.
    auto inj = dx12track::InjectDll(pi.hProcess, dll_path,
        wait_debugger ? INFINITE : 30000);
    if (!inj.ok) {
        fwprintf(stderr, L"Injection failed: %ls\n", inj.message.c_str());
        TerminateProcess(pi.hProcess, 5);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 5;
    }
    fwprintf(stdout, L"Injected dx12track.dll.\n");

    // Declared before the ETW monitor so it outlives the monitor's consumer
    // thread, which calls into it until Stop() joins.
    dx12track::Model model;
    dx12track::etw::Monitor etw_monitor;
    bool etw_running = false;
    std::wstring etw_sidecar;

    // 4b) --etw: start the ETW session while the child is still suspended so
    //     it sees the device and every object from the first one on.
    if (use_etw) {
        etw_sidecar = EtwSidecarPath(jsonl_path);
        if (!model.EnableEtw(etw_sidecar, etw_monitor.SessionName(), jsonl_path,
                             kEtwRundownIntervalMs)) {
            fwprintf(stderr, L"ETW: could not open sidecar %ls; continuing without it.\n",
                etw_sidecar.c_str());
            etw_sidecar.clear();
        }
        std::wstring err;
        if (etw_monitor.Start(pi.dwProcessId, &model, &err)) {
            etw_running = true;
            model.SetEtwRunning(true);
            SetConsoleCtrlHandler(EtwCtrlHandler, TRUE);
            fwprintf(stdout, L"ETW session %ls started.\n",
                etw_monitor.SessionName().c_str());
        } else {
            fwprintf(stderr, L"ETW: %ls\nContinuing without ETW.\n", err.c_str());
            model.DisableEtw(err);
            etw_sidecar.clear();
        }
    }

    // 5) Resume.
    ResumeThread(pi.hThread);

    // 6) Connect to the pipe (the DLL connects from its DllMain) and start
    //    streaming events into the model.
    if (!pipe.ConnectAndStart(model)) {
        fwprintf(stderr, L"Pipe handshake failed.\n");
    }

    // 7) Render loop: 10 Hz until the child exits, then one final frame.
    dx12track::Renderer renderer;
    if (!renderer.Init()) {
        fwprintf(stderr, L"Renderer init failed; falling back to silent mode.\n");
    }
    if (!dump_console_path.empty()) renderer.SetDumpPath(dump_console_path);

    using namespace std::chrono_literals;
    bool child_done = false;
    DWORD child_exit = 0;
    ULONGLONG last_rundown_ms = GetTickCount64();
    ULONGLONG last_stats_ms   = 0;
    dx12track::etw::Stats etw_stats;
    while (!child_done) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &child_exit);
            child_done = true;
            if (etw_running) {
                uint64_t exit_qpc = 0;
                if (ChildExitQpc(pi.hProcess, &exit_qpc)) model.SetChildExitQpc(exit_qpc);
            }
        }
        if (etw_running) {
            const ULONGLONG now = GetTickCount64();
            if (now - last_rundown_ms >= kEtwRundownIntervalMs) {
                last_rundown_ms = now;
                etw_monitor.RequestRundown();
            }
            if (now - last_stats_ms >= 1000) {  // ControlTrace(QUERY) once a second
                last_stats_ms = now;
                etw_stats = etw_monitor.GetStats();
            }
            model.EtwTick(etw_stats);
        }
        renderer.Render(model.GetSnapshot());
    }
    model.MarkChildExited(child_exit);

    // Give the pipe a moment to flush the Goodbye event.
    std::this_thread::sleep_for(200ms);
    pipe.Stop();

    // Stopping the session flushes ETW's buffers to the consumer, then the
    // library's OnDataComplete runs and the consumer thread exits.
    if (etw_running) {
        etw_monitor.Stop();
        etw_stats = etw_monitor.GetStats();
        model.FinishEtw(etw_stats);
        SetConsoleCtrlHandler(EtwCtrlHandler, FALSE);
    }
    renderer.Render(model.GetSnapshot());
    renderer.DumpNow();

    // Plain-text summary to stdout — visible even when output is redirected
    // and the WriteConsoleOutput surface isn't.
    auto final_snap = model.GetSnapshot();
    fwprintf(stdout,
        L"\nFinal summary: %zu live objects, %.2f MB still allocated, "
        L"exit code %lu\n",
        final_snap.live_count,
        (double)final_snap.live_bytes / (double)(1ull << 20),
        child_exit);
    if (use_etw) {
        const auto& e = final_snap.etw;
        if (!e.status.empty()) {
            fwprintf(stdout, L"ETW: not used (%ls)\n", e.status.c_str());
        } else {
            fwprintf(stdout,
                L"ETW: %llu events received, %llu events lost, %llu buffers lost, "
                L"%llu library exceptions, %llu rundowns (%llu failed)\n",
                (unsigned long long)etw_stats.events,
                (unsigned long long)etw_stats.events_lost,
                (unsigned long long)etw_stats.buffers_lost,
                (unsigned long long)etw_stats.exceptions,
                (unsigned long long)etw_stats.rundowns,
                (unsigned long long)etw_stats.rundown_failures);
            fwprintf(stdout,
                L"ETW: %llu objects reported (%llu rundown duplicates), %llu binds "
                L"(+%llu late matches to already-destroyed objects), "
                L"%llu group changes, pgIn %llu pgOut %llu, %llu library diagnostics\n",
                (unsigned long long)e.created, (unsigned long long)e.rundown_duplicates,
                (unsigned long long)e.binds, (unsigned long long)e.late_matches,
                (unsigned long long)e.group_changes,
                (unsigned long long)e.page_ins, (unsigned long long)e.page_outs,
                (unsigned long long)e.diagnostics);
            fwprintf(stdout, L"ETW: still unbound at exit: %llu", (unsigned long long)e.unbound);
            for (size_t t = 1; t < dx12track::EtwSnapshot::kTypes; ++t) {
                if (!e.unbound_by_type[t]) continue;
                fwprintf(stdout, L"  %hs=%llu",
                    dx12track::etw::ObjTypeName(static_cast<dx12track::etw::ObjType>(t)),
                    (unsigned long long)e.unbound_by_type[t]);
            }
            fwprintf(stdout, L"\n");
            if (!etw_sidecar.empty())
                fwprintf(stdout, L"ETW sidecar: %ls\n", etw_sidecar.c_str());
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)child_exit;
}
