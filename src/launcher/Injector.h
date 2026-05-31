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
InjectionResult InjectDll(HANDLE process_handle, const std::wstring& dll_path);

} // namespace dx12track
