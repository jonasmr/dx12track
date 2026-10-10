#include "Elevation.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

namespace dx12track {

// Shared between the original and the elevated instance (named file mapping
// "Local\dx12track-handoff-<original pid>", created by the original). Offsets
// are in bytes from the start of the header; strings are UTF-16.
struct HandoffHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t total_bytes;
    uint32_t cwd_off, cwd_chars;    // original's current directory (no NUL)
    uint32_t sid_off, sid_chars;    // original's user SID string (no NUL)
    uint32_t env_off, env_chars;    // original's environment block (incl. final NUL)
    uint32_t out_off, out_cap;      // text forwarded by the elevated instance
    volatile LONG console_state;    // written by the elevated instance, see below
    volatile LONG out_chars;
};

namespace {

constexpr uint32_t kHandoffMagic   = 0x48584432;  // "2DXH"
constexpr uint32_t kHandoffVersion = 1;
constexpr uint32_t kForwardChars   = 64 * 1024;

enum ConsoleState : LONG {
    kConsolePending  = 0,   // elevated instance not up yet
    kConsoleAttached = 1,   // it shares the original's console
    kConsoleOwn      = 2,   // it uses a console window of its own
};

std::wstring HandoffName(DWORD original_pid) {
    return L"Local\\dx12track-handoff-" + std::to_wstring(original_pid);
}

// Security descriptor from SDDL, freed on scope exit.
struct Sddl {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES  sa{};
    explicit Sddl(const std::wstring& s) {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                s.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
        }
    }
    ~Sddl() { if (sd) LocalFree(sd); }
    SECURITY_ATTRIBUTES* get() { return sd ? &sa : nullptr; }
};

// The launcher's own command line minus argv[0], exactly as typed (keeps the
// user's quoting). Same argv[0] rules as CommandLineToArgvW.
std::wstring RawArgsAfterExe() {
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

std::wstring SelfPath() {
    std::wstring s(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, s.data(), (DWORD)s.size());
        if (n == 0) return {};
        if (n < s.size()) { s.resize(n); return s; }
        s.resize(s.size() * 2);
    }
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// "NAME=value" -> length of NAME. Entries like "=C:=C:\dir" keep their
// leading '='.
size_t EnvNameLen(const std::wstring& e) {
    size_t eq = e.find(L'=', 1);
    return eq == std::wstring::npos ? e.size() : eq;
}

// Pointer to a double-NUL-terminated block -> entries.
void SplitEnvBlock(const wchar_t* block, std::vector<std::wstring>* out) {
    for (const wchar_t* p = block; p && *p; p += wcslen(p) + 1) out->emplace_back(p);
}

// Original instance: Ctrl+C while the elevated instance shares our console is
// for the elevated instance (it stops its ETW session and exits); we keep
// waiting so we can return its exit code. With its own window, Ctrl+C here
// just ends this waiting instance.
volatile LONG* g_wait_console_state = nullptr;

BOOL WINAPI WaitCtrlHandler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    const volatile LONG* st = g_wait_console_state;
    return (st && *st == kConsoleOwn) ? FALSE : TRUE;
}

} // namespace

// ===========================================================================
// Helpers
// ===========================================================================

std::wstring FormatWin32Error(DWORD code) {
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring out;
    if (len && buffer) {
        out.assign(buffer, len);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (buffer) LocalFree(buffer);
    wchar_t num[32];
    swprintf(num, 32, L"error %lu", code);
    return out.empty() ? std::wstring(num) : out + L" (" + num + L")";
}

ProcessElevation QueryProcessElevation(HANDLE process) {
    ProcessElevation r;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return r;
    TOKEN_ELEVATION te{};
    DWORD len = 0;
    const bool got_elev = GetTokenInformation(token, TokenElevation, &te, sizeof(te), &len) != 0;
    alignas(8) BYTE buf[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    const bool got_il = GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &len) != 0;
    CloseHandle(token);
    if (!got_elev || !got_il) return r;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf)->Label.Sid;
    const UCHAR n = *GetSidSubAuthorityCount(sid);
    r.integrity = n ? *GetSidSubAuthority(sid, n - 1) : 0;
    r.elevated  = te.TokenIsElevated != 0;
    r.ok        = true;
    return r;
}

const wchar_t* IntegrityName(DWORD rid) {
    if (rid <  SECURITY_MANDATORY_LOW_RID)               return L"untrusted";
    if (rid <  SECURITY_MANDATORY_MEDIUM_RID)            return L"low";
    if (rid <  SECURITY_MANDATORY_MEDIUM_PLUS_RID)       return L"medium";
    if (rid <  SECURITY_MANDATORY_HIGH_RID)              return L"medium-plus";
    if (rid <  SECURITY_MANDATORY_SYSTEM_RID)            return L"high";
    if (rid <  SECURITY_MANDATORY_PROTECTED_PROCESS_RID) return L"system";
    return L"protected";
}

bool IsCurrentProcessElevated() {
    ProcessElevation e = QueryProcessElevation(GetCurrentProcess());
    return e.ok && e.elevated;
}

bool IsPerformanceLogUser() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_LOGGING_USERS, 0, 0, 0, 0, 0, 0, &sid))
        return false;
    BOOL member = FALSE;
    if (!CheckTokenMembership(nullptr, sid, &member)) member = FALSE;
    FreeSid(sid);
    return member != FALSE;
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 1);
    std::wstring out;
    if (len && GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(token);
    return out;
}

// ===========================================================================
// ElevationRequest (original instance)
// ===========================================================================

ElevationRequest::~ElevationRequest() {
    if (hdr_) UnmapViewOfFile(hdr_);
    if (mapping_) CloseHandle(mapping_);
    if (process_) CloseHandle(process_);
}

ElevationRequest::Result ElevationRequest::Launch(std::wstring* error) {
    const std::wstring self = SelfPath();
    if (self.empty()) {
        *error = L"GetModuleFileNameW: " + FormatWin32Error(GetLastError());
        return Result::Failed;
    }

    // --- handoff: cwd, user SID, environment ---------------------------------
    std::wstring cwd(GetCurrentDirectoryW(0, nullptr), L'\0');
    cwd.resize(GetCurrentDirectoryW((DWORD)cwd.size(), cwd.data()));
    const std::wstring sid = CurrentUserSid();

    std::vector<wchar_t> env;
    if (LPWCH block = GetEnvironmentStringsW()) {
        const wchar_t* p = block;
        while (*p) p += wcslen(p) + 1;
        env.assign(static_cast<const wchar_t*>(block), p);
        FreeEnvironmentStringsW(block);
    }
    env.push_back(L'\0');
    if (env.size() == 1) env.push_back(L'\0');

    uint32_t off = (uint32_t)((sizeof(HandoffHeader) + 7) & ~size_t(7));
    auto reserve = [&off](size_t chars) {
        const uint32_t at = off;
        off += (uint32_t)(chars * sizeof(wchar_t));
        return at;
    };
    const uint32_t cwd_off = reserve(cwd.size() + 1);
    const uint32_t sid_off = reserve(sid.size() + 1);
    const uint32_t env_off = reserve(env.size());
    const uint32_t out_off = reserve(kForwardChars);
    const uint32_t total   = off;

    // Only us, the elevated instance (same user, or an administrator when the
    // UAC prompt asked for other credentials) and SYSTEM.
    Sddl sd(sid.empty() ? std::wstring()
                        : L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + sid + L")");
    const std::wstring name = HandoffName(GetCurrentProcessId());
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd.get(), PAGE_READWRITE,
                                  0, total, name.c_str());
    if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        *error = mapping_ ? L"handoff mapping " + name + L" already exists"
                          : L"CreateFileMappingW: " + FormatWin32Error(GetLastError());
        return Result::Failed;
    }
    hdr_ = static_cast<HandoffHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!hdr_) {
        *error = L"MapViewOfFile: " + FormatWin32Error(GetLastError());
        return Result::Failed;
    }
    BYTE* base = reinterpret_cast<BYTE*>(hdr_);
    hdr_->magic       = kHandoffMagic;
    hdr_->version     = kHandoffVersion;
    hdr_->total_bytes = total;
    hdr_->cwd_off = cwd_off; hdr_->cwd_chars = (uint32_t)cwd.size();
    hdr_->sid_off = sid_off; hdr_->sid_chars = (uint32_t)sid.size();
    hdr_->env_off = env_off; hdr_->env_chars = (uint32_t)env.size();
    hdr_->out_off = out_off; hdr_->out_cap   = kForwardChars;
    hdr_->console_state = kConsolePending;
    hdr_->out_chars     = 0;
    memcpy(base + cwd_off, cwd.c_str(), (cwd.size() + 1) * sizeof(wchar_t));
    memcpy(base + sid_off, sid.c_str(), (sid.size() + 1) * sizeof(wchar_t));
    memcpy(base + env_off, env.data(), env.size() * sizeof(wchar_t));

    // --- relaunch ------------------------------------------------------------
    const std::wstring params =
        L"--elevated-by " + std::to_wstring(GetCurrentProcessId()) + L" " + RawArgsAfterExe();

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE;
    sei.hwnd         = GetConsoleWindow();
    sei.lpVerb       = L"runas";
    sei.lpFile       = self.c_str();
    sei.lpParameters = params.c_str();
    sei.lpDirectory  = cwd.c_str();
    // The elevated instance moves to our console right away; its own console
    // window is only shown (re-created) if that fails.
    sei.nShow        = SW_HIDE;
    const BOOL ok    = ShellExecuteExW(&sei);
    const DWORD err  = ok ? 0 : GetLastError();
    if (SUCCEEDED(co)) CoUninitialize();

    if (!ok) {
        *error = FormatWin32Error(err);
        return err == ERROR_CANCELLED ? Result::Declined : Result::Failed;
    }
    if (!sei.hProcess) {
        *error = L"ShellExecuteExW returned no process handle";
        return Result::Failed;
    }
    process_ = sei.hProcess;
    pid_     = GetProcessId(process_);
    return Result::Ok;
}

int ElevationRequest::Wait() {
    g_wait_console_state = &hdr_->console_state;
    SetConsoleCtrlHandler(WaitCtrlHandler, TRUE);

    // Silent while the elevated instance may be drawing into this console.
    bool announced = false;
    for (;;) {
        const DWORD w = WaitForSingleObject(process_, 100);
        if (!announced && hdr_->console_state == kConsoleOwn) {
            announced = true;
            fwprintf(stdout,
                L"dx12track: the elevated instance (pid %lu) could not attach to this console;\n"
                L"           its output continues in its own window. Waiting for it to exit...\n",
                pid_);
            fflush(stdout);
        }
        if (w != WAIT_TIMEOUT) break;
    }

    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    const LONG state = hdr_->console_state;
    if (state != kConsoleAttached) {
        const LONG used = hdr_->out_chars;
        const LONG n = std::min<LONG>(used, (LONG)hdr_->out_cap);
        const wchar_t* text = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(hdr_) + hdr_->out_off);
        if (n > 0) {
            fwprintf(stdout, L"\n--- output of the elevated dx12track (pid %lu) ---\n%.*ls", pid_, (int)n, text);
            if (text[n - 1] != L'\n') fwprintf(stdout, L"\n");
        }
        fwprintf(stdout, L"dx12track: elevated instance (pid %lu) exited with code %lu.\n", pid_, code);
    }

    SetConsoleCtrlHandler(WaitCtrlHandler, FALSE);
    g_wait_console_state = nullptr;
    return (int)code;
}

// ===========================================================================
// ElevatedSession (elevated instance)
// ===========================================================================

ElevatedSession::~ElevatedSession() {
    if (hdr_) UnmapViewOfFile(hdr_);
    if (mapping_) CloseHandle(mapping_);
    if (original_) CloseHandle(original_);
    if (shell_) CloseHandle(shell_);
}

bool ElevatedSession::Begin(DWORD original_pid) {
    original_pid_ = original_pid;
    const bool self = original_pid == GetCurrentProcessId();

    // Open the original right away (before it could exit and its pid be
    // reused) - it's the parent for the target.
    const DWORD access = PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    original_ = OpenProcess(access, FALSE, original_pid);
    DWORD open_err = original_ ? 0 : GetLastError();
    if (!original_ && open_err == ERROR_ACCESS_DENIED && EnablePrivilege(SE_DEBUG_NAME)) {
        original_ = OpenProcess(access, FALSE, original_pid);
        open_err = original_ ? 0 : GetLastError();
    }
    // The original is gone (e.g. the UAC prompt was accepted after it had
    // been closed): nobody is waiting for us, don't start the target.
    if ((!original_ && open_err == ERROR_INVALID_PARAMETER) ||
        (original_ && WaitForSingleObject(original_, 0) == WAIT_OBJECT_0))
        return false;

    if (!self) OpenHandoff();

    // Same cwd as the original so relative paths (-o, the default
    // dx12track.jsonl, the target) resolve the same way.
    DWORD cwd_err = 0;
    std::wstring cwd;
    if (hdr_ && hdr_->cwd_chars) {
        cwd.assign(reinterpret_cast<const wchar_t*>(reinterpret_cast<BYTE*>(hdr_) + hdr_->cwd_off),
                   hdr_->cwd_chars);
        if (!SetCurrentDirectoryW(cwd.c_str())) cwd_err = GetLastError();
    }

    DWORD attach_err = 0;
    if (!self) {
        SwitchConsole();
        if (own_console_) attach_err = GetLastError();
    }

    // Report (after the console switch so it lands where the user looks).
    auto say = [this](FILE* f, const wchar_t* fmt, auto... args) {
        wchar_t buf[1024];
        int n = swprintf(buf, 1024, fmt, args...);
        if (n < 0) return;
        fputws(buf, f);
        Forward(buf, (size_t)n);
    };
    ProcessElevation me = QueryProcessElevation(GetCurrentProcess());
    say(stdout, L"dx12track pid %lu runs %ls (%ls integrity) for launcher pid %lu.\n",
        GetCurrentProcessId(), me.elevated ? L"elevated" : L"non-elevated",
        IntegrityName(me.integrity), original_pid);
    if (own_console_)
        say(stderr, L"Could not attach to the console of pid %lu (%ls); using this window.\n",
            original_pid, FormatWin32Error(attach_err).c_str());
    if (!original_)
        say(stderr, L"Cannot open launcher pid %lu as parent for the target: %ls\n",
            original_pid, FormatWin32Error(open_err).c_str());
    if (!self && !hdr_)
        say(stderr, L"No handoff from pid %lu; the target gets this instance's environment.\n",
            original_pid);
    if (cwd_err)
        say(stderr, L"Cannot change to the original working directory %ls: %ls\n",
            cwd.c_str(), FormatWin32Error(cwd_err).c_str());
    return true;
}

void ElevatedSession::OpenHandoff() {
    const std::wstring name = HandoffName(original_pid_);
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (!mapping_) return;
    auto* hdr = static_cast<HandoffHeader*>(
        MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (!hdr) return;
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(hdr, &mbi, sizeof(mbi));
    auto fits = [&](uint32_t off_bytes, uint32_t chars) {
        return (uint64_t)off_bytes + (uint64_t)chars * sizeof(wchar_t) <= hdr->total_bytes;
    };
    if (mbi.RegionSize < sizeof(HandoffHeader) || hdr->magic != kHandoffMagic ||
        hdr->version != kHandoffVersion || hdr->total_bytes > mbi.RegionSize ||
        !fits(hdr->cwd_off, hdr->cwd_chars) || !fits(hdr->sid_off, hdr->sid_chars) ||
        !fits(hdr->env_off, hdr->env_chars) || !fits(hdr->out_off, hdr->out_cap)) {
        UnmapViewOfFile(hdr);
        return;
    }
    hdr_ = hdr;
    const BYTE* base = reinterpret_cast<const BYTE*>(hdr_);
    original_sid_.assign(reinterpret_cast<const wchar_t*>(base + hdr_->sid_off), hdr_->sid_chars);
    const wchar_t* env = reinterpret_cast<const wchar_t*>(base + hdr_->env_off);
    original_env_.assign(env, env + hdr_->env_chars);
    original_env_.push_back(L'\0');   // guarantee termination
    original_env_.push_back(L'\0');
}

void ElevatedSession::SwitchConsole() {
    FreeConsole();
    if (AttachConsole(original_pid_)) {
        own_console_ = false;
    } else {
        const DWORD err = GetLastError();
        own_console_ = true;
        AllocConsole();
        SetConsoleTitleW(L"dx12track (elevated)");
        if (HWND w = GetConsoleWindow()) {
            ShowWindow(w, SW_SHOWNORMAL);
            SetForegroundWindow(w);
        }
        SetLastError(err);
    }
    const DWORD saved_err = GetLastError();

    // CRT streams first (reopening fd 0-2 makes the CRT call SetStdHandle with
    // its own write-only handle), then our read/write handles as the Win32
    // std handles - the Renderer needs GENERIC_READ for
    // GetConsoleScreenBufferInfo.
    FILE* f = nullptr;
    _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
    _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
    _wfreopen_s(&f, L"CONIN$", L"r", stdin);
    setvbuf(stderr, nullptr, _IONBF, 0);
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE in  = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, out);
        SetStdHandle(STD_ERROR_HANDLE, out);
    }
    if (in != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, in);

    if (hdr_) InterlockedExchange(&hdr_->console_state, own_console_ ? kConsoleOwn : kConsoleAttached);
    SetLastError(saved_err);
}

HANDLE ElevatedSession::ParentProcess(std::wstring* which) {
    if (original_ && WaitForSingleObject(original_, 0) == WAIT_TIMEOUT) {
        *which = L"launcher pid " + std::to_wstring(original_pid_);
        return original_;
    }
    if (!shell_) {
        DWORD shell_pid = 0;
        if (HWND sw = GetShellWindow()) GetWindowThreadProcessId(sw, &shell_pid);
        if (shell_pid)
            shell_ = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, shell_pid);
        if (shell_) *which = L"shell pid " + std::to_wstring(shell_pid);
    }
    if (shell_ && which->empty()) *which = L"shell";
    return shell_;
}

std::vector<wchar_t> ElevatedSession::BuildEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>>& vars) const {
    std::vector<std::wstring> entries;
    if (!original_env_.empty()) {
        SplitEnvBlock(original_env_.data(), &entries);
    } else if (LPWCH block = GetEnvironmentStringsW()) {
        SplitEnvBlock(block, &entries);
        FreeEnvironmentStringsW(block);
    }

    static const wchar_t kPrefix[] = L"DX12TRACK_";
    const int prefix_len = (int)(sizeof(kPrefix) / sizeof(wchar_t)) - 1;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
        return EnvNameLen(e) >= (size_t)prefix_len &&
               CompareStringOrdinal(e.c_str(), prefix_len, kPrefix, prefix_len, TRUE) == CSTR_EQUAL;
    }), entries.end());
    for (auto& kv : vars) entries.push_back(kv.first + L"=" + kv.second);

    // Environment blocks are sorted by name, case-insensitively.
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), (int)EnvNameLen(a),
                                    b.c_str(), (int)EnvNameLen(b), TRUE) == CSTR_LESS_THAN;
    });

    std::vector<wchar_t> block;
    for (auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1) block.push_back(L'\0');
    return block;
}

void ElevatedSession::Forward(const wchar_t* text, size_t chars) {
    if (!own_console_ || !hdr_ || !chars) return;
    const LONG used = hdr_->out_chars;
    if (used < 0 || (uint32_t)used >= hdr_->out_cap) return;
    const size_t n = std::min<size_t>(chars, hdr_->out_cap - (uint32_t)used);
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(hdr_) + hdr_->out_off);
    memcpy(dst + used, text, n * sizeof(wchar_t));
    InterlockedExchange(&hdr_->out_chars, used + (LONG)n);
}

// ===========================================================================

bool CreateProcessWithParent(HANDLE parent, const std::wstring& exe,
                             std::wstring& cmdline, const wchar_t* cwd,
                             std::vector<wchar_t>& env, PROCESS_INFORMATION* pi) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> storage(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return false;

    bool ok = false;
    if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                  &parent, sizeof(parent), nullptr, nullptr)) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = list;
        ok = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                                CREATE_UNICODE_ENVIRONMENT,
                            env.data(), cwd, &si.StartupInfo, pi) != 0;
    }
    const DWORD err = GetLastError();
    DeleteProcThreadAttributeList(list);
    SetLastError(err);
    return ok;
}

} // namespace dx12track
