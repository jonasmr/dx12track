#pragma once

#include <array>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EventTypes.h"
#include "EtwMonitor.h"

namespace dx12track {

// Where an object's memory lives, as far as ETW (--etw) knows.
enum class MemLoc : uint8_t { Unknown = 0, Vram, Sys, Count };

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
    uint64_t       object_ptr;      // app-visible interface pointer (protocol v4)
    std::wstring   name;

    // --etw join state (unused without --etw).
    uint64_t prev_destroy_ns = 0;   // ts_ns the previous object at object_ptr died (0 = none seen)
    uint64_t etw_key         = 0;   // canonical library id of the bound ETW object, 0 = unbound
    MemLoc   own_loc         = MemLoc::Unknown;  // last group of the bound ETW object
    MemLoc   eff_loc         = MemLoc::Unknown;  // own_loc, or the parent heap's (placed resources)
    bool     loc_reported    = false;            // a sidecar `location` was written at least once
    uint64_t driver_size     = 0;   // ETW-reported driver allocation (PSO / descriptor heap / cmd allocator)
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

// Everything --etw adds to the snapshot. Fixed-size; cheap to copy.
struct EtwSnapshot {
    static constexpr size_t kTypes    = static_cast<size_t>(etw::ObjType::Count);
    static constexpr size_t kCounters = static_cast<size_t>(etw::CounterKind::Count);
    static constexpr size_t kLocs     = static_cast<size_t>(MemLoc::Count);

    bool         enabled = false;   // --etw given
    bool         running = false;   // session up
    std::wstring status;            // why it isn't running

    // Per-process memory counters (latest value, bytes, summed over adapters).
    uint64_t counter[kCounters] = {};
    bool     counter_seen[kCounters] = {};

    // Same rows as MemoryTotals: bytes of every memory-bearing live object
    // (Heap-obj + Committed + Placed + Reserved) by effective location.
    uint64_t loc_bytes[MemoryTotals::kHeapBuckets][kLocs] = {};

    // ETW-side live objects (rundown duplicates merged) by library type.
    uint64_t live[kTypes]  = {};
    uint64_t sized[kTypes] = {};    // ... with a non-zero driver size
    uint64_t bytes[kTypes] = {};    // ... sum of driver sizes (driver types only)

    // Join diagnostics (refreshed once per second).
    uint64_t bound   = 0;           // ETW objects bound to a tracked live object
    uint64_t unbound = 0;           // alive, not bound (incl. pending)
    uint64_t pending = 0;           // waiting for their pipe object (< timeout)
    uint64_t unbound_by_type[kTypes] = {};
    uint64_t binds = 0, rundown_duplicates = 0, pending_expired = 0, created = 0;
    uint64_t late_matches = 0;      // ETW objects matched to an already-destroyed tracked object

    uint64_t group_changes = 0, make_resident = 0, evict = 0, page_ins = 0, page_outs = 0;
    uint64_t post_exit_ignored = 0;  // group changes / residency ops after the child exited
    uint64_t diagnostics = 0;
    etw::Stats session;             // events / lost / exceptions / rundowns
};

class Model : public etw::Sink {
public:
    Model();
    ~Model() override;

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
        EtwSnapshot  etw;
    };
    Snapshot GetSnapshot() const;

    void MarkChildExited(uint32_t exit_code);

    // --etw: the child's exit time as a raw QPC value. ETW segment-group
    // changes and residency operations stamped after it are process-teardown
    // noise (the kernel tears down allocations the app never released) and
    // are ignored.
    void SetChildExitQpc(uint64_t exit_qpc);

    // ---- --etw ------------------------------------------------------------
    // Call before starting the ETW session. Opens the sidecar JSONL (written
    // from here on, under the model lock). Returns false if it can't be opened
    // (ETW tracking still works, just without the sidecar).
    bool EnableEtw(const std::wstring& sidecar_path, const std::wstring& session_name,
                   const std::wstring& main_log_path, uint32_t rundown_interval_ms);
    // The session couldn't start: drop the sidecar, remember why.
    void DisableEtw(const std::wstring& reason);
    void SetEtwRunning(bool running);
    // ~10 Hz from the main thread: pending-bind timeouts, once-per-second
    // `counters` line and join statistics, sidecar flush.
    void EtwTick(const etw::Stats& session_stats);
    // After the session stopped: final `etw_stats` line, close the sidecar.
    void FinishEtw(const etw::Stats& session_stats);

    // etw::Sink (ETW consumer thread).
    void OnEtwObjectCreated(int64_t ts, uint64_t lib_id, etw::ObjType type,
                            uint64_t size, etw::Group group) override;
    void OnEtwObjectAddress(uint64_t lib_id, etw::ObjType type, uint64_t address) override;
    void OnEtwObjectDestroyed(int64_t ts, uint64_t lib_id, etw::ObjType type) override;
    void OnEtwObjectSize(int64_t ts, uint64_t lib_id, etw::ObjType type, uint64_t size) override;
    void OnEtwGroupChanges(const etw::GroupChange* changes, uint32_t count) override;
    void OnEtwResidency(int64_t ts, uint64_t lib_id, etw::ObjType type,
                        etw::ResidencyOp op) override;
    void OnEtwCounter(int64_t ts, uint64_t counter_id, etw::CounterKind kind,
                      double value_mb) override;
    void OnEtwDiagnostic(bool error, int code, const std::wstring& message) override;

private:
    static size_t HeapBucket(uint32_t heap_type);
    static size_t AllocBucket(AllocationKind a);

    // ETW side of the join: one record per distinct library object. Rundown
    // re-reports (new library id, same address) are folded in as aliases.
    struct EtwObj {
        etw::ObjType type       = etw::ObjType::Invalid;
        uint64_t     address    = 0;
        uint64_t     size       = 0;
        MemLoc       group      = MemLoc::Unknown;
        uint64_t     created_ts = 0;   // ts_ns
        uint64_t     bound_id   = 0;   // live object id
        uint64_t     orphan_ts  = 0;   // != 0: its tracked object died at this ts_ns; never rebinds
        uint64_t     pending_since_ms = 0;
        bool         expired    = false;
        std::vector<uint64_t> aliases;
    };

    // All of these expect mu_ held.
    uint64_t ConvTs(int64_t abs_qpc_ns) const;
    uint64_t NowTsLocked() const;
    MemLoc   EffLoc(const LiveObject& o, bool* via_heap) const;
    void     AddLoc(const LiveObject& o, int sign);
    void     RecomputeLoc(LiveObject& o, uint64_t ts, bool force_report);
    void     RecomputeChildren(uint64_t heap_id, uint64_t ts);
    void     DriverAgg(const EtwObj& e, int sign);
    bool     CanBind(const EtwObj& e, const LiveObject& o) const;
    void     Bind(uint64_t key, EtwObj& e, LiveObject& o, uint64_t ts);
    void     TryBindEtw(uint64_t key, uint64_t ts);
    void     ApplyGroup(EtwObj& e, MemLoc g, uint64_t ts);
    void     ApplySize(EtwObj& e, uint64_t size, uint64_t ts);
    EtwObj*  FindEtw(uint64_t lib_id, uint64_t* key_out);
    void     EtwOnCreated(LiveObject& o, uint64_t ts);
    void     EtwOnDestroyed(LiveObject& o, uint64_t ts);
    void     RefreshJoinStats();

    // Sidecar JSONL.
    void     SideLine(const std::string& line);
    void     SideFlush();

    mutable std::mutex mu_;
    Snapshot s_;
    std::unordered_map<uint64_t, LiveObject> live_;

    // --etw state
    bool     etw_enabled_ = false;
    bool     hello_seen_  = false;
    uint64_t qpc_freq_     = 0;
    uint64_t qpc_start_    = 0;
    uint64_t qpc_start_ns_ = 0;   // qpc_start in absolute QPC-ns (library timeline)
    uint64_t child_exit_ts_ = UINT64_MAX;  // ts_ns of the child's exit (see SetChildExitQpc)

    std::unordered_map<uint64_t, uint64_t> ptr_to_id_;      // object_ptr -> live id
    // Recently destroyed tracked objects per address (oldest first), so ETW
    // records that arrive after a short-lived object died can still be
    // attributed to it ("late match") instead of to the address's next owner.
    struct DeadObj {
        uint64_t   id;
        ObjectType type;
        uint64_t   prev_destroy_ns;  // death of the occupant before it
        uint64_t   destroyed_ns;
    };
    std::unordered_map<uint64_t, std::deque<DeadObj>> dead_by_ptr_;
    std::deque<std::pair<uint64_t, uint64_t>> destroy_hist_; // (ts_ns, ptr) for pruning
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> children_;  // heap id -> placed ids

    std::unordered_map<uint64_t, EtwObj>   etw_objs_;   // canonical lib id -> object
    std::unordered_map<uint64_t, uint64_t> etw_alias_;  // any lib id -> canonical lib id
    std::unordered_map<uint64_t, uint64_t> etw_by_addr_; // address -> canonical lib id
    std::deque<uint64_t> pending_q_;                     // canonical ids, oldest first

    struct CounterVal { etw::CounterKind kind; uint64_t bytes; };
    std::unordered_map<uint64_t, CounterVal> counters_;  // counter id -> latest

    uint64_t last_second_ms_ = 0;

    // Sidecar
    FILE*        side_ = nullptr;
    std::string  side_prebuf_;    // lines produced before Hello (no time base yet)
    std::wstring side_path_, session_name_, main_log_path_;
    uint32_t     rundown_interval_ms_ = 0;
};

} // namespace dx12track
