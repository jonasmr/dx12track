#include "ObjectHooks.h"

#include "Tracker.h"

#include <unknwn.h>

namespace dx12track {

namespace {

using PfnRelease = ULONG (STDMETHODCALLTYPE *)(IUnknown*);
using PfnSetName = HRESULT (STDMETHODCALLTYPE *)(IUnknown*, LPCWSTR);

// Resolve the original method by looking up the patch info for this object's
// vtable. The Tracker patched the vtable so the entry must exist.
PfnRelease GetRealRelease(IUnknown* self) {
    auto* p = GlobalTracker().PatchVTableIfNew(self);
    return p ? reinterpret_cast<PfnRelease>(p->real_release) : nullptr;
}

PfnSetName GetRealSetName(IUnknown* self) {
    auto* p = GlobalTracker().PatchVTableIfNew(self);
    return p ? reinterpret_cast<PfnSetName>(p->real_setname) : nullptr;
}

} // namespace

ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self) {
    auto real = GetRealRelease(self);
    if (!real) return 0;  // shouldn't happen — patch was installed
    ULONG ref = real(self);
    if (ref == 0) {
        GlobalTracker().OnReleaseToZero(self);
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE Hook_SetName(IUnknown* self, LPCWSTR name) {
    auto real = GetRealSetName(self);
    HRESULT hr = real ? real(self, name) : S_OK;
    if (SUCCEEDED(hr)) {
        GlobalTracker().OnSetName(self, name);
    }
    return hr;
}

} // namespace dx12track
