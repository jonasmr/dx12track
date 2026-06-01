#pragma once

#include <string>
#include <windows.h>

namespace dx12track {

struct InjectionResult {
    bool   ok;
    DWORD  last_error;
    std::wstring message;   // populated on failure
};

// Inject `dll_path` into `process_handle` by allocating remote memory and
// calling LoadLibraryW via CreateRemoteThread. The process should be created
// suspended so hooks are armed before the first instruction runs.
// `wait_ms` controls how long we wait for the remote LoadLibraryW to return.
// Pass INFINITE for --debugger mode where DllMain may block on a MessageBox.
InjectionResult InjectDll(HANDLE process_handle, const std::wstring& dll_path,
                          DWORD wait_ms = 30000);

} // namespace dx12track
