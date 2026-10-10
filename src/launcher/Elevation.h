#pragma once

#include <string>
#include <utility>
#include <vector>
#include <windows.h>

// UAC handling for --etw.
//
// A non-elevated launcher started with --etw relaunches itself elevated
// (ShellExecuteEx "runas") with the same arguments plus the internal
// "--elevated-by <pid>" (always argv[1]), then waits for it and returns its
// exit code. The two instances share a small named file mapping
// ("handoff") created by the original: its cwd, user SID and environment
// block go to the elevated instance; the elevated instance reports back
// whether it could attach to the original's console and, if it couldn't,
// forwards its text output so the original can print it after it exits.
//
// The elevated instance creates the target with the original launcher as
// its parent process (PROC_THREAD_ATTRIBUTE_PARENT_PROCESS), so the target
// gets the user's normal (non-elevated) token.

namespace dx12track {

std::wstring FormatWin32Error(DWORD code);

// Elevation / integrity level of a process (handle needs
// PROCESS_QUERY_LIMITED_INFORMATION).
struct ProcessElevation {
    bool  ok        = false;
    bool  elevated  = false;
    DWORD integrity = 0;   // SECURITY_MANDATORY_*_RID
};
ProcessElevation QueryProcessElevation(HANDLE process);
const wchar_t* IntegrityName(DWORD rid);

bool IsCurrentProcessElevated();
// Member of BUILTIN\Performance Log Users (S-1-5-32-559): can run the ETW
// session without elevation.
bool IsPerformanceLogUser();
// String SID of the current process token's user ("S-1-5-21-...").
std::wstring CurrentUserSid();

struct HandoffHeader;

// ---------------------------------------------------------------------------
// Original (non-elevated) instance.
// ---------------------------------------------------------------------------
class ElevationRequest {
public:
    enum class Result { Ok, Declined, Failed };

    ElevationRequest() = default;
    ~ElevationRequest();
    ElevationRequest(const ElevationRequest&) = delete;
    ElevationRequest& operator=(const ElevationRequest&) = delete;

    // Shows the UAC prompt and starts an elevated copy of this exe with this
    // process's own arguments, prefixed by "--elevated-by <our pid>".
    Result Launch(std::wstring* error);

    // Waits for the elevated instance (stays silent while it shares our
    // console) and returns its exit code.
    int Wait();

private:
    HANDLE         mapping_ = nullptr;
    HandoffHeader* hdr_     = nullptr;
    HANDLE         process_ = nullptr;
    DWORD          pid_     = 0;
};

// ---------------------------------------------------------------------------
// Elevated instance (started with --elevated-by <pid>).
// ---------------------------------------------------------------------------
class ElevatedSession {
public:
    ElevatedSession() = default;
    ~ElevatedSession();
    ElevatedSession(const ElevatedSession&) = delete;
    ElevatedSession& operator=(const ElevatedSession&) = delete;

    // Opens the original process and the handoff, switches to the original's
    // cwd and moves this process onto the original's console (or a console
    // window of its own when that fails). Call first thing in wmain.
    // original_pid == our own pid is allowed (test of the re-parenting path):
    // no console switch, no handoff.
    // Returns false (having printed nothing) when the original process no
    // longer exists - nobody is waiting for this instance then.
    bool Begin(DWORD original_pid);

    DWORD OriginalPid() const { return original_pid_; }
    bool  OwnConsole() const { return own_console_; }

    // Process to use as the target's parent: the original launcher if it is
    // still alive, else the shell (explorer.exe). nullptr if neither could be
    // opened with PROCESS_CREATE_PROCESS.
    HANDLE ParentProcess(std::wstring* which);

    // User SID of the original launcher (from the handoff); empty if unknown.
    const std::wstring& OriginalUserSid() const { return original_sid_; }

    // Environment block (CREATE_UNICODE_ENVIRONMENT) for the target: the
    // original launcher's environment (falls back to ours) with every
    // DX12TRACK_* variable replaced by `vars`.
    std::vector<wchar_t> BuildEnvironment(
        const std::vector<std::pair<std::wstring, std::wstring>>& vars) const;

    // With a console of our own, text passed here is also handed to the
    // original instance, which prints it after we exit.
    void Forward(const wchar_t* text, size_t chars);

private:
    void OpenHandoff();
    void SwitchConsole();

    DWORD          original_pid_ = 0;
    HANDLE         original_     = nullptr;
    HANDLE         shell_        = nullptr;
    HANDLE         mapping_      = nullptr;
    HandoffHeader* hdr_          = nullptr;
    bool           own_console_  = false;
    std::wstring   original_sid_;
    std::vector<wchar_t> original_env_;
};

// CreateProcessW(CREATE_SUSPENDED) with `parent` as the parent process (the
// new process inherits its token) and an explicit Unicode environment block.
bool CreateProcessWithParent(HANDLE parent, const std::wstring& exe,
                             std::wstring& cmdline, const wchar_t* cwd,
                             std::vector<wchar_t>& env, PROCESS_INFORMATION* pi);

} // namespace dx12track
