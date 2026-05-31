// dx12track.h — public C API for in-process self-instrumentation.
//
// Usage (host program):
//
//     #include <windows.h>
//     #include "dx12track.h"
//
//     int wWinMain(...) {
//         HMODULE dll = LoadLibraryW(L"dx12track.dll");
//         if (dll) {
//             auto start = (Dx12Track_StartLogging_t)GetProcAddress(
//                 dll, "Dx12Track_StartLogging");
//             if (start) start(L"my-run.jsonl", /*capture_callstacks=*/TRUE);
//         }
//         // ... InitD3D12, run the game ...
//     }
//
// You can also link against dx12track.lib (produced alongside the DLL) and
// call the functions directly without GetProcAddress; the DX12TRACK_API macro
// resolves to dllimport in client TUs.
//
// Calling LoadLibraryW("dx12track.dll") is enough to arm the hooks — the
// D3D12 hooks install in DllMain. Dx12Track_StartLogging is what makes them
// *visible* by opening the JSONL log; without it events are tracked in memory
// but never written anywhere. Call StartLogging before D3D12CreateDevice if
// you want to capture the device-creation event itself.

#ifndef DX12TRACK_H_
#define DX12TRACK_H_

#include <windows.h>

#if defined(DX12TRACK_BUILDING_DLL)
#  define DX12TRACK_API __declspec(dllexport)
#else
#  define DX12TRACK_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Open `jsonl_path` for append-write and start emitting events to it. If
// `capture_callstacks` is TRUE, every subsequent Create* hook captures a
// stack via RtlCaptureStackBackTrace and an initial enumeration of loaded
// modules is emitted (with PDB GUID + Age) so an offline resolver can find
// symbols.
//
// Safe to call multiple times — re-opening switches the log to a new path.
// Returns TRUE on success.
DX12TRACK_API BOOL Dx12Track_StartLogging(const wchar_t* jsonl_path,
                                          BOOL capture_callstacks);

// Flush and close the JSONL log. Hooks remain installed; future Create/Release
// events go nowhere until StartLogging is called again. Optional — DllMain on
// DLL_PROCESS_DETACH does the same cleanup.
DX12TRACK_API void Dx12Track_StopLogging(void);

// Convenience function pointer typedefs for GetProcAddress users.
typedef BOOL (*Dx12Track_StartLogging_t)(const wchar_t*, BOOL);
typedef void (*Dx12Track_StopLogging_t)(void);

#ifdef __cplusplus
}
#endif

#endif // DX12TRACK_H_
