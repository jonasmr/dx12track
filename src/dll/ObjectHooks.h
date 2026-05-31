#pragma once

#include <windows.h>

struct IUnknown;

namespace dx12track {

// IUnknown::Release hook (slot 2). Installed into every D3D12 object's vtable
// the first time we see an instance of that class.
ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self);

// ID3D12Object::SetName hook (slot 6).
HRESULT STDMETHODCALLTYPE Hook_SetName(IUnknown* self, LPCWSTR name);

} // namespace dx12track
