// The only dx12track translation unit that includes DxTimingCaptureLibrary
// headers (and with them the Agility SDK's unprefixed <d3d12.h>). It must not
// include EventTypes.h or anything else that pulls in vcpkg's
// <directx/d3d12.h>; it talks to the launcher only through EtwMonitor.h.
//
// ETW session setup follows the library's README / memorymap sample (and the
// dxtcl_monitor experiment in external/DxTimingCaptureLibrary_Test): a
// real-time session in raw-QPC mode (Wnode.ClientContext = 1 +
// PROCESS_TRACE_MODE_RAW_TIMESTAMP), DxgKrnl + Direct3D12 providers, an
// initial rundown, and ProcessTrace on its own thread.

#include "EtwMonitor.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/EtwProviders.h>
#include <DxTimingCaptureLibrary/NoOpCallbacks.h>

namespace dx12track::etw {

using namespace DirectX::Etw;

const char* ObjTypeName(ObjType t) {
    switch (t) {
        case ObjType::Device:           return "Device";
        case ObjType::Resource:         return "Resource";
        case ObjType::Heap:             return "Heap";
        case ObjType::PipelineState:    return "PipelineState";
        case ObjType::StateObject:      return "StateObject";
        case ObjType::CommandAllocator: return "CommandAllocator";
        case ObjType::DescriptorHeap:   return "DescriptorHeap";
        case ObjType::MetaCommand:      return "MetaCommand";
        default:                        return "Invalid";
    }
}

namespace {

constexpr wchar_t kSessionPrefix[] = L"dx12track-etw-";

std::wstring OwnSessionName() {
    return std::wstring(kSessionPrefix) + std::to_wstring(GetCurrentProcessId());
}

ObjType ToObjType(ApiObjectType t) {
    const int v = static_cast<int>(t);
    if (v <= 0 || v >= static_cast<int>(ObjType::Count)) return ObjType::Invalid;
    return static_cast<ObjType>(v);
}

Group ToGroup(MemorySegmentGroup g) {
    switch (g) {
        case MemorySegmentGroup::Local:    return Group::Local;
        case MemorySegmentGroup::NonLocal: return Group::NonLocal;
        default:                           return Group::Unknown;
    }
}

ResidencyOp ToResidencyOp(ResidencyOperationType t) {
    switch (t) {
        case ResidencyOperationType::Evict:   return ResidencyOp::Evict;
        case ResidencyOperationType::PageIn:  return ResidencyOp::PageIn;
        case ResidencyOperationType::PageOut: return ResidencyOp::PageOut;
        default:                              return ResidencyOp::MakeResident;
    }
}

// Counter names as MemoryCounterWriter reports them.
bool CounterKindFromName(const wchar_t* name, CounterKind* out) {
    static const struct { const wchar_t* name; CounterKind kind; } kMap[] = {
        { L"Local Budget",              CounterKind::LocalBudget },
        { L"Local Resident",            CounterKind::LocalResident },
        { L"Local Usage",               CounterKind::LocalUsage },
        { L"Non-Local Budget",          CounterKind::NonLocalBudget },
        { L"Non-Local Resident",        CounterKind::NonLocalResident },
        { L"Non-Local Usage",           CounterKind::NonLocalUsage },
        { L"Minimum Priority",          CounterKind::DemotedMin },
        { L"Low Priority",              CounterKind::DemotedLow },
        { L"Normal Priority",           CounterKind::DemotedNormal },
        { L"High Priority",             CounterKind::DemotedHigh },
        { L"Maximum Priority",          CounterKind::DemotedMax },
        { L"Local to Non-Local Paging", CounterKind::PagingLocalToNonLocal },
        { L"Non-Local to Local Paging", CounterKind::PagingNonLocalToLocal },
    };
    if (!name) return false;
    for (const auto& e : kMap) {
        if (wcscmp(e.name, name) == 0) { *out = e.kind; return true; }
    }
    return false;
}

// EVENT_TRACE_PROPERTIES followed by room for the logger name (and, for
// QueryAllTraces, the log file name).
struct SessionProperties {
    EVENT_TRACE_PROPERTIES properties;
    wchar_t logger_name[256];
    wchar_t log_file_name[256];

    SessionProperties() {
        ZeroMemory(this, sizeof(*this));
        properties.Wnode.BufferSize    = sizeof(*this);
        properties.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        properties.Wnode.ClientContext = 1;    // QPC timestamps (required by the library)
        properties.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        properties.BufferSize          = 1024; // KB; as in the library's memorymap sample
        properties.MinimumBuffers      = 64;
        properties.MaximumBuffers      = 1290;
        properties.FlushTimer          = 1;    // deliver at least once per second
        properties.LoggerNameOffset    = offsetof(SessionProperties, logger_name);
        properties.LogFileNameOffset   = 0;    // real-time only, no .etl
    }
};

// For ControlTrace (query/stop) and QueryAllTraces, which want room for both
// names.
struct ControlProperties : SessionProperties {
    ControlProperties() {
        properties.LogFileNameOffset = offsetof(SessionProperties, log_file_name);
    }
};

bool ProcessIsAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;  // exists, just not ours
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// Real-time sessions outlive their controller. Stop the ones a previous
// (killed) dx12track left behind, so they don't pile up against the system's
// session limit. Sessions of still-running dx12track instances are left alone.
void StopStaleSessions() {
    constexpr ULONG kMax = 128;
    std::vector<ControlProperties> storage(kMax);
    std::vector<EVENT_TRACE_PROPERTIES*> ptrs(kMax);
    for (ULONG i = 0; i < kMax; ++i) ptrs[i] = &storage[i].properties;
    ULONG count = 0;
    if (QueryAllTracesW(ptrs.data(), kMax, &count) != ERROR_SUCCESS) return;

    const size_t prefix_len = wcslen(kSessionPrefix);
    for (ULONG i = 0; i < count && i < kMax; ++i) {
        const wchar_t* name = storage[i].logger_name;
        if (wcsncmp(name, kSessionPrefix, prefix_len) != 0) continue;
        const DWORD pid = wcstoul(name + prefix_len, nullptr, 10);
        if (pid == GetCurrentProcessId() || (pid != 0 && ProcessIsAlive(pid)))
            continue;
        ControlProperties stop;
        ControlTraceW(0, name, &stop.properties, EVENT_TRACE_CONTROL_STOP);
    }
}

// The library's callbacks -> Sink. Runs on the ProcessTrace thread only, so
// the counter-id map needs no lock. Ids come from the NoOp base classes
// (unique, non-zero, monotonically increasing).
class Callbacks final : public NoOpApiObjectCallbacks,
                        public NoOpPixCounterCallbacks,
                        public NoOpResidencyEventCallbacks,
                        public DiagnosticsSink {
public:
    Callbacks(DWORD pid, Sink* sink) : pid_(pid), sink_(sink) {}

    // ---- ApiObjectCallbacks ----------------------------------------------

    HRESULT OnDeviceCreation(INT64 ts, const DeviceInfo* device, UINT64* id) override {
        NoOpApiObjectCallbacks::OnDeviceCreation(ts, device, id);
        sink_->OnEtwObjectCreated(ts, *id, ObjType::Device, 0, Group::Unknown);
        return S_OK;
    }
    HRESULT OnDescriptorHeapCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                     const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                     const ObjectPlacementInfo* pl, UINT64* id) override {
        NoOpApiObjectCallbacks::OnDescriptorHeapCreation(ts, dev, pid, tid, desc, pl, id);
        Created(ts, *id, ObjType::DescriptorHeap, pl);
        return S_OK;
    }
    HRESULT OnCommittedResourceCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                        const D3D12_RESOURCE_DESC* desc,
                                        const ObjectPlacementInfo* pl,
                                        const D3D12_HEAP_PROPERTIES* hp,
                                        D3D12_HEAP_FLAGS hf, UINT64* id) override {
        NoOpApiObjectCallbacks::OnCommittedResourceCreation(ts, dev, pid, tid, desc, pl, hp, hf, id);
        Created(ts, *id, ObjType::Resource, pl);
        return S_OK;
    }
    HRESULT OnPlacedResourceCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                     const D3D12_RESOURCE_DESC* desc,
                                     const ObjectPlacementInfo* pl, UINT64* id) override {
        NoOpApiObjectCallbacks::OnPlacedResourceCreation(ts, dev, pid, tid, desc, pl, id);
        Created(ts, *id, ObjType::Resource, pl);
        return S_OK;
    }
    HRESULT OnReservedResourceCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                       const D3D12_RESOURCE_DESC* desc,
                                       const ReservedResourceInfo* r, UINT64* id) override {
        NoOpApiObjectCallbacks::OnReservedResourceCreation(ts, dev, pid, tid, desc, r, id);
        sink_->OnEtwObjectCreated(ts, *id, ObjType::Resource, 0, Group::Unknown);
        return S_OK;
    }
    HRESULT OnHeapCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                           const D3D12_HEAP_DESC* desc, const ObjectPlacementInfo* pl,
                           UINT64* id) override {
        NoOpApiObjectCallbacks::OnHeapCreation(ts, dev, pid, tid, desc, pl, id);
        Created(ts, *id, ObjType::Heap, pl);
        return S_OK;
    }
    HRESULT OnPipelineStateCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                    const ObjectPlacementInfo* pl, UINT64* id) override {
        NoOpApiObjectCallbacks::OnPipelineStateCreation(ts, dev, pid, tid, pl, id);
        Created(ts, *id, ObjType::PipelineState, pl);
        return S_OK;
    }
    HRESULT OnStateObjectCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                  const ObjectPlacementInfo* pl, UINT64* id) override {
        NoOpApiObjectCallbacks::OnStateObjectCreation(ts, dev, pid, tid, pl, id);
        Created(ts, *id, ObjType::StateObject, pl);
        return S_OK;
    }
    HRESULT OnCommandAllocatorCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                       D3D12_COMMAND_LIST_TYPE type,
                                       const ObjectPlacementInfo* pl, UINT64* id) override {
        NoOpApiObjectCallbacks::OnCommandAllocatorCreation(ts, dev, pid, tid, type, pl, id);
        Created(ts, *id, ObjType::CommandAllocator, pl);
        return S_OK;
    }
    HRESULT OnMetaCommandCreation(INT64 ts, UINT64 dev, UINT32 pid, UINT32 tid,
                                  REFGUID cmd, const ObjectPlacementInfo* pl,
                                  UINT64* id) override {
        NoOpApiObjectCallbacks::OnMetaCommandCreation(ts, dev, pid, tid, cmd, pl, id);
        Created(ts, *id, ObjType::MetaCommand, pl);
        return S_OK;
    }
    HRESULT OnObjectDestruction(INT64 ts, ApiObjectType type, UINT64 id) override {
        sink_->OnEtwObjectDestroyed(ts, id, ToObjType(type));
        return S_OK;
    }
    HRESULT OnApiObjectName(INT64, ApiObjectType, UINT64, std::wstring_view) override {
        return S_OK;  // dx12track gets names from its own SetName hooks
    }
    HRESULT OnApiObjectSizeAndAddress(INT64 ts, ApiObjectType type, UINT64 id,
                                      UINT64 size, UINT64 /*gpu_base*/) override {
        sink_->OnEtwObjectSize(ts, id, ToObjType(type), size);
        return S_OK;
    }
    HRESULT OnApiObjectAddress(ApiObjectType type, UINT64 id, UINT64 address) override {
        sink_->OnEtwObjectAddress(id, ToObjType(type), address);
        return S_OK;
    }

    // ---- ResidencyEventCallbacks ------------------------------------------

    HRESULT OnResidencyOperation(const ResidencyOperation* op) override {
        if (op)
            sink_->OnEtwResidency(op->Timestamp, op->ObjectId, ToObjType(op->ObjectType),
                                  ToResidencyOp(op->OperationType));
        return S_OK;
    }
    HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* data,
                                            UINT32 count) override {
        if (!data || !count) return S_OK;
        changes_.resize(count);
        for (UINT32 i = 0; i < count; ++i) {
            changes_[i].ts     = data[i].Timestamp;
            changes_[i].lib_id = data[i].ObjectId;
            changes_[i].type   = ToObjType(data[i].ObjectType);
            changes_[i].group  = ToGroup(data[i].SegmentGroup);
        }
        sink_->OnEtwGroupChanges(changes_.data(), count);
        return S_OK;
    }
    // OnDemotedAllocations / OnAllocationMigrations: deliberately unused
    // (undocumented semantics, unreliable in practice) - NoOp base.

    // ---- PixCounterCallbacks ----------------------------------------------

    HRESULT OnPixCounterInfo(UINT64 group, UINT32 pid, PCWSTR name, PCWSTR desc,
                             PCWSTR units, CounterFlags flags, double mn, double mx,
                             UINT64* id) override {
        NoOpPixCounterCallbacks::OnPixCounterInfo(group, pid, name, desc, units, flags, mn, mx, id);
        NoteCounter(pid, name, *id);
        return S_OK;
    }
    HRESULT OnPixCounterInfoWithDefinition(UINT64 group, UINT32 pid, PCWSTR name,
                                           PCWSTR desc, PCWSTR def, PCWSTR units,
                                           CounterFlags flags, double mn, double mx,
                                           UINT64* id) override {
        NoOpPixCounterCallbacks::OnPixCounterInfoWithDefinition(group, pid, name, desc, def,
                                                                units, flags, mn, mx, id);
        NoteCounter(pid, name, *id);
        return S_OK;
    }
    HRESULT OnPixCounterData(const CounterDataPoint* points, UINT32 count) override {
        for (UINT32 i = 0; points && i < count; ++i) {
            auto it = counters_.find(points[i].CounterId);
            if (it != counters_.end())
                sink_->OnEtwCounter(points[i].Timestamp, points[i].CounterId, it->second,
                                    points[i].Value);
        }
        return S_OK;
    }

    // ---- DiagnosticsSink --------------------------------------------------

    void OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code,
                      std::wstring_view message) override {
        sink_->OnEtwDiagnostic(severity == DiagnosticSeverity::Error,
                               static_cast<int>(code), std::wstring(message));
    }

private:
    void Created(INT64 ts, UINT64 id, ObjType type, const ObjectPlacementInfo* pl) {
        sink_->OnEtwObjectCreated(ts, id, type, pl ? pl->GpuVirtualSize : 0,
                                  pl ? ToGroup(pl->ResidentSegmentGroup) : Group::Unknown);
    }
    void NoteCounter(UINT32 pid, PCWSTR name, UINT64 id) {
        CounterKind kind;
        if (pid == pid_ && CounterKindFromName(name, &kind))
            counters_[id] = kind;
    }

    DWORD pid_;
    Sink* sink_;
    std::unordered_map<UINT64, CounterKind> counters_;  // counters of the traced process
    std::vector<GroupChange> changes_;
};

} // namespace

struct Monitor::Impl {
    std::wstring session_name;
    std::unique_ptr<Callbacks> callbacks;
    std::unique_ptr<DxTimingCaptureEventHandler> handler;
    TRACEHANDLE session = 0;
    TRACEHANDLE trace   = INVALID_PROCESSTRACE_HANDLE;
    EVENT_TRACE_LOGFILEW logfile{};
    std::thread consumer;

    std::atomic<uint64_t> events{0};
    std::atomic<uint64_t> exceptions{0};
    std::atomic<uint64_t> rundowns{0};
    std::atomic<uint64_t> rundown_failures{0};
    uint64_t final_events_lost  = 0;
    uint64_t final_buffers_lost = 0;

    static void WINAPI OnEvent(EVENT_RECORD* record) {
        auto* self = static_cast<Impl*>(record->UserContext);
        ++self->events;
        try {
            self->handler->HandleEventRecord(record);
        } catch (...) {  // must never unwind into ETW
            ++self->exceptions;
        }
    }

    static ULONG WINAPI OnBuffer(EVENT_TRACE_LOGFILEW* lf) {
        auto* self = static_cast<Impl*>(lf->Context);
        try {
            self->handler->ReportTraceStatistics(*lf);  // lost events -> DiagnosticsSink
        } catch (...) {
            ++self->exceptions;
        }
        return TRUE;
    }
};

namespace {

const ULONGLONG kDxgkKeywords =
    DXGK_KEYWORD_LOG_FLAGS_BASE | DXGK_KEYWORD_LOG_FLAGS_RESOURCE |
    DXGK_KEYWORD_LOG_FLAGS_ALLOCATIONS_REFERENCES | DXGK_KEYWORD_LOG_FLAGS_LONG_HAUL;
const ULONGLONG kD3D12Keywords =
    D3D12_ETW_LOG_FLAGS_NAMES | D3D12_ETW_LOG_FLAGS_DEVICES |
    D3D12_ETW_LOG_FLAGS_OBJECT_LIFETIME | D3D12_ETW_LOG_FLAGS_RESOURCES | D3D12_ETW_LOG_APIS;

std::wstring EnableError(const wchar_t* provider, ULONG status) {
    wchar_t buf[512];
    if (status == ERROR_TIMEOUT) {
        swprintf(buf, 512,
            L"EnableTraceEx2(%ls) timed out (error 1460): a process using that provider "
            L"did not answer in time. Retry, or close other D3D12 apps (browsers, "
            L"streaming/overlay tools) and retry.", provider);
    } else if (status == ERROR_ACCESS_DENIED) {
        swprintf(buf, 512,
            L"EnableTraceEx2(%ls): access denied (error 5). Run as administrator or add "
            L"your account to the 'Performance Log Users' group (then sign out and in).",
            provider);
    } else {
        swprintf(buf, 512, L"EnableTraceEx2(%ls) failed: error %lu", provider, status);
    }
    return buf;
}

// Enable + initial rundown. One retry on ERROR_TIMEOUT (some other D3D12
// process was slow to answer the enable callback).
ULONG EnableProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords,
                     UCHAR level) {
    ENABLE_TRACE_PARAMETERS params = { ENABLE_TRACE_PARAMETERS_VERSION_2 };
    ULONG status = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                  level, keywords, 0, 5000, &params);
    if (status == ERROR_TIMEOUT)
        status = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                level, keywords, 0, 10000, &params);
    if (status != ERROR_SUCCESS) return status;

    // Rundown: have the provider log the state that already exists (adapters,
    // segments, ...). Failure here is not fatal.
    EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_CAPTURE_STATE,
                   TRACE_LEVEL_VERBOSE, 0, 0, 10000, nullptr);
    return ERROR_SUCCESS;
}

} // namespace

Monitor::Monitor() : impl_(std::make_unique<Impl>()) {
    impl_->session_name = OwnSessionName();
}

Monitor::~Monitor() { Stop(); }

std::wstring Monitor::SessionName() const { return impl_->session_name; }

bool Monitor::Running() const { return impl_->session != 0; }

bool Monitor::Start(uint32_t pid, Sink* sink, std::wstring* error) {
    Impl& d = *impl_;
    if (d.session) return true;

    d.callbacks = std::make_unique<Callbacks>(pid, sink);
    DxTimingCaptureLibraryOptions options{};
    options.TrackApiObjects = true;
    DxTimingCaptureEventCallbacks cb{};
    cb.ApiObjectCallbacks      = d.callbacks.get();
    cb.PixCounterCallbacks     = d.callbacks.get();
    cb.ResidencyEventCallbacks = d.callbacks.get();
    cb.DiagnosticsSink         = d.callbacks.get();
    try {
        d.handler = DxTimingCaptureEventHandler::Create(pid, options, cb);
    } catch (...) {
        if (error) *error = L"DxTimingCaptureEventHandler::Create threw";
        return false;
    }

    StopStaleSessions();
    {
        ControlProperties stale;  // our own name from an earlier, recycled pid
        ControlTraceW(0, d.session_name.c_str(), &stale.properties, EVENT_TRACE_CONTROL_STOP);
    }

    SessionProperties props;
    ULONG status = StartTraceW(&d.session, d.session_name.c_str(), &props.properties);
    if (status != ERROR_SUCCESS) {
        d.session = 0;
        if (error) {
            wchar_t buf[512];
            if (status == ERROR_ACCESS_DENIED)
                swprintf(buf, 512,
                    L"StartTrace: access denied (error 5). Run as administrator, or add your "
                    L"account to the 'Performance Log Users' group (then sign out and in).");
            else if (status == ERROR_NO_SYSTEM_RESOURCES)
                swprintf(buf, 512,
                    L"StartTrace failed: too many ETW sessions (error 1450). Stop unused "
                    L"ones (logman query -ets).");
            else
                swprintf(buf, 512, L"StartTrace failed: error %lu", status);
            *error = buf;
        }
        return false;
    }

    status = EnableProvider(d.session, DxgkControlGuid, kDxgkKeywords, TRACE_LEVEL_VERBOSE);
    if (status != ERROR_SUCCESS) {
        if (error) *error = EnableError(L"DxgKrnl", status);
        Stop();
        return false;
    }
    status = EnableProvider(d.session, Direct3D12EtwProviderGuid, kD3D12Keywords,
                            TRACE_LEVEL_RESERVED6);
    if (status != ERROR_SUCCESS) {
        if (error) *error = EnableError(L"Direct3D12", status);
        Stop();
        return false;
    }

    d.logfile = {};
    d.logfile.LoggerName = const_cast<LPWSTR>(d.session_name.c_str());
    d.logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME |
                                 PROCESS_TRACE_MODE_EVENT_RECORD |
                                 PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    d.logfile.EventRecordCallback = &Impl::OnEvent;
    d.logfile.BufferCallback      = &Impl::OnBuffer;
    d.logfile.Context             = &d;
    d.trace = OpenTraceW(&d.logfile);
    if (d.trace == INVALID_PROCESSTRACE_HANDLE) {
        if (error) *error = L"OpenTrace failed: error " + std::to_wstring(GetLastError());
        Stop();
        return false;
    }

    d.consumer = std::thread([&d] {
        ProcessTrace(&d.trace, 1, nullptr, nullptr);  // returns once the session stops
        try {
            d.handler->OnDataComplete();
        } catch (...) {
            ++d.exceptions;
        }
    });
    return true;
}

bool Monitor::RequestRundown() {
    Impl& d = *impl_;
    if (!d.session) return false;
    // Timeout 0: don't block the caller on slow D3D12 processes.
    ULONG status = EnableTraceEx2(d.session, &Direct3D12EtwProviderGuid,
                                  EVENT_CONTROL_CODE_CAPTURE_STATE, TRACE_LEVEL_VERBOSE,
                                  0, 0, 0, nullptr);
    if (status == ERROR_SUCCESS) { ++d.rundowns; return true; }
    ++d.rundown_failures;
    return false;
}

void Monitor::Stop() {
    Impl& d = *impl_;
    if (d.session) {
        ControlProperties props;  // ControlTrace fills in the final statistics
        if (ControlTraceW(d.session, nullptr, &props.properties,
                          EVENT_TRACE_CONTROL_STOP) == ERROR_SUCCESS) {
            d.final_events_lost  = props.properties.EventsLost;
            d.final_buffers_lost = props.properties.RealTimeBuffersLost;
        }
        d.session = 0;
    }
    if (d.consumer.joinable()) d.consumer.join();
    if (d.trace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(d.trace);
        d.trace = INVALID_PROCESSTRACE_HANDLE;
    }
}

Stats Monitor::GetStats() const {
    const Impl& d = *impl_;
    Stats s;
    s.events           = d.events.load();
    s.exceptions       = d.exceptions.load();
    s.rundowns         = d.rundowns.load();
    s.rundown_failures = d.rundown_failures.load();
    s.events_lost      = d.final_events_lost;
    s.buffers_lost     = d.final_buffers_lost;
    if (d.session) {
        ControlProperties props;
        if (ControlTraceW(d.session, nullptr, &props.properties,
                          EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS) {
            s.events_lost  = props.properties.EventsLost;
            s.buffers_lost = props.properties.RealTimeBuffersLost;
        }
    }
    return s;
}

void Monitor::EmergencyStop() {
    ControlProperties props;
    ControlTraceW(0, OwnSessionName().c_str(), &props.properties, EVENT_TRACE_CONTROL_STOP);
}

} // namespace dx12track::etw
