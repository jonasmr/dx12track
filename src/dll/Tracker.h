#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

#include "EventTypes.h"

struct IUnknown;
struct ID3D12Device;

namespace dx12track {

struct ObjectInfo {
    uint64_t       id;
    ObjectType     type;
    AllocationKind alloc;
    uint32_t       heap_type;
    uint32_t       dimension;
    uint32_t       format;
    uint64_t       size_bytes;
    uint64_t       parent_heap_id;
    std::wstring   name;
};

class Tracker {
public:
    Tracker();

    // Register an object the first time we see it. If `obj` is already tracked,
    // updates the existing record. Patches the object's vtable for Release+SetName
    // exactly once per distinct vtable encountered.
    uint64_t Register(IUnknown* obj, ObjectInfo info);

    // Lookup the tracking id for `obj`, or 0 if not tracked.
    uint64_t LookupId(IUnknown* obj);

    // Called from the Release hook AFTER the real Release has returned the value
    // 0. Emits a Destroyed event and removes the entry.
    void OnReleaseToZero(IUnknown* obj);

    // Called from the SetName hook. Updates the record and emits Renamed.
    void OnSetName(IUnknown* obj, const wchar_t* name);

    // Vtable hook bookkeeping --------------------------------------------------
    struct VTablePatch {
        void* real_release        = nullptr;   // original IUnknown::Release       (slot 2)
        void* real_setprivatedata = nullptr;   // original ID3D12Object::SetPrivateData (slot 4)
        void* real_setname        = nullptr;   // original ID3D12Object::SetName        (slot 6)
    };
    // Returns the patch info for the vtable backing `obj`. If the vtable hasn't
    // been patched yet, patches it now (Release+SetName) and records the
    // originals. Returns NULL if patching failed.
    const VTablePatch* PatchVTableIfNew(IUnknown* obj);

private:
    void EmitCreated(const ObjectInfo& info);
    void EmitRenamed(uint64_t id, const wchar_t* name);
    void EmitDestroyed(uint64_t id);

    std::mutex                                  mu_;
    std::atomic<uint64_t>                       next_id_{1};
    std::unordered_map<IUnknown*, ObjectInfo>   objects_;
    std::unordered_map<void*, VTablePatch>      vtable_patches_;
};

Tracker& GlobalTracker();

} // namespace dx12track
