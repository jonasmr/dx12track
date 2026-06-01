#include "Hooks.h"

#include "DeviceHooks.h"
#include "Diag.h"
#include "PipeClient.h"

#include <atomic>
#include <cstdio>
#include <directx/d3d12.h>
#include <MinHook.h>
#include <mutex>

namespace dx12track {

namespace {

// ---- Original D3D12CreateDevice trampoline ---------------------------------

using PFN_D3D12CreateDevice_t = HRESULT (WINAPI*)(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);

PFN_D3D12CreateDevice_t g_real_D3D12CreateDevice = nullptr;
std::atomic<bool>       g_hooks_installed{false};

// Track which device vtables we've already patched so we never patch the same
// vtable twice (re-patching is harmless but pointless).
std::mutex                                g_patched_vtables_mu;
struct VTableSet { void* vt[8] = {}; size_t n = 0; };
VTableSet                                 g_patched_vtables;

bool AlreadyPatched(void* vt) {
    std::lock_guard<std::mutex> l(g_patched_vtables_mu);
    for (size_t i = 0; i < g_patched_vtables.n; ++i)
        if (g_patched_vtables.vt[i] == vt) return true;
    if (g_patched_vtables.n < 8) g_patched_vtables.vt[g_patched_vtables.n++] = vt;
    return false;
}

// ---- VTable patch primitive ------------------------------------------------

// Returns true if the slot was actually overwritten. Logs the address mapping
// (slot index, what was there, what we put there) in verbose mode — the diag
// stream is what the user grepps when nothing seems to be tracking.
bool PatchSlot(void** vtable, size_t index, const char* slot_name,
               void* hook, void** out_prev) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
        DiagF("PatchSlot[%zu %s] VirtualProtect FAILED at 0x%llx, err=%lu",
              index, slot_name,
              (unsigned long long)(uintptr_t)&vtable[index], GetLastError());
        return false;
    }
    void* prev = vtable[index];
    if (out_prev) *out_prev = prev;
    vtable[index] = hook;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old, &ignored);
    DiagF("PatchSlot[%zu %s] vtable=0x%llx prev=0x%llx hook=0x%llx",
          index, slot_name,
          (unsigned long long)(uintptr_t)vtable,
          (unsigned long long)(uintptr_t)prev,
          (unsigned long long)(uintptr_t)hook);
    return true;
}

// Probe for the highest-supported ID3D12DeviceN interface so we know which
// vtable slots are valid to patch. Returns -1 if no Device interface succeeds.
//
// The (version, IID) table is built so that newer interfaces are listed only
// when the headers we're compiling against actually declare them. That lets us
// build cleanly against older directx-headers releases without losing the
// ability to probe newer interfaces when a newer header set is in use.
int ProbeMaxDeviceVersion(IUnknown* obj) {
    struct Entry { int version; IID iid; };
    static const Entry entries[] = {
#ifdef __ID3D12Device15_INTERFACE_DEFINED__
        { 15, __uuidof(ID3D12Device15) },
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
        { 14, __uuidof(ID3D12Device14) },
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
        { 13, __uuidof(ID3D12Device13) },
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
        { 12, __uuidof(ID3D12Device12) },
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
        { 11, __uuidof(ID3D12Device11) },
#endif
        { 10, __uuidof(ID3D12Device10) },
        {  9, __uuidof(ID3D12Device9)  },
        {  8, __uuidof(ID3D12Device8)  },
        {  7, __uuidof(ID3D12Device7)  },
        {  6, __uuidof(ID3D12Device6)  },
        {  5, __uuidof(ID3D12Device5)  },
        {  4, __uuidof(ID3D12Device4)  },
        {  3, __uuidof(ID3D12Device3)  },
        {  2, __uuidof(ID3D12Device2)  },
        {  1, __uuidof(ID3D12Device1)  },
        {  0, __uuidof(ID3D12Device)   },
    };
    for (const auto& e : entries) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(obj->QueryInterface(e.iid, reinterpret_cast<void**>(&p)))) {
            p->Release();
            return e.version;
        }
    }
    return -1;
}

DeviceVtableOriginals g_dev_originals;

} // namespace

DeviceVtableOriginals& DeviceOriginals() { return g_dev_originals; }

void PatchDeviceVTable(void* device_iunknown) {
    if (!device_iunknown) return;
    auto* obj = static_cast<IUnknown*>(device_iunknown);
    void** vtable = *reinterpret_cast<void***>(obj);

    if (AlreadyPatched(vtable)) {
        DiagF("PatchDeviceVTable: vtable 0x%llx already patched, skipping",
              (unsigned long long)(uintptr_t)vtable);
        return;
    }

    int v = ProbeMaxDeviceVersion(obj);
    DiagF("PatchDeviceVTable: device=0x%llx vtable=0x%llx max_device_version=%d",
          (unsigned long long)(uintptr_t)device_iunknown,
          (unsigned long long)(uintptr_t)vtable, v);
    if (v < 0) return;

    // ID3D12Device (always present if v >= 0).
    PatchSlot(vtable, 8,  "CreateCommandQueue", reinterpret_cast<void*>(&Hook_CreateCommandQueue),
              &g_dev_originals.CreateCommandQueue);
    PatchSlot(vtable, 9,  "CreateCommandAllocator", reinterpret_cast<void*>(&Hook_CreateCommandAllocator),
              &g_dev_originals.CreateCommandAllocator);
    PatchSlot(vtable, 10, "CreateGraphicsPipelineState", reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState),
              &g_dev_originals.CreateGraphicsPipelineState);
    PatchSlot(vtable, 11, "CreateComputePipelineState", reinterpret_cast<void*>(&Hook_CreateComputePipelineState),
              &g_dev_originals.CreateComputePipelineState);
    PatchSlot(vtable, 12, "CreateCommandList", reinterpret_cast<void*>(&Hook_CreateCommandList),
              &g_dev_originals.CreateCommandList);
    PatchSlot(vtable, 14, "CreateDescriptorHeap", reinterpret_cast<void*>(&Hook_CreateDescriptorHeap),
              &g_dev_originals.CreateDescriptorHeap);
    PatchSlot(vtable, 16, "CreateRootSignature", reinterpret_cast<void*>(&Hook_CreateRootSignature),
              &g_dev_originals.CreateRootSignature);
    PatchSlot(vtable, 27, "CreateCommittedResource", reinterpret_cast<void*>(&Hook_CreateCommittedResource),
              &g_dev_originals.CreateCommittedResource);
    PatchSlot(vtable, 28, "CreateHeap", reinterpret_cast<void*>(&Hook_CreateHeap),
              &g_dev_originals.CreateHeap);
    PatchSlot(vtable, 29, "CreatePlacedResource", reinterpret_cast<void*>(&Hook_CreatePlacedResource),
              &g_dev_originals.CreatePlacedResource);
    PatchSlot(vtable, 30, "CreateReservedResource", reinterpret_cast<void*>(&Hook_CreateReservedResource),
              &g_dev_originals.CreateReservedResource);
    PatchSlot(vtable, 36, "CreateFence", reinterpret_cast<void*>(&Hook_CreateFence),
              &g_dev_originals.CreateFence);
    PatchSlot(vtable, 39, "CreateQueryHeap", reinterpret_cast<void*>(&Hook_CreateQueryHeap),
              &g_dev_originals.CreateQueryHeap);
    PatchSlot(vtable, 41, "CreateCommandSignature", reinterpret_cast<void*>(&Hook_CreateCommandSignature),
              &g_dev_originals.CreateCommandSignature);

    if (v >= 2)
        PatchSlot(vtable, 47, "CreatePipelineState", reinterpret_cast<void*>(&Hook_CreatePipelineState),
                  &g_dev_originals.CreatePipelineState);

    if (v >= 4) {
        PatchSlot(vtable, 51, "CreateCommandList1", reinterpret_cast<void*>(&Hook_CreateCommandList1),
                  &g_dev_originals.CreateCommandList1);
        PatchSlot(vtable, 53, "CreateCommittedResource1", reinterpret_cast<void*>(&Hook_CreateCommittedResource1),
                  &g_dev_originals.CreateCommittedResource1);
        PatchSlot(vtable, 54, "CreateHeap1", reinterpret_cast<void*>(&Hook_CreateHeap1),
                  &g_dev_originals.CreateHeap1);
        PatchSlot(vtable, 55, "CreateReservedResource1", reinterpret_cast<void*>(&Hook_CreateReservedResource1),
                  &g_dev_originals.CreateReservedResource1);
    }

    if (v >= 8) {
        PatchSlot(vtable, 69, "CreateCommittedResource2", reinterpret_cast<void*>(&Hook_CreateCommittedResource2),
                  &g_dev_originals.CreateCommittedResource2);
        PatchSlot(vtable, 70, "CreatePlacedResource1", reinterpret_cast<void*>(&Hook_CreatePlacedResource1),
                  &g_dev_originals.CreatePlacedResource1);
    }

    if (v >= 9)
        PatchSlot(vtable, 75, "CreateCommandQueue1", reinterpret_cast<void*>(&Hook_CreateCommandQueue1),
                  &g_dev_originals.CreateCommandQueue1);

    if (v >= 10) {
        PatchSlot(vtable, 76, "CreateCommittedResource3", reinterpret_cast<void*>(&Hook_CreateCommittedResource3),
                  &g_dev_originals.CreateCommittedResource3);
        PatchSlot(vtable, 77, "CreatePlacedResource2", reinterpret_cast<void*>(&Hook_CreatePlacedResource2),
                  &g_dev_originals.CreatePlacedResource2);
        PatchSlot(vtable, 78, "CreateReservedResource2", reinterpret_cast<void*>(&Hook_CreateReservedResource2),
                  &g_dev_originals.CreateReservedResource2);
    }
}

// ---- The D3D12CreateDevice hook -------------------------------------------

HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* pAdapter,
                                       D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                       REFIID riid, void** ppDevice) {
    DiagF("Hook_D3D12CreateDevice fired: adapter=0x%llx feature_level=0x%x",
          (unsigned long long)(uintptr_t)pAdapter,
          (unsigned)MinimumFeatureLevel);
    HRESULT hr = g_real_D3D12CreateDevice(pAdapter, MinimumFeatureLevel,
                                          riid, ppDevice);
    DiagF("Hook_D3D12CreateDevice returned: hr=0x%08x device=0x%llx",
          (unsigned)hr,
          (ppDevice && *ppDevice)
              ? (unsigned long long)(uintptr_t)*ppDevice : 0ull);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        PatchDeviceVTable(*ppDevice);
    }
    return hr;
}

// ---- Install / Remove -----------------------------------------------------

bool InstallExportHooks() {
    if (g_hooks_installed.exchange(true)) return true;

    MH_STATUS mh_init = MH_Initialize();
    DiagF("InstallExportHooks: MH_Initialize = %d (%s)", (int)mh_init,
          mh_init == MH_OK ? "OK" : "FAILED");
    if (mh_init != MH_OK) return false;

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        DiagF("InstallExportHooks: LoadLibraryW(d3d12.dll) FAILED, err=%lu",
              GetLastError());
        return false;
    }
    wchar_t d3d12_path[MAX_PATH] = {};
    GetModuleFileNameW(d3d12, d3d12_path, MAX_PATH);
    DiagF("InstallExportHooks: d3d12.dll handle=0x%llx path=%ls",
          (unsigned long long)(uintptr_t)d3d12, d3d12_path);

    auto target = GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!target) {
        DiagF("InstallExportHooks: GetProcAddress(D3D12CreateDevice) FAILED");
        return false;
    }
    DiagF("InstallExportHooks: D3D12CreateDevice export at 0x%llx, hook at 0x%llx",
          (unsigned long long)(uintptr_t)target,
          (unsigned long long)(uintptr_t)&Hook_D3D12CreateDevice);

    MH_STATUS mh_create = MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Hook_D3D12CreateDevice),
        reinterpret_cast<LPVOID*>(&g_real_D3D12CreateDevice));
    DiagF("InstallExportHooks: MH_CreateHook = %d (%s), trampoline=0x%llx",
          (int)mh_create, mh_create == MH_OK ? "OK" : "FAILED",
          (unsigned long long)(uintptr_t)g_real_D3D12CreateDevice);
    if (mh_create != MH_OK) return false;

    MH_STATUS mh_enable = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    DiagF("InstallExportHooks: MH_EnableHook = %d (%s)",
          (int)mh_enable, mh_enable == MH_OK ? "OK" : "FAILED");
    if (mh_enable != MH_OK) return false;

    return true;
}

void RemoveAllHooks() {
    if (!g_hooks_installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    // We do NOT un-patch the D3D12 vtables: the application may still hold
    // device pointers and the originals could now be in our trampolines, which
    // are about to vanish. Leaving the vtable patched is fine because our hook
    // bodies degrade to no-ops once the global pipe / log are closed.
}

} // namespace dx12track
