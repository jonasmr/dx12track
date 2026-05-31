#pragma once

#include <windows.h>

struct IUnknown;

namespace dx12track {

// IUnknown::Release hook (slot 2). Installed into every D3D12 object's vtable
// the first time we see an instance of that class.
ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self);

// ID3D12Object::SetName hook (slot 6).
HRESULT STDMETHODCALLTYPE Hook_SetName(IUnknown* self, LPCWSTR name);

// ID3D12Object::SetPrivateData hook (slot 4). The same vtable slot is used to
// install debug names on D3D12 objects when the app calls it with
// WKPDID_D3DDebugObjectName (ANSI) or WKPDID_D3DDebugObjectNameW (wide). Many
// engines port from D3D11 still use this path instead of SetName.
HRESULT STDMETHODCALLTYPE Hook_SetPrivateData(IUnknown* self,
                                              REFGUID guid,
                                              UINT DataSize,
                                              const void* pData);

} // namespace dx12track
