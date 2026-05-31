#include "Model.h"

#include <cstring>

namespace dx12track {

Model::Model() = default;

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
            o.created_ns     = hdr.ts_ns;
            o.name.assign(p->name, wcsnlen(p->name, kMaxNameChars));
            live_[o.id] = o;

            const size_t hb = HeapBucket(o.heap_type);
            const size_t ab = AllocBucket(o.alloc);
            s_.totals.bytes [hb][ab] += o.size_bytes;
            s_.totals.counts[hb][ab] += 1;
            s_.live_count = live_.size();
            s_.live_bytes += o.size_bytes;
            s_.per_type_counts[static_cast<size_t>(o.type)] += 1;

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
            const LiveObject& o = it->second;

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
    }
}

Model::Snapshot Model::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return s_;
}

void Model::MarkChildExited(uint32_t exit_code) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.child_exited    = true;
    s_.child_exit_code = exit_code;
}

} // namespace dx12track
