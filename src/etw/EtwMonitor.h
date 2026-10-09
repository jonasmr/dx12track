#pragma once
//
// dx12track's wrapper around DxTimingCaptureLibrary (external/DxTimingCaptureLibrary).
//
// This header is deliberately plain: no <d3d12.h>, no Windows headers, no
// library headers. The library includes the Agility SDK's unprefixed
// <d3d12.h> while the rest of dx12track uses vcpkg's <directx/d3d12.h>, so
// everything that touches the library lives in EtwMonitor.cpp (its own static
// library, built as C++20) and talks to the launcher only through the plain
// types below.
//

#include <cstdint>
#include <memory>
#include <string>

namespace dx12track::etw {

// Mirrors DirectX::Etw::ApiObjectType (same numeric values).
enum class ObjType : uint8_t {
    Invalid = 0,
    Device,
    Resource,
    Heap,
    PipelineState,
    StateObject,
    CommandAllocator,
    DescriptorHeap,
    MetaCommand,
    Count
};

const char* ObjTypeName(ObjType t);

// Where an object's backing allocation lives (DirectX::Etw::MemorySegmentGroup).
enum class Group : uint8_t {
    Unknown = 0,
    Local,     // video memory
    NonLocal,  // system memory
};

enum class ResidencyOp : uint8_t { MakeResident = 0, Evict, PageIn, PageOut };

// The per-process memory counters the library reports (MemoryCounterWriter).
enum class CounterKind : uint8_t {
    LocalBudget = 0,
    LocalResident,
    LocalUsage,
    NonLocalBudget,
    NonLocalResident,
    NonLocalUsage,
    DemotedMin,
    DemotedLow,
    DemotedNormal,
    DemotedHigh,
    DemotedMax,
    PagingLocalToNonLocal,
    PagingNonLocalToLocal,
    Count
};

struct GroupChange {
    int64_t  ts;       // absolute QPC-ns, see Sink
    uint64_t lib_id;
    ObjType  type;
    Group    group;
};

// Implemented by the consumer (the launcher's Model). Every method is called
// on the ETW consumer thread (ProcessTrace); guard shared state yourself.
// Timestamps are absolute "QPC nanoseconds": raw QPC ticks * 1e9 / QPC
// frequency, i.e. the library's default timeline. Subtract the same
// conversion of the DLL's Hello.qpc_start to get dx12track ts_ns.
class Sink {
public:
    virtual ~Sink() = default;

    // A D3D12 object of the traced process. `size` is the size the library
    // reported with the creation (GpuVirtualSize: VA range for resources and
    // heaps, driver allocation size for pipeline states / state objects /
    // descriptor heaps / command allocators; often 0). `lib_id` is unique per
    // creation callback, also for rundown re-reports of the same object.
    virtual void OnEtwObjectCreated(int64_t ts, uint64_t lib_id, ObjType type,
                                    uint64_t size, Group group) = 0;
    // Called right after OnEtwObjectCreated: the object's address in the
    // traced process = the interface pointer the app got from Create*.
    virtual void OnEtwObjectAddress(uint64_t lib_id, ObjType type, uint64_t address) = 0;
    virtual void OnEtwObjectDestroyed(int64_t ts, uint64_t lib_id, ObjType type) = 0;
    virtual void OnEtwObjectSize(int64_t ts, uint64_t lib_id, ObjType type, uint64_t size) = 0;
    virtual void OnEtwGroupChanges(const GroupChange* changes, uint32_t count) = 0;
    virtual void OnEtwResidency(int64_t ts, uint64_t lib_id, ObjType type, ResidencyOp op) = 0;
    // Only counters of the traced process are forwarded. `counter_id` is
    // unique per (adapter, counter); values are in MB of 10^6 bytes.
    virtual void OnEtwCounter(int64_t ts, uint64_t counter_id, CounterKind kind,
                              double value_mb) = 0;
    virtual void OnEtwDiagnostic(bool error, int code, const std::wstring& message) = 0;
};

struct Stats {
    uint64_t events           = 0;  // records delivered by ProcessTrace
    uint64_t events_lost      = 0;  // EVENT_TRACE_PROPERTIES::EventsLost
    uint64_t buffers_lost     = 0;  // RealTimeBuffersLost
    uint64_t exceptions       = 0;  // records the library threw on (dropped)
    uint64_t rundowns         = 0;  // capture-state requests issued
    uint64_t rundown_failures = 0;
};

// Owns the real-time ETW session (DxgKrnl + Direct3D12 providers) and the
// library's DxTimingCaptureEventHandler for one traced process.
class Monitor {
public:
    Monitor();
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    // Starts the session, enables the providers (with an initial rundown) and
    // starts the ProcessTrace thread. On failure returns false and puts a
    // human-readable reason into *error.
    bool Start(uint32_t pid, Sink* sink, std::wstring* error);

    // Asks the Direct3D12 provider to re-log every live object (capture
    // state). Pipeline-state sizes are only reported through this. Async:
    // doesn't wait for D3D12 processes to respond.
    bool RequestRundown();

    // Stops the session: ETW delivers the remaining buffered events, then the
    // library's OnDataComplete runs (on the consumer thread) and the thread is
    // joined. Safe to call more than once.
    void Stop();

    bool Running() const;

    // Live stats (events_lost is queried from the session while it runs; after
    // Stop() it holds the final value).
    Stats GetStats() const;

    // Name of the ETW session ("dx12track-etw-<launcher pid>").
    std::wstring SessionName() const;

    // For console-control handlers: stops this process's session by name
    // without touching any other state.
    static void EmergencyStop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dx12track::etw
