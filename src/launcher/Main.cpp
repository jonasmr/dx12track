#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "Injector.h"
#include "Model.h"
#include "PipeServer.h"
#include "Renderer.h"

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    fwprintf(stderr,
        L"Usage: dx12track.exe [-o <log.jsonl>] [--callstacks] [--verbose] [--debugger] [--] <target.exe> [args...]\n"
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
        L"                rest of injection/hook install.\n");
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring jsonl_path = L"dx12track.jsonl";
    std::vector<std::wstring> child_args;
    std::wstring target;
    bool callstacks    = false;
    bool verbose       = false;
    bool wait_debugger = false;

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

    // 5) Resume.
    ResumeThread(pi.hThread);

    // 6) Connect to the pipe (the DLL connects from its DllMain) and start
    //    streaming events into the model.
    dx12track::Model model;
    if (!pipe.ConnectAndStart(model)) {
        fwprintf(stderr, L"Pipe handshake failed.\n");
    }

    // 7) Render loop: 10 Hz until the child exits, then one final frame.
    dx12track::Renderer renderer;
    if (!renderer.Init()) {
        fwprintf(stderr, L"Renderer init failed; falling back to silent mode.\n");
    }

    using namespace std::chrono_literals;
    bool child_done = false;
    DWORD child_exit = 0;
    while (!child_done) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &child_exit);
            child_done = true;
        }
        renderer.Render(model.GetSnapshot());
    }
    model.MarkChildExited(child_exit);

    // Give the pipe a moment to flush the Goodbye event.
    std::this_thread::sleep_for(200ms);
    pipe.Stop();
    renderer.Render(model.GetSnapshot());

    // Plain-text summary to stdout — visible even when output is redirected
    // and the WriteConsoleOutput surface isn't.
    auto final_snap = model.GetSnapshot();
    fwprintf(stdout,
        L"\nFinal summary: %zu live objects, %.2f MB still allocated, "
        L"exit code %lu\n",
        final_snap.live_count,
        (double)final_snap.live_bytes / (double)(1ull << 20),
        child_exit);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)child_exit;
}
