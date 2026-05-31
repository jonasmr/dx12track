#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "EventTypes.h"

namespace dx12track {

struct LiveObject {
    uint64_t       id;
    ObjectType     type;
    AllocationKind alloc;
    uint32_t       heap_type;
    uint32_t       dimension;
    uint32_t       format;
    uint64_t       size_bytes;
    uint64_t       parent_heap_id;
    uint64_t       created_ns;
    std::wstring   name;
};

struct ActivityLine {
    uint64_t     ts_ns;
    bool         created;     // false = destroyed
    ObjectType   type;
    uint64_t     id;
    uint64_t     size_bytes;  // 0 for destroyed (we look up the original size)
    std::wstring name;
    std::wstring heap_label;  // "Committed/Default", "Placed/Upload", "Heap/Default", ""
};

// Indexed by (heap_type bucket, alloc kind bucket); 0 buckets are "everything else".
struct MemoryTotals {
    // Heap types: Default=1, Upload=2, Readback=3, Custom=4, GpuUpload=5 (D3D12).
    // We collapse to 6 buckets [0..5], where 0 = unknown/non-memory.
    static constexpr size_t kHeapBuckets  = 6;
    // Alloc kinds: None=0, Committed=1, Placed=2, Reserved=3, Heap=4
    static constexpr size_t kAllocBuckets = 5;

    uint64_t bytes [kHeapBuckets][kAllocBuckets] = {};
    uint64_t counts[kHeapBuckets][kAllocBuckets] = {};
};

class Model {
public:
    Model();

    // Wire-level event ingestion (called from PipeServer reader thread).
    void OnEvent(const EventHeader& hdr, const void* payload, size_t bytes);

    // Snapshot read for the renderer (called from main thread).
    struct Snapshot {
        uint32_t     pid           = 0;
        std::wstring exe_path;
        uint64_t     dll_start_ns  = 0;
        uint64_t     latest_ns     = 0;
        bool         child_exited  = false;
        uint32_t     child_exit_code = 0;

        size_t       live_count    = 0;
        uint64_t     live_bytes    = 0;
        // Per-object-type live counts, indexed by ObjectType ordinal.
        std::array<size_t, static_cast<size_t>(ObjectType::Count)> per_type_counts{};
        MemoryTotals totals;
        std::deque<ActivityLine> recent;  // newest at the back
    };
    Snapshot GetSnapshot() const;

    void MarkChildExited(uint32_t exit_code);

private:
    static size_t HeapBucket(uint32_t heap_type);
    static size_t AllocBucket(AllocationKind a);

    mutable std::mutex mu_;
    Snapshot s_;
    std::unordered_map<uint64_t, LiveObject> live_;
};

} // namespace dx12track
