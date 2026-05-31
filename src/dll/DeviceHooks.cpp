#include "DeviceHooks.h"

#include "Hooks.h"
#include "Tracker.h"

#include <cstring>

namespace dx12track {

namespace {

// ---- Real-method signature typedefs ---------------------------------------

using PFN_CreateCommandQueue          = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using PFN_CreateCommandAllocator      = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
using PFN_CreateGraphicsPipelineState = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateComputePipelineState  = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateCommandList           = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using PFN_CreateDescriptorHeap        = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using PFN_CreateRootSignature         = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using PFN_CreateCommittedResource     = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateHeap                  = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using PFN_CreatePlacedResource        = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateReservedResource      = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateFence                 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
using PFN_CreateQueryHeap             = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**);
using PFN_CreateCommandSignature      = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
using PFN_CreatePipelineState         = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using PFN_CreateCommandList1          = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using PFN_CreateCommittedResource1    = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateHeap1                 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateReservedResource1     = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource2    = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreatePlacedResource1       = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateCommandQueue1         = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device9*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID, void**);
using PFN_CreateCommittedResource3    = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using PFN_CreatePlacedResource2       = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using PFN_CreateReservedResource2     = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);

// ---- Helpers --------------------------------------------------------------

D3D12_RESOURCE_DESC ToDesc(const D3D12_RESOURCE_DESC1& d1) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = d1.Dimension;
    d.Alignment        = d1.Alignment;
    d.Width            = d1.Width;
    d.Height           = d1.Height;
    d.DepthOrArraySize = d1.DepthOrArraySize;
    d.MipLevels        = d1.MipLevels;
    d.Format           = d1.Format;
    d.SampleDesc       = d1.SampleDesc;
    d.Layout           = d1.Layout;
    d.Flags            = d1.Flags;
    return d;
}

uint64_t ComputeResourceSize(ID3D12Device* dev, const D3D12_RESOURCE_DESC& desc) {
    // For BUFFER, Width is the size in bytes (and GetResourceAllocationInfo
    // returns that rounded up to 64KB on most adapters). Use Width to avoid
    // surprise rounding and to skip an extra D3D call.
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return desc.Width;
    if (!dev) return 0;
    D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
    return info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
}

ObjectInfo MakeResourceInfo(ID3D12Device* dev,
                             const D3D12_RESOURCE_DESC& desc,
                             AllocationKind alloc,
                             D3D12_HEAP_TYPE heap_type,
                             uint64_t parent_heap_id) {
    ObjectInfo info{};
    info.type           = ObjectType::Resource;
    info.alloc          = alloc;
    info.heap_type      = static_cast<uint32_t>(heap_type);
    info.dimension      = static_cast<uint32_t>(desc.Dimension);
    info.format         = static_cast<uint32_t>(desc.Format);
    info.size_bytes     = ComputeResourceSize(dev, desc);
    info.parent_heap_id = parent_heap_id;
    return info;
}

ObjectInfo MakeHeapInfo(const D3D12_HEAP_DESC& desc) {
    ObjectInfo info{};
    info.type       = ObjectType::Heap;
    info.alloc      = AllocationKind::Heap;
    info.heap_type  = static_cast<uint32_t>(desc.Properties.Type);
    info.size_bytes = desc.SizeInBytes;
    return info;
}

ObjectInfo MakeSimpleInfo(ObjectType t) {
    ObjectInfo info{};
    info.type  = t;
    info.alloc = AllocationKind::None;
    return info;
}

// noinline so the RtlCaptureStackBackTrace skip count below stays correct
// regardless of optimizer choices. From inside Track, FramesToSkip=1 skips
// Track's own frame and starts the trace at the hook function's frame
// (Hook_CreateCommittedResource, etc.), which is the deepest app-visible
// frame in the chain.
__declspec(noinline)
void Track(IUnknown** ppv, ObjectInfo info) {
    if (!ppv || !*ppv) return;
    if (g_capture_callstacks) {
        PVOID frames[kMaxCallstackFrames];
        USHORT n = RtlCaptureStackBackTrace(1, kMaxCallstackFrames,
                                             frames, nullptr);
        info.frame_count = (uint8_t)n;
        for (USHORT i = 0; i < n; ++i)
            info.frames[i] = reinterpret_cast<uint64_t>(frames[i]);
    }
    GlobalTracker().Register(*ppv, info);
}

uint64_t LookupHeapId(ID3D12Heap* heap) {
    return heap ? GlobalTracker().LookupId(heap) : 0;
}

} // namespace

// ===========================================================================
// ID3D12Device
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
    ID3D12Device* This, const D3D12_COMMAND_QUEUE_DESC* pDesc,
    REFIID riid, void** ppCommandQueue) {
    auto fn = reinterpret_cast<PFN_CreateCommandQueue>(
        DeviceOriginals().CreateCommandQueue);
    HRESULT hr = fn(This, pDesc, riid, ppCommandQueue);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppCommandQueue),
              MakeSimpleInfo(ObjectType::CommandQueue));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandAllocator(
    ID3D12Device* This, D3D12_COMMAND_LIST_TYPE type,
    REFIID riid, void** ppCommandAllocator) {
    auto fn = reinterpret_cast<PFN_CreateCommandAllocator>(
        DeviceOriginals().CreateCommandAllocator);
    HRESULT hr = fn(This, type, riid, ppCommandAllocator);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppCommandAllocator),
              MakeSimpleInfo(ObjectType::CommandAllocator));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(
    ID3D12Device* This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState) {
    auto fn = reinterpret_cast<PFN_CreateGraphicsPipelineState>(
        DeviceOriginals().CreateGraphicsPipelineState);
    HRESULT hr = fn(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppPipelineState),
              MakeSimpleInfo(ObjectType::PipelineState));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(
    ID3D12Device* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid, void** ppPipelineState) {
    auto fn = reinterpret_cast<PFN_CreateComputePipelineState>(
        DeviceOriginals().CreateComputePipelineState);
    HRESULT hr = fn(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppPipelineState),
              MakeSimpleInfo(ObjectType::PipelineState));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
    ID3D12Device* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState,
    REFIID riid, void** ppCommandList) {
    auto fn = reinterpret_cast<PFN_CreateCommandList>(
        DeviceOriginals().CreateCommandList);
    HRESULT hr = fn(This, nodeMask, type, pCommandAllocator, pInitialState,
                    riid, ppCommandList);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppCommandList),
              MakeSimpleInfo(ObjectType::CommandList));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(
    ID3D12Device* This, const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
    REFIID riid, void** ppvHeap) {
    auto fn = reinterpret_cast<PFN_CreateDescriptorHeap>(
        DeviceOriginals().CreateDescriptorHeap);
    HRESULT hr = fn(This, pDescriptorHeapDesc, riid, ppvHeap);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppvHeap),
              MakeSimpleInfo(ObjectType::DescriptorHeap));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(
    ID3D12Device* This, UINT nodeMask, const void* pBlobWithRootSignature,
    SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) {
    auto fn = reinterpret_cast<PFN_CreateRootSignature>(
        DeviceOriginals().CreateRootSignature);
    HRESULT hr = fn(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes,
                    riid, ppvRootSignature);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppvRootSignature),
              MakeSimpleInfo(ObjectType::RootSignature));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
    ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riidResource, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateCommittedResource>(
        DeviceOriginals().CreateCommittedResource);
    HRESULT hr = fn(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, riidResource, ppvResource);
    if (SUCCEEDED(hr) && pDesc && pHeapProperties)
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Committed,
                               pHeapProperties->Type, 0));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap(
    ID3D12Device* This, const D3D12_HEAP_DESC* pDesc,
    REFIID riid, void** ppvHeap) {
    auto fn = reinterpret_cast<PFN_CreateHeap>(DeviceOriginals().CreateHeap);
    HRESULT hr = fn(This, pDesc, riid, ppvHeap);
    if (SUCCEEDED(hr) && pDesc)
        Track(reinterpret_cast<IUnknown**>(ppvHeap), MakeHeapInfo(*pDesc));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(
    ID3D12Device* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreatePlacedResource>(
        DeviceOriginals().CreatePlacedResource);
    HRESULT hr = fn(This, pHeap, HeapOffset, pDesc, InitialState,
                    pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc) {
        D3D12_HEAP_TYPE ht = D3D12_HEAP_TYPE_DEFAULT;
        if (pHeap) {
            D3D12_HEAP_DESC d = pHeap->GetDesc();
            ht = d.Properties.Type;
        }
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Placed,
                               ht, LookupHeapId(pHeap)));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource(
    ID3D12Device* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateReservedResource>(
        DeviceOriginals().CreateReservedResource);
    HRESULT hr = fn(This, pDesc, InitialState, pOptimizedClearValue,
                    riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc)
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Reserved,
                               D3D12_HEAP_TYPE_DEFAULT, 0));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateFence(
    ID3D12Device* This, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags,
    REFIID riid, void** ppFence) {
    auto fn = reinterpret_cast<PFN_CreateFence>(DeviceOriginals().CreateFence);
    HRESULT hr = fn(This, InitialValue, Flags, riid, ppFence);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppFence),
              MakeSimpleInfo(ObjectType::Fence));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQueryHeap(
    ID3D12Device* This, const D3D12_QUERY_HEAP_DESC* pDesc,
    REFIID riid, void** ppvHeap) {
    auto fn = reinterpret_cast<PFN_CreateQueryHeap>(
        DeviceOriginals().CreateQueryHeap);
    HRESULT hr = fn(This, pDesc, riid, ppvHeap);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppvHeap),
              MakeSimpleInfo(ObjectType::QueryHeap));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandSignature(
    ID3D12Device* This, const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
    ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) {
    auto fn = reinterpret_cast<PFN_CreateCommandSignature>(
        DeviceOriginals().CreateCommandSignature);
    HRESULT hr = fn(This, pDesc, pRootSignature, riid, ppvCommandSignature);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppvCommandSignature),
              MakeSimpleInfo(ObjectType::CommandSignature));
    return hr;
}

// ===========================================================================
// ID3D12Device2
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(
    ID3D12Device2* This, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
    REFIID riid, void** ppPipelineState) {
    auto fn = reinterpret_cast<PFN_CreatePipelineState>(
        DeviceOriginals().CreatePipelineState);
    HRESULT hr = fn(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppPipelineState),
              MakeSimpleInfo(ObjectType::PipelineState));
    return hr;
}

// ===========================================================================
// ID3D12Device4
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(
    ID3D12Device4* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppCommandList) {
    auto fn = reinterpret_cast<PFN_CreateCommandList1>(
        DeviceOriginals().CreateCommandList1);
    HRESULT hr = fn(This, nodeMask, type, flags, riid, ppCommandList);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppCommandList),
              MakeSimpleInfo(ObjectType::CommandList));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(
    ID3D12Device4* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateCommittedResource1>(
        DeviceOriginals().CreateCommittedResource1);
    HRESULT hr = fn(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, pProtectedSession,
                    riidResource, ppvResource);
    if (SUCCEEDED(hr) && pDesc && pHeapProperties)
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Committed,
                               pHeapProperties->Type, 0));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap1(
    ID3D12Device4* This, const D3D12_HEAP_DESC* pDesc,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap) {
    auto fn = reinterpret_cast<PFN_CreateHeap1>(DeviceOriginals().CreateHeap1);
    HRESULT hr = fn(This, pDesc, pProtectedSession, riid, ppvHeap);
    if (SUCCEEDED(hr) && pDesc)
        Track(reinterpret_cast<IUnknown**>(ppvHeap), MakeHeapInfo(*pDesc));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource1(
    ID3D12Device4* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateReservedResource1>(
        DeviceOriginals().CreateReservedResource1);
    HRESULT hr = fn(This, pDesc, InitialState, pOptimizedClearValue,
                    pProtectedSession, riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc)
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Reserved,
                               D3D12_HEAP_TYPE_DEFAULT, 0));
    return hr;
}

// ===========================================================================
// ID3D12Device8
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(
    ID3D12Device8* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateCommittedResource2>(
        DeviceOriginals().CreateCommittedResource2);
    HRESULT hr = fn(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, pProtectedSession,
                    riidResource, ppvResource);
    if (SUCCEEDED(hr) && pDesc && pHeapProperties) {
        D3D12_RESOURCE_DESC d = ToDesc(*pDesc);
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, d, AllocationKind::Committed,
                               pHeapProperties->Type, 0));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(
    ID3D12Device8* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreatePlacedResource1>(
        DeviceOriginals().CreatePlacedResource1);
    HRESULT hr = fn(This, pHeap, HeapOffset, pDesc, InitialState,
                    pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc) {
        D3D12_RESOURCE_DESC d = ToDesc(*pDesc);
        D3D12_HEAP_TYPE ht = D3D12_HEAP_TYPE_DEFAULT;
        if (pHeap) ht = pHeap->GetDesc().Properties.Type;
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, d, AllocationKind::Placed,
                               ht, LookupHeapId(pHeap)));
    }
    return hr;
}

// ===========================================================================
// ID3D12Device9
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue1(
    ID3D12Device9* This, const D3D12_COMMAND_QUEUE_DESC* pDesc,
    REFIID CreatorID, REFIID riid, void** ppCommandQueue) {
    auto fn = reinterpret_cast<PFN_CreateCommandQueue1>(
        DeviceOriginals().CreateCommandQueue1);
    HRESULT hr = fn(This, pDesc, CreatorID, riid, ppCommandQueue);
    if (SUCCEEDED(hr))
        Track(reinterpret_cast<IUnknown**>(ppCommandQueue),
              MakeSimpleInfo(ObjectType::CommandQueue));
    return hr;
}

// ===========================================================================
// ID3D12Device10
// ===========================================================================

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(
    ID3D12Device10* This, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riidResource, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateCommittedResource3>(
        DeviceOriginals().CreateCommittedResource3);
    HRESULT hr = fn(This, pHeapProperties, HeapFlags, pDesc, InitialLayout,
                    pOptimizedClearValue, pProtectedSession,
                    NumCastableFormats, pCastableFormats,
                    riidResource, ppvResource);
    if (SUCCEEDED(hr) && pDesc && pHeapProperties) {
        D3D12_RESOURCE_DESC d = ToDesc(*pDesc);
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, d, AllocationKind::Committed,
                               pHeapProperties->Type, 0));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource2(
    ID3D12Device10* This, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreatePlacedResource2>(
        DeviceOriginals().CreatePlacedResource2);
    HRESULT hr = fn(This, pHeap, HeapOffset, pDesc, InitialLayout,
                    pOptimizedClearValue, NumCastableFormats, pCastableFormats,
                    riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc) {
        D3D12_RESOURCE_DESC d = ToDesc(*pDesc);
        D3D12_HEAP_TYPE ht = D3D12_HEAP_TYPE_DEFAULT;
        if (pHeap) ht = pHeap->GetDesc().Properties.Type;
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, d, AllocationKind::Placed,
                               ht, LookupHeapId(pHeap)));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource2(
    ID3D12Device10* This, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_BARRIER_LAYOUT InitialLayout,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats,
    REFIID riid, void** ppvResource) {
    auto fn = reinterpret_cast<PFN_CreateReservedResource2>(
        DeviceOriginals().CreateReservedResource2);
    HRESULT hr = fn(This, pDesc, InitialLayout, pOptimizedClearValue,
                    pProtectedSession, NumCastableFormats, pCastableFormats,
                    riid, ppvResource);
    if (SUCCEEDED(hr) && pDesc)
        Track(reinterpret_cast<IUnknown**>(ppvResource),
              MakeResourceInfo(This, *pDesc, AllocationKind::Reserved,
                               D3D12_HEAP_TYPE_DEFAULT, 0));
    return hr;
}

} // namespace dx12track
