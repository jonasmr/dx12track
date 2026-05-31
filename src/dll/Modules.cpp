#include "Modules.h"

#include "EventTypes.h"
#include "JsonLog.h"
#include "PipeClient.h"

#include <atomic>
#include <cstring>
#include <psapi.h>
#include <winternl.h>

// LdrRegisterDllNotification lives in ntdll. The structures aren't in the
// public Windows SDK headers; declare what we need locally.
extern "C" {

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG            Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID            DllBase;
    ULONG            SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
    ULONG            Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID            DllBase;
    ULONG            SizeOfImage;
} LDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA   Loaded;
    LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA;

#define LDR_DLL_NOTIFICATION_REASON_LOADED   1
#define LDR_DLL_NOTIFICATION_REASON_UNLOADED 2

typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    const LDR_DLL_NOTIFICATION_DATA* NotificationData,
    PVOID Context);

typedef NTSTATUS (NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG Flags, PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context, PVOID* Cookie);

typedef NTSTATUS (NTAPI *PFN_LdrUnregisterDllNotification)(PVOID Cookie);

} // extern "C"

namespace dx12track {

namespace {

std::atomic<bool> g_started{false};
PVOID             g_cookie = nullptr;
PFN_LdrUnregisterDllNotification g_unregister = nullptr;

uint64_t NowNs() {
    static LARGE_INTEGER freq{}, start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    long double ns = (long double)(now.QuadPart - start.QuadPart) * 1e9L /
                     (long double)freq.QuadPart;
    return (uint64_t)ns;
}

// Locate the IMAGE_NT_HEADERS in a module mapped at `base`.
const IMAGE_NT_HEADERS* NtHeaders(const void* base) {
    auto dos = static_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        static_cast<const BYTE*>(base) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

// Walk the debug directory looking for the CV_INFO_PDB70 record ("RSDS").
// Fills `out_guid`, `out_age`, and copies the PDB path. Returns false if the
// module has no CV record (rare for binaries built with /DEBUG).
struct PdbInfo { GUID guid; DWORD age; char pdb_path[260]; bool found; };

PdbInfo ExtractPdbInfo(const void* base) {
    PdbInfo info{};
    const IMAGE_NT_HEADERS* nt = NtHeaders(base);
    if (!nt) return info;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
        return info;

    const IMAGE_DATA_DIRECTORY& dd =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dd.VirtualAddress || !dd.Size) return info;

    auto* dbgs = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        static_cast<const BYTE*>(base) + dd.VirtualAddress);
    size_t n = dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

#pragma pack(push, 1)
    struct CvInfoPdb70 {
        DWORD CvSignature; // 'SDSR' = 0x53445352
        GUID  Signature;
        DWORD Age;
        char  PdbFileName[1]; // null-terminated, actually variable
    };
#pragma pack(pop)

    for (size_t i = 0; i < n; ++i) {
        if (dbgs[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (!dbgs[i].AddressOfRawData || !dbgs[i].SizeOfData) continue;
        auto* cv = reinterpret_cast<const CvInfoPdb70*>(
            static_cast<const BYTE*>(base) + dbgs[i].AddressOfRawData);
        if (cv->CvSignature != 0x53445352u /*'SDSR'*/) continue;
        info.guid = cv->Signature;
        info.age  = cv->Age;
        // Copy the embedded UTF-8 string, bounded by SizeOfData and our buffer.
        size_t name_max = dbgs[i].SizeOfData -
                          (offsetof(CvInfoPdb70, PdbFileName));
        if (name_max > sizeof(info.pdb_path) - 1)
            name_max = sizeof(info.pdb_path) - 1;
        memcpy(info.pdb_path, cv->PdbFileName, name_max);
        info.pdb_path[name_max] = 0;
        info.found = true;
        return info;
    }
    return info;
}

void EmitModuleLoaded(HMODULE hmod) {
    if (!hmod) return;

    ModuleLoadedPayload p{};
    p.base = reinterpret_cast<uint64_t>(hmod);

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), hmod, &mi, sizeof(mi))) {
        p.size = mi.SizeOfImage;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(hmod, path, MAX_PATH);
    size_t name_chars = (n && n < kMaxNameChars) ? n : 0;
    memcpy(p.name, path, name_chars * sizeof(wchar_t));
    p.name[name_chars] = 0;

    if (const IMAGE_NT_HEADERS* nt = NtHeaders(hmod)) {
        p.timestamp = nt->FileHeader.TimeDateStamp;
    }

    PdbInfo pi = ExtractPdbInfo(hmod);
    if (pi.found) {
        memcpy(p.pdb_guid, &pi.guid, sizeof(p.pdb_guid));
        p.pdb_age = pi.age;
        int wc = MultiByteToWideChar(CP_UTF8, 0, pi.pdb_path, -1,
                                     p.pdb_name, kMaxNameChars);
        if (wc <= 0) p.pdb_name[0] = 0;
    }

    GlobalPipe().Send(EventKind::ModuleLoaded, &p, sizeof(p));
    GlobalLog().Append(EventKind::ModuleLoaded, NowNs(), &p, sizeof(p));
}

void EmitModuleUnloaded(uint64_t base) {
    ModuleUnloadedPayload p{}; p.base = base;
    GlobalPipe().Send(EventKind::ModuleUnloaded, &p, sizeof(p));
    GlobalLog().Append(EventKind::ModuleUnloaded, NowNs(), &p, sizeof(p));
}

// LDR notification callback runs inside the loader lock — avoid LoadLibrary,
// avoid heap operations that could re-enter the loader. Pipe + file writes
// are fine.
VOID CALLBACK OnLdrNotification(ULONG reason,
                                 const LDR_DLL_NOTIFICATION_DATA* data,
                                 PVOID /*ctx*/) {
    if (!data) return;
    if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
        EmitModuleLoaded(reinterpret_cast<HMODULE>(data->Loaded.DllBase));
    } else if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
        EmitModuleUnloaded(reinterpret_cast<uint64_t>(data->Unloaded.DllBase));
    }
}

void EnumerateExistingModules() {
    HMODULE buf[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), buf, sizeof(buf), &needed))
        return;
    DWORD n = needed / sizeof(HMODULE);
    if (n > 1024) n = 1024;
    for (DWORD i = 0; i < n; ++i)
        EmitModuleLoaded(buf[i]);
}

} // namespace

void StartModuleTracking() {
    if (g_started.exchange(true)) return;

    EnumerateExistingModules();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    g_unregister = reinterpret_cast<PFN_LdrUnregisterDllNotification>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    if (reg) {
        reg(0, &OnLdrNotification, nullptr, &g_cookie);
    }
}

void StopModuleTracking() {
    if (g_unregister && g_cookie) {
        g_unregister(g_cookie);
        g_cookie = nullptr;
    }
}

} // namespace dx12track
