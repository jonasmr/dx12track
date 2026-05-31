#pragma once

#include <windows.h>

namespace dx12track {

// Installs MinHook trampolines on d3d12.dll exports. Must be called once at
// DLL load time, before the host thread is resumed.
bool InstallExportHooks();

// Tears down all hooks. Called from DllMain on DLL_PROCESS_DETACH.
void RemoveAllHooks();

// Helpers used by DeviceHooks: take an ID3D12Device-derived COM pointer and
// patch its (shared) vtable for every Create* method we care about. Idempotent.
void PatchDeviceVTable(void* device_iunknown);

// Storage for the original device-method pointers, keyed by vtable slot.
// Populated by PatchDeviceVTable, read by the per-method trampolines.
struct DeviceVtableOriginals {
    void* CreateCommandQueue          = nullptr;  // slot 8
    void* CreateCommandAllocator      = nullptr;  // 9
    void* CreateGraphicsPipelineState = nullptr;  // 10
    void* CreateComputePipelineState  = nullptr;  // 11
    void* CreateCommandList           = nullptr;  // 12
    void* CreateDescriptorHeap        = nullptr;  // 14
    void* CreateRootSignature         = nullptr;  // 16
    void* CreateCommittedResource     = nullptr;  // 27
    void* CreateHeap                  = nullptr;  // 28
    void* CreatePlacedResource        = nullptr;  // 29
    void* CreateReservedResource      = nullptr;  // 30
    void* CreateFence                 = nullptr;  // 36
    void* CreateQueryHeap             = nullptr;  // 39
    void* CreateCommandSignature      = nullptr;  // 41
    void* CreatePipelineState         = nullptr;  // 47   (Device2)
    void* CreateCommandList1          = nullptr;  // 51   (Device4)
    void* CreateCommittedResource1    = nullptr;  // 53   (Device4)
    void* CreateHeap1                 = nullptr;  // 54   (Device4)
    void* CreateReservedResource1     = nullptr;  // 55   (Device4)
    void* CreateCommittedResource2    = nullptr;  // 69   (Device8)
    void* CreatePlacedResource1       = nullptr;  // 70   (Device8)
    void* CreateCommandQueue1         = nullptr;  // 75   (Device9)
    void* CreateCommittedResource3    = nullptr;  // 76   (Device10)
    void* CreatePlacedResource2       = nullptr;  // 77   (Device10)
    void* CreateReservedResource2     = nullptr;  // 78   (Device10)
};

DeviceVtableOriginals& DeviceOriginals();

} // namespace dx12track
