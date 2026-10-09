#include "Model.h"

#include <cstring>
#include <intrin.h>
#include <windows.h>

namespace dx12track {

namespace {

// --etw join tuning.
//
// An ETW object that has an address but no live pipe object at that address
// waits this long before it's counted as "pending expired". It can still bind
// later; the timeout only affects the diagnostics.
constexpr uint64_t kEtwPendingTimeoutMs = 10'000;
// An ETW object at address P created at time T belongs to the tracked object
// at P whose lifetime contains T: ETW stamps the creation inside the Create*
// call (shortly before the DLL registers the object) and the DLL stamps the
// previous occupant's death right after its real Release returned. If another
// thread reuses the address in that tiny window, T can precede the recorded
// death by a few microseconds - hence a small slack. Keep it small: allocators
// that churn upload pages reuse an address within a millisecond or two.
constexpr uint64_t kBindSlackNs = 100'000;
// How long destroyed objects are remembered per address (ETW typically runs
// ~1 s behind the pipe), and how many per address (allocators that churn
// transient objects reuse the same address hundreds of times a second).
constexpr uint64_t kDestroyHistoryNs = 10'000'000'000ull;
constexpr size_t   kDeadPerAddress   = 1024;

uint64_t TicksToNs(uint64_t ticks, uint64_t freq) {
    if (!freq) return 0;
    uint64_t hi = 0;
    const uint64_t lo = _umul128(ticks, 1'000'000'000ull, &hi);
    if (hi >= freq) return UINT64_MAX;
    uint64_t rem = 0;
    return _udiv128(hi, lo, freq, &rem);
}

uint64_t NowMs() { return GetTickCount64(); }

bool IsDriverType(etw::ObjType t) {
    return t == etw::ObjType::PipelineState || t == etw::ObjType::StateObject ||
           t == etw::ObjType::DescriptorHeap || t == etw::ObjType::CommandAllocator;
}

// ETW library type <-> dx12track hook type. Device / StateObject / MetaCommand
// have no hooked counterpart and never bind.
bool Compatible(etw::ObjType e, ObjectType o) {
    switch (e) {
        case etw::ObjType::Resource:         return o == ObjectType::Resource;
        case etw::ObjType::Heap:             return o == ObjectType::Heap;
        case etw::ObjType::PipelineState:    return o == ObjectType::PipelineState;
        case etw::ObjType::CommandAllocator: return o == ObjectType::CommandAllocator;
        case etw::ObjType::DescriptorHeap:   return o == ObjectType::DescriptorHeap;
        default:                             return false;
    }
}
bool Bindable(etw::ObjType e) {
    return e == etw::ObjType::Resource || e == etw::ObjType::Heap ||
           e == etw::ObjType::PipelineState || e == etw::ObjType::CommandAllocator ||
           e == etw::ObjType::DescriptorHeap;
}

MemLoc FromGroup(etw::Group g) {
    switch (g) {
        case etw::Group::Local:    return MemLoc::Vram;
        case etw::Group::NonLocal: return MemLoc::Sys;
        default:                   return MemLoc::Unknown;
    }
}

const char* LocName(MemLoc l) {
    switch (l) {
        case MemLoc::Vram: return "vram";
        case MemLoc::Sys:  return "sys";
        default:           return "unknown";
    }
}

const char* ResidencyOpName(etw::ResidencyOp op) {
    switch (op) {
        case etw::ResidencyOp::PageIn:  return "page_in";
        case etw::ResidencyOp::PageOut: return "page_out";
        case etw::ResidencyOp::Evict:   return "evict";
        default:                        return "make_resident";
    }
}

// ---- tiny JSON line builder ------------------------------------------------

struct J {
    std::string s;
    explicit J(const char* event, uint64_t ts_ns) {
        s.reserve(160);
        s += "{\"event\":\""; s += event; s += "\",\"ts_ns\":";
        s += std::to_string(ts_ns);
    }
    J& u(const char* k, uint64_t v) {
        s += ",\""; s += k; s += "\":"; s += std::to_string(v); return *this;
    }
    J& i(const char* k, int64_t v) {
        s += ",\""; s += k; s += "\":"; s += std::to_string(v); return *this;
    }
    J& b(const char* k, bool v) {
        s += ",\""; s += k; s += "\":"; s += v ? "true" : "false"; return *this;
    }
    J& str(const char* k, const char* v) {
        s += ",\""; s += k; s += "\":\""; s += v; s += '"'; return *this;
    }
    J& hex(const char* k, uint64_t v) {
        char b[24]; snprintf(b, sizeof(b), "\"0x%llx\"", (unsigned long long)v);
        s += ",\""; s += k; s += "\":"; s += b; return *this;
    }
    J& wstr(const char* k, const std::wstring& v) {
        s += ",\""; s += k; s += "\":\"";
        for (wchar_t c : v) {
            switch (c) {
                case L'\\': s += "\\\\"; continue;
                case L'"':  s += "\\\""; continue;
                case L'\n': s += "\\n";  continue;
                case L'\r': s += "\\r";  continue;
                case L'\t': s += "\\t";  continue;
                default: break;
            }
            if (c < 0x20) {
                char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                s += esc; continue;
            }
            char utf8[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, sizeof(utf8),
                                        nullptr, nullptr);
            if (n > 0) s.append(utf8, utf8 + n);
        }
        s += '"';
        return *this;
    }
    std::string done() { s += "}\n"; return std::move(s); }
};

} // namespace

Model::Model() = default;

Model::~Model() {
    if (side_) { fclose(side_); side_ = nullptr; }
}

size_t Model::HeapBucket(uint32_t heap_type) {
    switch (heap_type) {
        case D3D12_HEAP_TYPE_DEFAULT:    return 1;
        case D3D12_HEAP_TYPE_UPLOAD:     return 2;
        case D3D12_HEAP_TYPE_READBACK:   return 3;
        case D3D12_HEAP_TYPE_CUSTOM:     return 4;
        case D3D12_HEAP_TYPE_GPU_UPLOAD: return 5;
        default:                         return 0;
    }
}

size_t Model::AllocBucket(AllocationKind a) {
    return static_cast<size_t>(a);
}

void Model::OnEvent(const EventHeader& hdr, const void* payload, size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.latest_ns = hdr.ts_ns;

    switch (hdr.kind) {
        case EventKind::Hello: {
            if (bytes < sizeof(HelloPayload)) return;
            auto* p = static_cast<const HelloPayload*>(payload);
            s_.pid = p->pid;
            s_.exe_path.assign(p->exe_path,
                wcsnlen(p->exe_path, kMaxNameChars));
            s_.dll_start_ns = hdr.ts_ns;

            qpc_freq_     = p->qpc_frequency;
            qpc_start_    = p->qpc_start;
            qpc_start_ns_ = TicksToNs(qpc_start_, qpc_freq_);
            const bool first = !hello_seen_;
            hello_seen_   = true;
            if (etw_enabled_ && first && side_) {
                std::string hello =
                    J("etw_hello", hdr.ts_ns)
                        .u("etw_format", 1)
                        .u("pid", p->pid)
                        .u("qpc_freq", qpc_freq_)
                        .u("qpc_start", qpc_start_)
                        .wstr("session", session_name_)
                        .u("rundown_interval_ms", rundown_interval_ms_)
                        .u("pending_timeout_ms", kEtwPendingTimeoutMs)
                        .wstr("main_log", main_log_path_)
                        .done();
                fwrite(hello.data(), 1, hello.size(), side_);
                if (!side_prebuf_.empty()) {
                    fwrite(side_prebuf_.data(), 1, side_prebuf_.size(), side_);
                    side_prebuf_.clear();
                    side_prebuf_.shrink_to_fit();
                }
            }
            break;
        }
        case EventKind::Created: {
            if (bytes < sizeof(CreatedPayload)) return;
            auto* p = static_cast<const CreatedPayload*>(payload);
            LiveObject o{};
            o.id             = p->id;
            o.type           = p->type;
            o.alloc          = p->alloc;
            o.heap_type      = p->heap_type;
            o.dimension      = p->dimension;
            o.format         = p->format;
            o.size_bytes     = p->size_bytes;
            o.parent_heap_id = p->parent_heap_id;
            o.object_ptr     = p->object_ptr;
            o.created_ns     = hdr.ts_ns;
            o.name.assign(p->name, wcsnlen(p->name, kMaxNameChars));
            LiveObject& stored = (live_[o.id] = o);

            const size_t hb = HeapBucket(o.heap_type);
            const size_t ab = AllocBucket(o.alloc);
            s_.totals.bytes [hb][ab] += o.size_bytes;
            s_.totals.counts[hb][ab] += 1;
            s_.live_count = live_.size();
            s_.live_bytes += o.size_bytes;
            s_.per_type_counts[static_cast<size_t>(o.type)] += 1;

            if (etw_enabled_) EtwOnCreated(stored, hdr.ts_ns);

            ActivityLine line{};
            line.ts_ns      = hdr.ts_ns;
            line.created    = true;
            line.type       = o.type;
            line.id         = o.id;
            line.size_bytes = o.size_bytes;
            line.name       = o.name;
            if (o.alloc != AllocationKind::None) {
                line.heap_label =
                    std::wstring(L"") +
                    (o.alloc == AllocationKind::Committed ? L"Committed/" :
                     o.alloc == AllocationKind::Placed    ? L"Placed/"    :
                     o.alloc == AllocationKind::Reserved  ? L"Reserved/"  :
                                                            L"Heap/");
                const char* h = HeapTypeName(o.heap_type);
                while (*h) line.heap_label.push_back(static_cast<wchar_t>(*h++));
            }
            s_.recent.push_back(std::move(line));
            if (s_.recent.size() > 32) s_.recent.pop_front();
            break;
        }
        case EventKind::Renamed: {
            if (bytes < sizeof(RenamedPayload)) return;
            auto* p = static_cast<const RenamedPayload*>(payload);
            auto it = live_.find(p->id);
            if (it != live_.end())
                it->second.name.assign(p->name,
                    wcsnlen(p->name, kMaxNameChars));
            break;
        }
        case EventKind::Destroyed: {
            if (bytes < sizeof(DestroyedPayload)) return;
            auto* p = static_cast<const DestroyedPayload*>(payload);
            auto it = live_.find(p->id);
            if (it == live_.end()) return;
            LiveObject& o = it->second;

            if (etw_enabled_) EtwOnDestroyed(o, hdr.ts_ns);

            const size_t hb = HeapBucket(o.heap_type);
            const size_t ab = AllocBucket(o.alloc);
            if (s_.totals.bytes [hb][ab] >= o.size_bytes)
                s_.totals.bytes [hb][ab] -= o.size_bytes;
            if (s_.totals.counts[hb][ab] > 0)
                s_.totals.counts[hb][ab] -= 1;
            if (s_.live_bytes >= o.size_bytes)
                s_.live_bytes -= o.size_bytes;
            if (s_.per_type_counts[static_cast<size_t>(o.type)] > 0)
                s_.per_type_counts[static_cast<size_t>(o.type)] -= 1;

            ActivityLine line{};
            line.ts_ns      = hdr.ts_ns;
            line.created    = false;
            line.type       = o.type;
            line.id         = o.id;
            line.size_bytes = o.size_bytes;
            line.name       = o.name;
            s_.recent.push_back(std::move(line));
            if (s_.recent.size() > 32) s_.recent.pop_front();

            live_.erase(it);
            s_.live_count = live_.size();
            break;
        }
        case EventKind::Goodbye: {
            if (bytes < sizeof(GoodbyePayload)) return;
            auto* p = static_cast<const GoodbyePayload*>(payload);
            s_.child_exited    = true;
            s_.child_exit_code = p->exit_code;
            break;
        }
        default:
            break;
    }
}

Model::Snapshot Model::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    Snapshot snap = s_;
    if (etw_enabled_) {
        // Counters: a handful of entries; sum each kind over adapters.
        for (const auto& [id, c] : counters_) {
            const size_t k = static_cast<size_t>(c.kind);
            snap.etw.counter[k] += c.bytes;
            snap.etw.counter_seen[k] = true;
        }
    }
    return snap;
}

void Model::MarkChildExited(uint32_t exit_code) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.child_exited    = true;
    s_.child_exit_code = exit_code;
}

void Model::SetChildExitQpc(uint64_t exit_qpc) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!hello_seen_ || !qpc_freq_) return;
    child_exit_ts_ = exit_qpc > qpc_start_ ? TicksToNs(exit_qpc - qpc_start_, qpc_freq_) : 0;
}

// ===========================================================================
// --etw: setup / periodic / teardown
// ===========================================================================

bool Model::EnableEtw(const std::wstring& sidecar_path, const std::wstring& session_name,
                      const std::wstring& main_log_path, uint32_t rundown_interval_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    etw_enabled_         = true;
    s_.etw.enabled       = true;
    side_path_           = sidecar_path;
    session_name_        = session_name;
    main_log_path_       = main_log_path;
    rundown_interval_ms_ = rundown_interval_ms;
    side_ = _wfopen(sidecar_path.c_str(), L"wb");
    if (side_) setvbuf(side_, nullptr, _IOFBF, 256 * 1024);
    return side_ != nullptr;
}

void Model::DisableEtw(const std::wstring& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.etw.running = false;
    s_.etw.status  = reason;
    etw_enabled_   = false;   // stop doing join work; keep s_.etw.enabled for the UI
    if (side_) {
        fclose(side_);
        side_ = nullptr;
        DeleteFileW(side_path_.c_str());
    }
    side_prebuf_.clear();
}

void Model::SetEtwRunning(bool running) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.etw.running = running;
}

void Model::EtwTick(const etw::Stats& session_stats) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    s_.etw.session = session_stats;

    const uint64_t now_ms = NowMs();

    // Pending binds that timed out (diagnostics only; they may still bind).
    while (!pending_q_.empty()) {
        auto it = etw_objs_.find(pending_q_.front());
        if (it == etw_objs_.end() || it->second.bound_id || !it->second.pending_since_ms) {
            pending_q_.pop_front();
            continue;
        }
        if (now_ms - it->second.pending_since_ms < kEtwPendingTimeoutMs) break;
        it->second.pending_since_ms = 0;
        it->second.expired = true;
        ++s_.etw.pending_expired;
        pending_q_.pop_front();
    }

    // Forget old destroyed objects.
    const uint64_t now_ts = NowTsLocked();
    while (!destroy_hist_.empty() &&
           now_ts > kDestroyHistoryNs &&
           destroy_hist_.front().first < now_ts - kDestroyHistoryNs) {
        const uint64_t horizon = now_ts - kDestroyHistoryNs;
        auto it = dead_by_ptr_.find(destroy_hist_.front().second);
        if (it != dead_by_ptr_.end()) {
            while (!it->second.empty() && it->second.front().destroyed_ns < horizon)
                it->second.pop_front();
            if (it->second.empty()) dead_by_ptr_.erase(it);
        }
        destroy_hist_.pop_front();
    }

    if (now_ms - last_second_ms_ < 1000) return;
    last_second_ms_ = now_ms;

    RefreshJoinStats();

    // Once-per-second counters line.
    if (!counters_.empty() && side_) {
        uint64_t v[EtwSnapshot::kCounters] = {};
        bool seen[EtwSnapshot::kCounters] = {};
        for (const auto& [id, c] : counters_) {
            v[static_cast<size_t>(c.kind)] += c.bytes;
            seen[static_cast<size_t>(c.kind)] = true;
        }
        using CK = etw::CounterKind;
        static const struct { CK kind; const char* key; } kKeys[] = {
            { CK::LocalBudget,           "local_budget" },
            { CK::LocalResident,         "local_resident" },
            { CK::LocalUsage,            "local_usage" },
            { CK::NonLocalBudget,        "nonlocal_budget" },
            { CK::NonLocalResident,      "nonlocal_resident" },
            { CK::NonLocalUsage,         "nonlocal_usage" },
            { CK::DemotedMin,            "demoted_min" },
            { CK::DemotedLow,            "demoted_low" },
            { CK::DemotedNormal,         "demoted_normal" },
            { CK::DemotedHigh,           "demoted_high" },
            { CK::DemotedMax,            "demoted_max" },
            { CK::PagingLocalToNonLocal, "paging_local_to_nonlocal" },
            { CK::PagingNonLocalToLocal, "paging_nonlocal_to_local" },
        };
        J j("counters", now_ts);
        uint64_t demoted = 0; bool any_demoted = false;
        for (const auto& k : kKeys) {
            const size_t i = static_cast<size_t>(k.kind);
            if (!seen[i]) continue;
            j.u(k.key, v[i]);
            if (k.kind >= CK::DemotedMin && k.kind <= CK::DemotedMax) {
                demoted += v[i]; any_demoted = true;
            }
        }
        if (any_demoted) j.u("demoted", demoted);
        j.u("page_ins", s_.etw.page_ins).u("page_outs", s_.etw.page_outs);
        SideLine(j.done());
    }
    SideFlush();
}

void Model::FinishEtw(const etw::Stats& st) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.etw.session = st;
    s_.etw.running = false;
    if (!etw_enabled_) return;
    RefreshJoinStats();
    if (side_) {
        static const char* kUnboundKeys[EtwSnapshot::kTypes] = {
            nullptr, "unbound_device", "unbound_resource", "unbound_heap",
            "unbound_pipeline_state", "unbound_state_object",
            "unbound_command_allocator", "unbound_descriptor_heap",
            "unbound_meta_command",
        };
        J j("etw_stats", NowTsLocked());
        j.u("events", st.events).u("events_lost", st.events_lost)
         .u("buffers_lost", st.buffers_lost).u("exceptions", st.exceptions)
         .u("rundowns", st.rundowns).u("rundown_failures", st.rundown_failures)
         .u("etw_objects_created", s_.etw.created)
         .u("rundown_duplicates", s_.etw.rundown_duplicates)
         .u("binds", s_.etw.binds).u("late_matches", s_.etw.late_matches)
         .u("bound_alive", s_.etw.bound)
         .u("unbound_alive", s_.etw.unbound)
         .u("pending_expired", s_.etw.pending_expired)
         .u("group_changes", s_.etw.group_changes)
         .u("make_resident", s_.etw.make_resident).u("evict", s_.etw.evict)
         .u("page_ins", s_.etw.page_ins).u("page_outs", s_.etw.page_outs)
         .u("post_exit_ignored", s_.etw.post_exit_ignored)
         .u("diagnostics", s_.etw.diagnostics);
        for (size_t t = 1; t < EtwSnapshot::kTypes; ++t)
            j.u(kUnboundKeys[t], s_.etw.unbound_by_type[t]);
        SideLine(j.done());
        if (side_prebuf_.size()) {  // never saw Hello: dump what we have
            fwrite(side_prebuf_.data(), 1, side_prebuf_.size(), side_);
            side_prebuf_.clear();
        }
        fclose(side_);
        side_ = nullptr;
    }
}

void Model::RefreshJoinStats() {
    EtwSnapshot& e = s_.etw;
    e.bound = e.unbound = e.pending = 0;
    for (auto& v : e.unbound_by_type) v = 0;
    for (const auto& [key, o] : etw_objs_) {
        if (o.bound_id) { ++e.bound; continue; }
        if (o.orphan_ts) continue;  // its object is gone; waiting for the ETW destroy
        ++e.unbound;
        ++e.unbound_by_type[static_cast<size_t>(o.type)];
        if (o.pending_since_ms) ++e.pending;
    }
}

// ===========================================================================
// --etw: helpers (mu_ held)
// ===========================================================================

uint64_t Model::ConvTs(int64_t abs_qpc_ns) const {
    if (!hello_seen_ || abs_qpc_ns <= 0) return 0;
    const uint64_t a = static_cast<uint64_t>(abs_qpc_ns);
    return a > qpc_start_ns_ ? a - qpc_start_ns_ : 0;
}

uint64_t Model::NowTsLocked() const {
    if (!hello_seen_ || !qpc_freq_) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const uint64_t q = static_cast<uint64_t>(now.QuadPart);
    return q > qpc_start_ ? TicksToNs(q - qpc_start_, qpc_freq_) : 0;
}

void Model::SideLine(const std::string& line) {
    if (!side_) return;
    if (!hello_seen_) { side_prebuf_ += line; return; }
    fwrite(line.data(), 1, line.size(), side_);
}

void Model::SideFlush() {
    if (side_) fflush(side_);
}

MemLoc Model::EffLoc(const LiveObject& o, bool* via_heap) const {
    *via_heap = false;
    // Placed resources: the kernel allocation belongs to the heap, so segment
    // moves are reported on the heap. Follow the heap when its location is
    // known; fall back to the resource's own (creation-time) group.
    if (o.alloc == AllocationKind::Placed && o.parent_heap_id) {
        auto h = live_.find(o.parent_heap_id);
        if (h != live_.end() && h->second.own_loc != MemLoc::Unknown) {
            *via_heap = true;
            return h->second.own_loc;
        }
    }
    return o.own_loc;
}

void Model::AddLoc(const LiveObject& o, int sign) {
    if (o.alloc == AllocationKind::None) return;
    uint64_t& b = s_.etw.loc_bytes[HeapBucket(o.heap_type)][static_cast<size_t>(o.eff_loc)];
    if (sign > 0) b += o.size_bytes;
    else b = (b >= o.size_bytes) ? b - o.size_bytes : 0;
}

void Model::RecomputeLoc(LiveObject& o, uint64_t ts, bool force_report) {
    bool via_heap = false;
    const MemLoc l = EffLoc(o, &via_heap);
    const bool changed = (l != o.eff_loc);
    if (changed) {
        AddLoc(o, -1);
        o.eff_loc = l;
        AddLoc(o, +1);
    }
    if (changed || force_report || (!o.loc_reported && l != MemLoc::Unknown)) {
        o.loc_reported = true;
        SideLine(J("location", ts).u("id", o.id).str("group", LocName(l))
                     .str("via", via_heap ? "heap" : "self").done());
    }
}

void Model::RecomputeChildren(uint64_t heap_id, uint64_t ts) {
    auto it = children_.find(heap_id);
    if (it == children_.end()) return;
    for (uint64_t cid : it->second) {
        auto c = live_.find(cid);
        if (c != live_.end()) RecomputeLoc(c->second, ts, false);
    }
}

void Model::DriverAgg(const EtwObj& e, int sign) {
    const size_t t = static_cast<size_t>(e.type);
    if (t >= EtwSnapshot::kTypes) return;
    auto sub = [](uint64_t& v, uint64_t d) { v = v >= d ? v - d : 0; };
    if (sign > 0) {
        ++s_.etw.live[t];
        if (IsDriverType(e.type) && e.size) { ++s_.etw.sized[t]; s_.etw.bytes[t] += e.size; }
    } else {
        sub(s_.etw.live[t], 1);
        if (IsDriverType(e.type) && e.size) { sub(s_.etw.sized[t], 1); sub(s_.etw.bytes[t], e.size); }
    }
}

Model::EtwObj* Model::FindEtw(uint64_t lib_id, uint64_t* key_out) {
    auto a = etw_alias_.find(lib_id);
    if (a == etw_alias_.end()) return nullptr;
    auto o = etw_objs_.find(a->second);
    if (o == etw_objs_.end()) return nullptr;
    if (key_out) *key_out = a->second;
    return &o->second;
}

bool Model::CanBind(const EtwObj& e, const LiveObject& o) const {
    if (e.bound_id || e.orphan_ts || !Compatible(e.type, o.type)) return false;
    // The ETW object must not predate the death of the previous object that
    // lived at this address (else it *is* that previous object).
    return e.created_ts + kBindSlackNs >= o.prev_destroy_ns;
}

void Model::Bind(uint64_t key, EtwObj& e, LiveObject& o, uint64_t ts) {
    if (o.etw_key && o.etw_key != key) {
        auto old = etw_objs_.find(o.etw_key);
        if (old != etw_objs_.end()) old->second.bound_id = 0;
    }
    e.bound_id = o.id;
    e.pending_since_ms = 0;
    o.etw_key = key;
    ++s_.etw.binds;
    SideLine(J("etw_bind", ts).u("id", o.id).u("lib_id", key)
                 .str("type", etw::ObjTypeName(e.type)).hex("ptr", o.object_ptr).done());

    o.own_loc = e.group;
    // Report on the first bind even if the group is unknown (unless a
    // creation-time `location` via the parent heap was already written).
    RecomputeLoc(o, ts, /*force_report=*/!o.loc_reported);
    if (o.type == ObjectType::Heap) RecomputeChildren(o.id, ts);

    if (IsDriverType(e.type)) {
        o.driver_size = e.size;
        if (e.size)
            SideLine(J("driver_size", ts).u("id", o.id).u("bytes", e.size).done());
    }
}

void Model::TryBindEtw(uint64_t key, uint64_t ts) {
    auto it = etw_objs_.find(key);
    if (it == etw_objs_.end()) return;
    EtwObj& e = it->second;
    if (e.bound_id || e.orphan_ts || !e.address || !Bindable(e.type)) return;
    auto p = ptr_to_id_.find(e.address);
    if (p != ptr_to_id_.end()) {
        auto o = live_.find(p->second);
        if (o != live_.end() && CanBind(e, o->second)) {
            Bind(key, e, o->second, ts);
            return;
        }
    }
    // Created while an already-destroyed tracked object owned the address?
    // (Typical for objects that lived shorter than the ETW latency.) Attribute
    // it to that object; it never binds to a live one.
    auto d = dead_by_ptr_.find(e.address);
    if (d != dead_by_ptr_.end()) {
        for (const DeadObj& x : d->second) {
            if (!Compatible(e.type, x.type)) continue;
            if (e.created_ts <= x.destroyed_ns + kBindSlackNs &&
                e.created_ts + kBindSlackNs >= x.prev_destroy_ns) {
                e.orphan_ts = x.destroyed_ns ? x.destroyed_ns : 1;
                e.pending_since_ms = 0;
                ++s_.etw.late_matches;
                SideLine(J("etw_bind", ts).u("id", x.id).u("lib_id", key)
                             .str("type", etw::ObjTypeName(e.type)).hex("ptr", e.address)
                             .b("late", true).done());
                return;
            }
        }
    }
    if (!e.pending_since_ms && !e.expired) {
        e.pending_since_ms = NowMs();
        if (!e.pending_since_ms) e.pending_since_ms = 1;
        pending_q_.push_back(key);
    }
}

void Model::ApplyGroup(EtwObj& e, MemLoc g, uint64_t ts) {
    e.group = g;
    if (!e.bound_id) return;
    auto o = live_.find(e.bound_id);
    if (o == live_.end()) return;
    o->second.own_loc = g;
    RecomputeLoc(o->second, ts, false);
    if (o->second.type == ObjectType::Heap) RecomputeChildren(o->second.id, ts);
}

void Model::ApplySize(EtwObj& e, uint64_t size, uint64_t ts) {
    // A later 0 means "not known", not "freed": keep the last non-zero size.
    if (!size || size == e.size) return;
    DriverAgg(e, -1);
    e.size = size;
    DriverAgg(e, +1);
    if (!e.bound_id || !IsDriverType(e.type)) return;
    auto o = live_.find(e.bound_id);
    if (o == live_.end() || o->second.driver_size == size) return;
    o->second.driver_size = size;
    SideLine(J("driver_size", ts).u("id", o->second.id).u("bytes", size).done());
}

void Model::EtwOnCreated(LiveObject& o, uint64_t ts) {
    if (o.object_ptr) {
        auto d = dead_by_ptr_.find(o.object_ptr);
        if (d != dead_by_ptr_.end() && !d->second.empty())
            o.prev_destroy_ns = d->second.back().destroyed_ns;
        ptr_to_id_[o.object_ptr] = o.id;
    }
    if (o.alloc == AllocationKind::Placed && o.parent_heap_id)
        children_[o.parent_heap_id].insert(o.id);

    bool via_heap = false;
    o.eff_loc = EffLoc(o, &via_heap);
    AddLoc(o, +1);
    if (o.eff_loc != MemLoc::Unknown) RecomputeLoc(o, ts, true);

    // ETW may have seen this object first.
    if (o.object_ptr) {
        auto a = etw_by_addr_.find(o.object_ptr);
        if (a != etw_by_addr_.end()) {
            auto e = etw_objs_.find(a->second);
            if (e != etw_objs_.end() && CanBind(e->second, o))
                Bind(a->second, e->second, o, ts);
        }
    }
}

void Model::EtwOnDestroyed(LiveObject& o, uint64_t ts) {
    AddLoc(o, -1);
    if (o.object_ptr) {
        auto p = ptr_to_id_.find(o.object_ptr);
        if (p != ptr_to_id_.end() && p->second == o.id) ptr_to_id_.erase(p);
        auto& dead = dead_by_ptr_[o.object_ptr];
        dead.push_back({ o.id, o.type, o.prev_destroy_ns, ts });
        if (dead.size() > kDeadPerAddress) dead.pop_front();
        destroy_hist_.emplace_back(ts, o.object_ptr);
    }
    if (o.etw_key) {
        auto e = etw_objs_.find(o.etw_key);
        if (e != etw_objs_.end() && e->second.bound_id == o.id) {
            e->second.bound_id  = 0;
            e->second.orphan_ts = ts ? ts : 1;
        }
        o.etw_key = 0;
    }
    if (o.alloc == AllocationKind::Placed && o.parent_heap_id) {
        auto c = children_.find(o.parent_heap_id);
        if (c != children_.end()) {
            c->second.erase(o.id);
            if (c->second.empty()) children_.erase(c);
        }
    }
    if (o.type == ObjectType::Heap) {
        // Placed resources normally die before their heap; if not, they fall
        // back to their own location. Recompute with the heap gone.
        // The heap is still in live_ here, so set the children's fallback
        // (their own group) explicitly instead of going through EffLoc.
        auto c = children_.find(o.id);
        if (c != children_.end()) {
            std::unordered_set<uint64_t> kids = std::move(c->second);
            children_.erase(c);
            for (uint64_t cid : kids) {
                auto k = live_.find(cid);
                if (k == live_.end()) continue;
                LiveObject& child = k->second;
                if (child.eff_loc != child.own_loc) {
                    AddLoc(child, -1);
                    child.eff_loc = child.own_loc;
                    AddLoc(child, +1);
                    SideLine(J("location", ts).u("id", child.id)
                                 .str("group", LocName(child.eff_loc))
                                 .str("via", "self").done());
                }
            }
        }
    }
}

// ===========================================================================
// --etw: etw::Sink (ETW consumer thread)
// ===========================================================================

void Model::OnEtwObjectCreated(int64_t ts, uint64_t lib_id, etw::ObjType type,
                               uint64_t size, etw::Group group) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_ || etw_alias_.count(lib_id)) return;
    EtwObj e;
    e.type       = type;
    e.size       = size;
    e.group      = FromGroup(group);
    e.created_ts = ConvTs(ts);
    DriverAgg(e, +1);
    etw_objs_.emplace(lib_id, std::move(e));
    etw_alias_[lib_id] = lib_id;
    ++s_.etw.created;
}

void Model::OnEtwObjectAddress(uint64_t lib_id, etw::ObjType /*type*/, uint64_t address) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_ || !address) return;
    uint64_t key = 0;
    EtwObj* e = FindEtw(lib_id, &key);
    if (!e || e->address) return;

    auto ex = etw_by_addr_.find(address);
    if (ex != etw_by_addr_.end() && ex->second != key) {
        auto old_it = etw_objs_.find(ex->second);
        if (old_it != etw_objs_.end() && old_it->second.type == e->type) {
            // A new library id for an address that still has a live ETW
            // object of the same type is a rundown re-report of that object.
            // (The library emits callbacks in ETW order and D3D12 logs an
            // object's destruction before its memory can be reused, so a
            // genuinely new object here would have seen the old one's destroy
            // first.) This holds even when the old one is an orphan: e.g. the
            // runtime's internal PSOs go through our hooks, drop to refcount 0
            // from the app's view at once, yet live on inside the runtime.
            EtwObj& old = old_it->second;
            const uint64_t into = ex->second;
            const uint64_t ts   = e->created_ts;
            ++s_.etw.rundown_duplicates;
            if (e->size) ApplySize(old, e->size, ts);
            if (e->group != MemLoc::Unknown && e->group != old.group)
                ApplyGroup(old, e->group, ts);
            DriverAgg(*e, -1);
            etw_objs_.erase(key);           // invalidates e
            old.aliases.push_back(key);
            etw_alias_[key] = into;
            TryBindEtw(into, ts);
            return;
        }
        // Different type: the old entry is stale (its destroy was never
        // reported); the new one takes the address. The old one still dies
        // via its ETW destroy, if that ever comes.
    }
    e->address = address;
    etw_by_addr_[address] = key;
    SideLine(J("etw_object", e->created_ts).u("lib_id", key)
                 .str("type", etw::ObjTypeName(e->type)).hex("ptr", address)
                 .u("size", e->size).str("group", LocName(e->group)).done());
    TryBindEtw(key, e->created_ts);
}

void Model::OnEtwObjectDestroyed(int64_t ts, uint64_t lib_id, etw::ObjType /*type*/) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    uint64_t key = 0;
    EtwObj* e = FindEtw(lib_id, &key);
    if (!e) return;
    if (e->address)
        SideLine(J("etw_object_destroyed", ConvTs(ts)).u("lib_id", key).done());
    if (e->bound_id) {
        // Our object is still alive (pipe destroy not seen yet, or the library
        // dropped and will re-report it): unbind but keep its last location.
        auto o = live_.find(e->bound_id);
        if (o != live_.end() && o->second.etw_key == key) o->second.etw_key = 0;
    }
    for (uint64_t a : e->aliases) etw_alias_.erase(a);
    etw_alias_.erase(key);
    if (e->address) {
        auto b = etw_by_addr_.find(e->address);
        if (b != etw_by_addr_.end() && b->second == key) etw_by_addr_.erase(b);
    }
    DriverAgg(*e, -1);
    etw_objs_.erase(key);
}

void Model::OnEtwObjectSize(int64_t ts, uint64_t lib_id, etw::ObjType /*type*/,
                            uint64_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    EtwObj* e = FindEtw(lib_id, nullptr);
    if (e) ApplySize(*e, size, ConvTs(ts));
}

void Model::OnEtwGroupChanges(const etw::GroupChange* changes, uint32_t count) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t ts = ConvTs(changes[i].ts);
        if (ts > child_exit_ts_) { ++s_.etw.post_exit_ignored; continue; }
        ++s_.etw.group_changes;
        EtwObj* e = FindEtw(changes[i].lib_id, nullptr);
        if (!e) continue;
        const MemLoc g = FromGroup(changes[i].group);
        if (g != e->group) ApplyGroup(*e, g, ts);
    }
}

void Model::OnEtwResidency(int64_t ts, uint64_t lib_id, etw::ObjType type,
                           etw::ResidencyOp op) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    if (ConvTs(ts) > child_exit_ts_) { ++s_.etw.post_exit_ignored; return; }
    switch (op) {
        case etw::ResidencyOp::MakeResident: ++s_.etw.make_resident; return;
        case etw::ResidencyOp::Evict:        ++s_.etw.evict;         return;
        case etw::ResidencyOp::PageIn:       ++s_.etw.page_ins;      break;
        case etw::ResidencyOp::PageOut:      ++s_.etw.page_outs;     break;
    }
    uint64_t key = 0;
    EtwObj* e = FindEtw(lib_id, &key);
    SideLine(J("residency", ConvTs(ts))
                 .u("id", e ? e->bound_id : 0)
                 .u("lib_id", e ? key : lib_id)
                 .str("type", etw::ObjTypeName(type))
                 .str("op", ResidencyOpName(op)).done());
}

void Model::OnEtwCounter(int64_t /*ts*/, uint64_t counter_id, etw::CounterKind kind,
                         double value_mb) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    const double b = value_mb * 1e6;
    counters_[counter_id] = { kind, b > 0 ? static_cast<uint64_t>(b + 0.5) : 0 };
}

void Model::OnEtwDiagnostic(bool error, int code, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!etw_enabled_) return;
    ++s_.etw.diagnostics;
    SideLine(J("etw_diag", NowTsLocked())
                 .str("severity", error ? "error" : "warning")
                 .i("code", code).wstr("msg", message).done());
}

} // namespace dx12track
