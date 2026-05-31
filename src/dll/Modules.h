#pragma once

#include <windows.h>

namespace dx12track {

// Enumerate every module currently loaded in this process and emit a
// ModuleLoaded event for each. Then register with the NT loader so we get
// further load/unload notifications. Idempotent — safe to call once.
void StartModuleTracking();

// Best-effort: unregister the LDR notification before the DLL goes away.
void StopModuleTracking();

} // namespace dx12track
