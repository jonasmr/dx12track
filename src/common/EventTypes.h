#pragma once
//
// Wire format shared between dx12track.dll (producer) and dx12track.exe (consumer).
// Sent over the per-process named pipe \\.\pipe\dx12track-<token>.
// All structures are POD with fixed sizes so we can write/read raw bytes.
//

#include <cstdint>

// Pull in D3D12_HEAP_TYPE / D3D12_RESOURCE_DIMENSION / DXGI_FORMAT enums.
// Both sides include this header; both have the DirectX-Headers (Agility SDK)
// supplied by vcpkg on the include path.
#include <directx/d3d12.h>

namespace dx12track {

constexpr uint32_t kProtocolMagic       = 0x44583132; // 'DX12'
constexpr uint32_t kProtocolVersion     = 3;
constexpr size_t   kMaxNameChars        = 256;
constexpr size_t   kMaxCallstackFrames  = 32;
constexpr size_t   kMaxDiagnosticChars  = 512;

enum class ObjectType : uint8_t {
    Unknown = 0,
    Device,
    Resource,
    Heap,
    DescriptorHeap,
    CommandQueue,
    CommandAllocator,
    CommandList,
    PipelineState,
    RootSignature,
    Fence,
    QueryHeap,
    CommandSignature,
    Count
};

enum class AllocationKind : uint8_t {
    None = 0,        // non-memory-bearing
    Committed,       // CreateCommittedResource* — implicit heap
    Placed,          // CreatePlacedResource* — into an explicit heap
    Reserved,        // CreateReservedResource* — virtual; physical mapped via UpdateTileMappings
    Heap             // CreateHeap*  — the heap object itself
};

enum class EventKind : uint8_t {
    Hello = 1,       // sent by DLL on connect; carries pid + start time
    Created,
    Renamed,
    Destroyed,
    Goodbye,         // sent by DLL on DLL_PROCESS_DETACH
    ModuleLoaded,    // initial enumeration + LdrRegisterDllNotification
    ModuleUnloaded,
    Diagnostic,      // verbose-mode trace of injection / hook install / per-fire markers
    ResidencyPriority,  // ID3D12Device1::SetResidencyPriority — one event per object
};

#pragma pack(push, 1)

struct EventHeader {
    uint32_t  magic;          // kProtocolMagic
    uint32_t  payload_bytes;  // size of payload that follows this header
    uint64_t  ts_ns;          // monotonic ns since process start
    EventKind kind;
    uint8_t   _pad[7];
};

struct HelloPayload {
    uint32_t protocol_version;
    uint32_t pid;
    uint64_t qpc_frequency;
    wchar_t  exe_path[kMaxNameChars];
};

struct CreatedPayload {
    uint64_t id;
    uint64_t size_bytes;
    uint64_t parent_heap_id;   // tracker id of the parent heap, 0 if not Placed
    uint64_t parent_heap_ptr;  // raw IUnknown* of the parent heap, 0 if not Placed
    uint32_t heap_type;        // D3D12_HEAP_TYPE (0 if alloc==None)
    uint32_t dimension;        // D3D12_RESOURCE_DIMENSION (Buffer/Texture*/Unknown)
    uint32_t format;           // DXGI_FORMAT (UNKNOWN for non-resources)
    ObjectType     type;
    AllocationKind alloc;
    uint8_t  frame_count;      // 0 when callstacks aren't enabled
    uint8_t  _pad;
    wchar_t  name[kMaxNameChars];   // empty until SetName fires
    uint64_t frames[kMaxCallstackFrames];  // top of stack first; rest is garbage
};

struct RenamedPayload {
    uint64_t id;
    wchar_t  name[kMaxNameChars];
};

struct DestroyedPayload {
    uint64_t id;
};

struct GoodbyePayload {
    uint32_t exit_code;
};

// Enough metadata for an offline symbol resolver to locate matching binaries
// and PDBs (e.g., via a symbol server using the image hash or PDB hash).
struct ModuleLoadedPayload {
    uint64_t base;             // load address in the target process
    uint64_t size;             // image size in bytes (SizeOfImage)
    uint32_t timestamp;        // IMAGE_FILE_HEADER.TimeDateStamp (for image hash)
    uint32_t pdb_age;          // CV_INFO_PDB70.Age (for PDB hash)
    uint8_t  pdb_guid[16];     // CV_INFO_PDB70.Signature (PDB GUID), all zero if absent
    wchar_t  name[kMaxNameChars];      // full module path
    wchar_t  pdb_name[kMaxNameChars];  // PDB path as recorded in the PE debug dir
};

struct ModuleUnloadedPayload {
    uint64_t base;
};

// ASCII message buffer — diagnostics carry hex addresses, MH_* result codes,
// and short English strings. 512 bytes is plenty for what we emit and keeps
// the wire format fixed-size like everything else.
struct DiagnosticPayload {
    char message[kMaxDiagnosticChars];
};

// ID3D12Device1::SetResidencyPriority takes parallel arrays of (object, prio).
// We emit one ResidencyPriority event per object so the wire format stays
// fixed-size. `id` is 0 if the object's vtable wasn't registered with us
// (untracked pageable — still log the priority change for completeness).
struct ResidencyPriorityPayload {
    uint64_t id;
    uint64_t object_ptr;      // raw IUnknown* of the pageable for cross-ref
    uint32_t priority;        // D3D12_RESIDENCY_PRIORITY raw value
    uint8_t  _pad[4];
};

#pragma pack(pop)

inline const char* ResidencyPriorityName(uint32_t p) {
    switch (p) {
        case 0x28000000u: return "Minimum";
        case 0x50000000u: return "Low";
        case 0x78000000u: return "Normal";
        case 0xa0010000u: return "High";
        case 0xc8000000u: return "Maximum";
        default:          return "Custom";
    }
}

inline const char* ObjectTypeName(ObjectType t) {
    switch (t) {
        case ObjectType::Device:           return "Device";
        case ObjectType::Resource:         return "Resource";
        case ObjectType::Heap:             return "Heap";
        case ObjectType::DescriptorHeap:   return "DescriptorHeap";
        case ObjectType::CommandQueue:     return "CommandQueue";
        case ObjectType::CommandAllocator: return "CommandAllocator";
        case ObjectType::CommandList:      return "CommandList";
        case ObjectType::PipelineState:    return "PipelineState";
        case ObjectType::RootSignature:    return "RootSignature";
        case ObjectType::Fence:            return "Fence";
        case ObjectType::QueryHeap:        return "QueryHeap";
        case ObjectType::CommandSignature: return "CommandSignature";
        default:                           return "Unknown";
    }
}

inline const char* AllocationKindName(AllocationKind k) {
    switch (k) {
        case AllocationKind::Committed: return "Committed";
        case AllocationKind::Placed:    return "Placed";
        case AllocationKind::Reserved:  return "Reserved";
        case AllocationKind::Heap:      return "Heap";
        default:                        return "None";
    }
}

inline const char* HeapTypeName(uint32_t t) {
    switch (t) {
        case D3D12_HEAP_TYPE_DEFAULT:    return "Default";
        case D3D12_HEAP_TYPE_UPLOAD:     return "Upload";
        case D3D12_HEAP_TYPE_READBACK:   return "Readback";
        case D3D12_HEAP_TYPE_CUSTOM:     return "Custom";
        case D3D12_HEAP_TYPE_GPU_UPLOAD: return "GpuUpload";
        default:                         return "None";
    }
}

inline const char* DimensionName(uint32_t d) {
    switch (d) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:    return "Buffer";
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return "Tex1D";
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D: return "Tex2D";
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return "Tex3D";
        default:                                 return "Unknown";
    }
}

} // namespace dx12track
