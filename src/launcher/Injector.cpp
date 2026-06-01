#include "Injector.h"

#include <sstream>

namespace dx12track {

namespace {

std::wstring FormatLastError(DWORD code) {
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring out;
    if (len && buffer) {
        out.assign(buffer, len);
        // Strip trailing CRLF.
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n'))
            out.pop_back();
    }
    if (buffer) LocalFree(buffer);
    if (out.empty()) {
        std::wostringstream s; s << L"error " << code;
        out = s.str();
    }
    return out;
}

} // namespace

InjectionResult InjectDll(HANDLE process_handle, const std::wstring& dll_path,
                          DWORD wait_ms) {
    InjectionResult r{};
    r.ok = false;

    // Get LoadLibraryW from our own kernel32 — the address matches in the target
    // because kernel32.dll loads at the same base in every process on a given boot.
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        r.last_error = GetLastError();
        r.message = L"GetModuleHandleW(kernel32) failed: " + FormatLastError(r.last_error);
        return r;
    }
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    if (!load_library) {
        r.last_error = GetLastError();
        r.message = L"GetProcAddress(LoadLibraryW) failed: " + FormatLastError(r.last_error);
        return r;
    }

    // Copy the wide path (with trailing NUL) into the target's address space.
    const SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process_handle, nullptr, path_bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        r.last_error = GetLastError();
        r.message = L"VirtualAllocEx failed: " + FormatLastError(r.last_error);
        return r;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process_handle, remote, dll_path.c_str(),
                             path_bytes, &written) || written != path_bytes) {
        r.last_error = GetLastError();
        r.message = L"WriteProcessMemory failed: " + FormatLastError(r.last_error);
        VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);
        return r;
    }

    HANDLE thread = CreateRemoteThread(process_handle, nullptr, 0,
                                       load_library, remote, 0, nullptr);
    if (!thread) {
        r.last_error = GetLastError();
        r.message = L"CreateRemoteThread failed: " + FormatLastError(r.last_error);
        VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);
        return r;
    }

    // Wait for LoadLibraryW to return inside the target.
    DWORD wait = WaitForSingleObject(thread, wait_ms);
    if (wait != WAIT_OBJECT_0) {
        r.last_error = GetLastError();
        r.message = L"WaitForSingleObject(remote LoadLibrary) returned non-signaled";
        CloseHandle(thread);
        VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);
        return r;
    }

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);  // low 32 bits of the returned HMODULE
    CloseHandle(thread);
    VirtualFreeEx(process_handle, remote, 0, MEM_RELEASE);

    if (exit_code == 0) {
        r.message = L"LoadLibraryW returned NULL in target process";
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace dx12track
