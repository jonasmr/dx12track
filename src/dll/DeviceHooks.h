#pragma once

#include <windows.h>
#include <directx/d3d12.h>

// Forward decls keep this header light.
namespace dx12track {

// ID3D12Device --------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
    ID3D12Device* This, const D3D12_COMMAND_QUEUE_DESC* pDesc,
    REFIID riid, void** ppCommandQueue);

HRESULT STDMETHODCALLTYPE Hook_CreateCommandAllocator(
    ID3D12Device* This, D3D12_COMMAND_LIST_TYPE type,
    REFIID riid, void** ppCommandAllocator);

HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(
    ID3D12Device* This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState);

HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(
    ID3D12Device* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState);

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
    ID3D12Device* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState,
    REFIID riid, void** ppCommandList);

HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(
    ID3D12Device* This, const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
    REFIID riid, void** ppvHeap);

HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(
    ID3D12Device* This, UINT nodeMask, const void* pBlobWithRootSignature,
    SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature);

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
    ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreateHeap(
    ID3D12Device* This, const D3D12_HEAP_DESC* pDesc,
    REFIID riid, void** ppvHeap);

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(
    ID3D12Device* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource(
    ID3D12Device* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreateFence(
    ID3D12Device* This, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags,
    REFIID riid, void** ppFence);

HRESULT STDMETHODCALLTYPE Hook_CreateQueryHeap(
    ID3D12Device* This, const D3D12_QUERY_HEAP_DESC* pDesc,
    REFIID riid, void** ppvHeap);

HRESULT STDMETHODCALLTYPE Hook_CreateCommandSignature(
    ID3D12Device* This, const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
    ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature);

// ID3D12Device2 -------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(
    ID3D12Device2* This, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
    REFIID riid, void** ppPipelineState);

// ID3D12Device4 -------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(
    ID3D12Device4* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppCommandList);

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(
    ID3D12Device4* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreateHeap1(
    ID3D12Device4* This, const D3D12_HEAP_DESC* pDesc,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap);

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource1(
    ID3D12Device4* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource);

// ID3D12Device8 -------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(
    ID3D12Device8* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(
    ID3D12Device8* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource);

// ID3D12Device9 -------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue1(
    ID3D12Device9* This, const D3D12_COMMAND_QUEUE_DESC* pDesc,
    REFIID CreatorID, REFIID riid, void** ppCommandQueue);

// ID3D12Device10 ------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(
    ID3D12Device10* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource2(
    ID3D12Device10* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riid, void** ppvResource);

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource2(
    ID3D12Device10* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riid, void** ppvResource);

} // namespace dx12track
