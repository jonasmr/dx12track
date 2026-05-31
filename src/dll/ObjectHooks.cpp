#include "ObjectHooks.h"

#include "Tracker.h"

// WKPDID_D3DDebugObjectName{,W} live in d3dcommon.h, which directx/d3d12.h
// pulls in transitively. No direct <d3dcommon.h> include — the Windows SDK
// copy carries auto-link pragmas for MSVCRTD that fight our static CRT.
#include <directx/d3d12.h>
#include <string>
#include <unknwn.h>

namespace dx12track {

namespace {

using PfnRelease        = ULONG   (STDMETHODCALLTYPE *)(IUnknown*);
using PfnSetName        = HRESULT (STDMETHODCALLTYPE *)(IUnknown*, LPCWSTR);
using PfnSetPrivateData = HRESULT (STDMETHODCALLTYPE *)(IUnknown*, REFGUID,
                                                         UINT, const void*);

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

PfnSetPrivateData GetRealSetPrivateData(IUnknown* self) {
    auto* p = GlobalTracker().PatchVTableIfNew(self);
    return p ? reinterpret_cast<PfnSetPrivateData>(p->real_setprivatedata)
             : nullptr;
}

// Returns the visible UTF-16 name carried by a SetPrivateData(name) call, or
// an empty string when the GUID isn't a debug-name GUID. `DataSize` is the
// byte length of `pData` and may or may not include a terminator depending
// on the caller; we treat the buffer as bounded by DataSize and add our own
// terminator.
std::wstring ExtractDebugName(REFGUID guid, UINT DataSize, const void* pData) {
    if (DataSize == 0 || pData == nullptr) return {};

    if (guid == WKPDID_D3DDebugObjectNameW) {
        const size_t chars = DataSize / sizeof(wchar_t);
        const wchar_t* w = static_cast<const wchar_t*>(pData);
        // Trim at first NUL if the caller included one.
        size_t n = 0;
        while (n < chars && w[n] != L'\0') ++n;
        return std::wstring(w, w + n);
    }

    if (guid == WKPDID_D3DDebugObjectName) {
        const char* a = static_cast<const char*>(pData);
        size_t n = 0;
        while (n < DataSize && a[n] != '\0') ++n;
        if (n == 0) return {};
        int wide = MultiByteToWideChar(CP_ACP, 0, a, (int)n, nullptr, 0);
        if (wide <= 0) return {};
        std::wstring out(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, a, (int)n, out.data(), wide);
        return out;
    }

    return {};
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

HRESULT STDMETHODCALLTYPE Hook_SetPrivateData(IUnknown* self, REFGUID guid,
                                               UINT DataSize, const void* pData) {
    auto real = GetRealSetPrivateData(self);
    HRESULT hr = real ? real(self, guid, DataSize, pData) : S_OK;
    if (SUCCEEDED(hr)) {
        std::wstring name = ExtractDebugName(guid, DataSize, pData);
        if (!name.empty()) {
            GlobalTracker().OnSetName(self, name.c_str());
        }
    }
    return hr;
}

} // namespace dx12track
