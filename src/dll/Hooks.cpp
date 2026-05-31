#include "Hooks.h"

#include "DeviceHooks.h"
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

void PatchSlot(void** vtable, size_t index, void* hook, void** out_prev) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old))
        return;
    if (out_prev) *out_prev = vtable[index];
    vtable[index] = hook;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old, &ignored);
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

    if (AlreadyPatched(vtable)) return;

    int v = ProbeMaxDeviceVersion(obj);
    if (v < 0) return;

    // ID3D12Device (always present if v >= 0).
    PatchSlot(vtable, 8,  reinterpret_cast<void*>(&Hook_CreateCommandQueue),
              &g_dev_originals.CreateCommandQueue);
    PatchSlot(vtable, 9,  reinterpret_cast<void*>(&Hook_CreateCommandAllocator),
              &g_dev_originals.CreateCommandAllocator);
    PatchSlot(vtable, 10, reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState),
              &g_dev_originals.CreateGraphicsPipelineState);
    PatchSlot(vtable, 11, reinterpret_cast<void*>(&Hook_CreateComputePipelineState),
              &g_dev_originals.CreateComputePipelineState);
    PatchSlot(vtable, 12, reinterpret_cast<void*>(&Hook_CreateCommandList),
              &g_dev_originals.CreateCommandList);
    PatchSlot(vtable, 14, reinterpret_cast<void*>(&Hook_CreateDescriptorHeap),
              &g_dev_originals.CreateDescriptorHeap);
    PatchSlot(vtable, 16, reinterpret_cast<void*>(&Hook_CreateRootSignature),
              &g_dev_originals.CreateRootSignature);
    PatchSlot(vtable, 27, reinterpret_cast<void*>(&Hook_CreateCommittedResource),
              &g_dev_originals.CreateCommittedResource);
    PatchSlot(vtable, 28, reinterpret_cast<void*>(&Hook_CreateHeap),
              &g_dev_originals.CreateHeap);
    PatchSlot(vtable, 29, reinterpret_cast<void*>(&Hook_CreatePlacedResource),
              &g_dev_originals.CreatePlacedResource);
    PatchSlot(vtable, 30, reinterpret_cast<void*>(&Hook_CreateReservedResource),
              &g_dev_originals.CreateReservedResource);
    PatchSlot(vtable, 36, reinterpret_cast<void*>(&Hook_CreateFence),
              &g_dev_originals.CreateFence);
    PatchSlot(vtable, 39, reinterpret_cast<void*>(&Hook_CreateQueryHeap),
              &g_dev_originals.CreateQueryHeap);
    PatchSlot(vtable, 41, reinterpret_cast<void*>(&Hook_CreateCommandSignature),
              &g_dev_originals.CreateCommandSignature);

    if (v >= 2)
        PatchSlot(vtable, 47, reinterpret_cast<void*>(&Hook_CreatePipelineState),
                  &g_dev_originals.CreatePipelineState);

    if (v >= 4) {
        PatchSlot(vtable, 51, reinterpret_cast<void*>(&Hook_CreateCommandList1),
                  &g_dev_originals.CreateCommandList1);
        PatchSlot(vtable, 53, reinterpret_cast<void*>(&Hook_CreateCommittedResource1),
                  &g_dev_originals.CreateCommittedResource1);
        PatchSlot(vtable, 54, reinterpret_cast<void*>(&Hook_CreateHeap1),
                  &g_dev_originals.CreateHeap1);
        PatchSlot(vtable, 55, reinterpret_cast<void*>(&Hook_CreateReservedResource1),
                  &g_dev_originals.CreateReservedResource1);
    }

    if (v >= 8) {
        PatchSlot(vtable, 69, reinterpret_cast<void*>(&Hook_CreateCommittedResource2),
                  &g_dev_originals.CreateCommittedResource2);
        PatchSlot(vtable, 70, reinterpret_cast<void*>(&Hook_CreatePlacedResource1),
                  &g_dev_originals.CreatePlacedResource1);
    }

    if (v >= 9)
        PatchSlot(vtable, 75, reinterpret_cast<void*>(&Hook_CreateCommandQueue1),
                  &g_dev_originals.CreateCommandQueue1);

    if (v >= 10) {
        PatchSlot(vtable, 76, reinterpret_cast<void*>(&Hook_CreateCommittedResource3),
                  &g_dev_originals.CreateCommittedResource3);
        PatchSlot(vtable, 77, reinterpret_cast<void*>(&Hook_CreatePlacedResource2),
                  &g_dev_originals.CreatePlacedResource2);
        PatchSlot(vtable, 78, reinterpret_cast<void*>(&Hook_CreateReservedResource2),
                  &g_dev_originals.CreateReservedResource2);
    }
}

// ---- The D3D12CreateDevice hook -------------------------------------------

HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* pAdapter,
                                       D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                       REFIID riid, void** ppDevice) {
    HRESULT hr = g_real_D3D12CreateDevice(pAdapter, MinimumFeatureLevel,
                                          riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        PatchDeviceVTable(*ppDevice);
    }
    return hr;
}

// ---- Install / Remove -----------------------------------------------------

bool InstallExportHooks() {
    if (g_hooks_installed.exchange(true)) return true;

    if (MH_Initialize() != MH_OK) {
        return false;
    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) return false;

    auto target = GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!target) return false;

    if (MH_CreateHook(reinterpret_cast<LPVOID>(target),
                      reinterpret_cast<LPVOID>(&Hook_D3D12CreateDevice),
                      reinterpret_cast<LPVOID*>(&g_real_D3D12CreateDevice)) != MH_OK)
        return false;
    if (MH_EnableHook(reinterpret_cast<LPVOID>(target)) != MH_OK)
        return false;

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
