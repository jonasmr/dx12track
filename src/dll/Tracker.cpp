#include "Tracker.h"

#include "JsonLog.h"
#include "ObjectHooks.h"
#include "PipeClient.h"

#include <cstring>

namespace dx12track {

namespace {

// IUnknown::Release lives at vtable index 2; ID3D12Object::SetName at 6.
constexpr size_t kSlot_Release = 2;
constexpr size_t kSlot_SetName = 6;

void* PatchSlot(void** vtable, size_t index, void* new_fn) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*),
                        PAGE_READWRITE, &old))
        return nullptr;
    void* prev = vtable[index];
    vtable[index] = new_fn;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old, &ignored);
    return prev;
}

uint64_t NowNsForJson() {
    static LARGE_INTEGER freq{}, start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    long double ns = (long double)(now.QuadPart - start.QuadPart) * 1e9L /
                     (long double)freq.QuadPart;
    return (uint64_t)ns;
}

} // namespace

Tracker::Tracker() = default;

const Tracker::VTablePatch* Tracker::PatchVTableIfNew(IUnknown* obj) {
    if (!obj) return nullptr;
    void** vtable = *reinterpret_cast<void***>(obj);

    std::lock_guard<std::mutex> lock(mu_);
    auto it = vtable_patches_.find(vtable);
    if (it != vtable_patches_.end()) return &it->second;

    VTablePatch p{};
    p.real_release = PatchSlot(vtable, kSlot_Release,
                               reinterpret_cast<void*>(&Hook_Release));
    p.real_setname = PatchSlot(vtable, kSlot_SetName,
                               reinterpret_cast<void*>(&Hook_SetName));
    auto [ins, _] = vtable_patches_.emplace(vtable, p);
    return &ins->second;
}

uint64_t Tracker::Register(IUnknown* obj, ObjectInfo info) {
    if (!obj) return 0;

    // Patch the vtable first so a fast Release after this Register call still
    // hits our hook.
    PatchVTableIfNew(obj);

    info.id = next_id_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[obj] = info;
    }

    EmitCreated(info);
    return info.id;
}

uint64_t Tracker::LookupId(IUnknown* obj) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(obj);
    return (it == objects_.end()) ? 0 : it->second.id;
}

void Tracker::OnReleaseToZero(IUnknown* obj) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = objects_.find(obj);
        if (it == objects_.end()) return;
        id = it->second.id;
        objects_.erase(it);
    }
    EmitDestroyed(id);
}

void Tracker::OnSetName(IUnknown* obj, const wchar_t* name) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = objects_.find(obj);
        if (it == objects_.end()) return;
        id = it->second.id;
        it->second.name.assign(name ? name : L"");
    }
    EmitRenamed(id, name ? name : L"");
}

void Tracker::EmitCreated(const ObjectInfo& info) {
    CreatedPayload p{};
    p.id             = info.id;
    p.size_bytes     = info.size_bytes;
    p.parent_heap_id = info.parent_heap_id;
    p.heap_type      = info.heap_type;
    p.dimension      = info.dimension;
    p.format         = info.format;
    p.type           = info.type;
    p.alloc          = info.alloc;
    size_t n = info.name.size();
    if (n >= kMaxNameChars) n = kMaxNameChars - 1;
    if (n) memcpy(p.name, info.name.c_str(), n * sizeof(wchar_t));
    p.name[n] = 0;

    GlobalPipe().Send(EventKind::Created, &p, sizeof(p));
    GlobalLog().Append(EventKind::Created, NowNsForJson(), &p, sizeof(p));
}

void Tracker::EmitRenamed(uint64_t id, const wchar_t* name) {
    RenamedPayload p{};
    p.id = id;
    if (name) {
        size_t n = wcsnlen(name, kMaxNameChars - 1);
        memcpy(p.name, name, n * sizeof(wchar_t));
        p.name[n] = 0;
    }
    GlobalPipe().Send(EventKind::Renamed, &p, sizeof(p));
    GlobalLog().Append(EventKind::Renamed, NowNsForJson(), &p, sizeof(p));
}

void Tracker::EmitDestroyed(uint64_t id) {
    DestroyedPayload p{}; p.id = id;
    GlobalPipe().Send(EventKind::Destroyed, &p, sizeof(p));
    GlobalLog().Append(EventKind::Destroyed, NowNsForJson(), &p, sizeof(p));
}

Tracker& GlobalTracker() {
    static Tracker g;
    return g;
}

} // namespace dx12track
