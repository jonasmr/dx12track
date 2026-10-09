# dx12track

A Windows tool that injects into a D3D12 game and tracks every D3D12 object it
creates — resources, heaps, descriptor heaps, pipeline states, command lists,
fences, query heaps, command signatures, root signatures, command allocators
and queues. Outputs a live console table and a JSON-Lines log of every
create / rename / destroy event. Optional per-creation callstack capture with
loaded-module metadata for offline symbolication.

Built for VS2022 / MSVC / Windows 11 x64. Headers come from the
[DirectX-Headers Agility SDK](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)
via vcpkg.

## Build

Prereqs: VS2022 (Desktop C++ workload, Windows SDK 10.0.26100), git, any
[vcpkg](https://learn.microsoft.com/vcpkg/get_started/get-started)
installation on `PATH` or `VCPKG_ROOT` (the VS-bundled copy works), and
nuget.org access. The project pins its vcpkg registry via
`vcpkg-configuration.json`, so the vcpkg tool's working-tree state doesn't
matter — only that it can fetch.

```powershell
git clone --recurse-submodules git@github.com:jonasmr/dx12track.git
cd dx12track
# (existing clone: git submodule update --init external/DxTimingCaptureLibrary)
MSBuild.exe dx12track.sln /restore /p:Configuration=Release /p:Platform=x64
```

`/restore` is needed (Visual Studio does it automatically): the
`dx12track_etw` project compiles
[DxTimingCaptureLibrary](external/DxTimingCaptureLibrary) (git submodule,
used unmodified) and gets `d3d12.h` / `D3D12Events.h` for it from the
`Microsoft.Direct3D.D3D12` NuGet package. Its build output, including the
NuGet restore files, goes under `build/`; nothing is written into the
submodule.

Outputs:

- `build/x64/Release/dx12track.exe` — the launcher
- `build/x64/Release/dx12track.dll` — the injected payload
- `build/x64/Release/dx12track.lib` — import library for clients that
  prefer linking over `GetProcAddress`
- `build/x64/Release/lib/dx12track_etw.lib` — ETW consumer + library,
  linked into the launcher (intermediate)

## Usage

### Mode 1: launcher-based injection

```
dx12track.exe [-o <log.jsonl>] [--callstacks] [--verbose] [--debugger] [--etw]
              [--dump-console <file>] [--] <target.exe> [target args...]
```

`dx12track.exe` does `CreateProcess(CREATE_SUSPENDED)` on the target, injects
`dx12track.dll` via `CreateRemoteThread(LoadLibraryW)`, resumes the target,
and from then on streams events over a per-PID named pipe. The launcher
maintains a live in-memory model and redraws a console table with
`WriteConsoleOutputW` at ~10 Hz showing:

- per-`(heap-type, allocation-kind)` byte totals
- live object counts per type
- recent create/destroy tail

```
DX12 Track   PID 36592   …\ModelViewer.exe   uptime 0:00:08   live=409   bytes=489011200
Memory by heap / allocation kind
  Heap         Heap-obj     Committed    Placed       Reserved     Total
  Default          0  B    415.69 MB         0  B         0  B    415.69 MB
  Upload           0  B     35.06 MB         0  B         0  B     35.06 MB
  TOTAL            0  B    450.75 MB         0  B         0  B    450.75 MB
...
```

When the target exits, a one-line plain-text summary is printed (visible even
when stdout is redirected and the WriteConsoleOutput surface isn't).

`--callstacks` (alias `-c`) turns on callstack capture and loaded-module
logging — see [Callstacks](#callstacks) below.

`--etw` adds per-object memory location (VRAM / system memory) and driver
memory sizes from ETW — see [Memory location via ETW](#memory-location-via-etw---etw).

`--dump-console <file>` is a debugging aid: the launcher also writes the
console frame as plain text to `<file>` (once per second and at exit), so
automated runs without a real console can see what would be drawn.

### Mode 2: in-process self-injection

For when the live console isn't useful (CI runs, automated repro capture,
embedding into your own dev build) — load the DLL yourself, call one C
function, and JSONL events stream straight to disk.

```c
#include <windows.h>
#include "dx12track.h"        // from dx12track's include/

int wWinMain(...) {
    HMODULE dll = LoadLibraryW(L"dx12track.dll");
    if (dll) {
        auto start = (Dx12Track_StartLogging_t)GetProcAddress(
            dll, "Dx12Track_StartLogging");
        if (start) start(L"run.jsonl", /*capture_callstacks=*/TRUE);
    }
    // … InitD3D12, run the app …
}
```

Or link against `dx12track.lib` and call `Dx12Track_StartLogging(...)`
directly — `dx12track.h` resolves the macro to `dllimport` in client TUs.

Hooks install in `DllMain`, so `LoadLibraryW` is what arms the tracker; the
`Start` call only opens the log file. Call it before
`D3D12CreateDevice` if you want to capture the device-creation event itself.

## Memory location via ETW (`--etw`)

```
dx12track.exe --etw -o run.jsonl game.exe
```

The hooks see what the app asked for; they can't see where the kernel
actually put it. With `--etw` the launcher also runs a real-time ETW session
(Direct3D12 + DxgKrnl providers) for the target, decoded by Microsoft's
[DxTimingCaptureLibrary](external/DxTimingCaptureLibrary), and joins the
library's objects with the hooked ones by address (the DLL reports the
interface pointer the app got from `Create*`, which is the address D3D12's
ETW provider logs). The session starts while the target is still suspended,
so it sees every object from the device on.

**Requirements:** administrator rights or membership in the
**Performance Log Users** group (sign out and in after adding yourself).
If the session can't start, the launcher prints why and continues without
ETW: access denied → the group/admin requirement; error 1460 (timeout) →
another process using D3D12 (browsers, overlays, streaming tools) didn't
answer the provider-enable request; retry or close those apps.

What it adds:

- **Console** (with the existing layout):
  - two status lines under the header:
    `VRAM resident / budget`, `sys resident`, `demoted` (memory that wanted
    video memory but was placed in system memory, summed over the five
    residency priorities), `pgIn` / `pgOut` (page-in / page-out operations
    so far) — all from the kernel's per-process counters — then
    `untracked (resident - tracked)` = VRAM resident + sys resident minus
    what dx12track accounts for (heap objects + committed resources + the
    ETW driver sizes below; clamped at 0): memory the process holds that no
    tracked object explains (swap chains, driver/runtime internals, pooled
    PSO memory, ...). Also how many ETW objects are bound / unbound / still
    pending, and lost ETW events (red if any).
  - `VRAM` / `Sys` / `Unknown` columns in the memory table: how much of each
    row's total is where. Placed resources follow their heap.
  - a **Driver memory (ETW)** section: live count, how many have a non-zero
    size, and bytes, for pipeline states, state objects, descriptor heaps
    and command allocators.
- **Sidecar log** `<log>.etw.jsonl` (`-o run.jsonl` → `run.etw.jsonl`):
  binds, every location change per object id, driver sizes, page-ins/outs,
  the counters once per second, library diagnostics and final statistics.
  Same `ts_ns` timeline as the main log. Format:
  [FORMAT.md](FORMAT.md#etw-sidecar-logetwjsonl).
- **Final summary**: ETW events received / lost, binds, what stayed unbound.

Every 10 s the launcher requests a D3D12 rundown, because pipeline-state
driver sizes are only reported that way.

Limitations:

- Pipeline-state sizes are approximate: they only arrive with a rundown,
  and drivers pool PSO memory (several PSOs share an allocation), so many
  PSOs report no size at all.
- Command-allocator sizes are always 0 in practice (shown as `n/a`).
- CPU-only (non-shader-visible) descriptor heaps are never reported by ETW:
  no location, no size.
- Swap-chain buffers and runtime-internal objects aren't created through the
  hooked `Create*` calls, so they stay unbound (they show up in
  `untracked`). Objects that live shorter than the ETW latency (~1 s) can't
  get a live location; the sidecar attributes them to the destroyed object
  (`"late":true`).
- Reserved (tiled) resources have no location.
- Counters are the kernel's raw values; one-sample dips happen.
- When the target is killed, location changes after its exit time (process
  teardown) are ignored.
- The ETW session sees every D3D12 process on the machine (the library
  filters by pid), so a busy system costs some launcher CPU.
- One session per launcher (`dx12track-etw-<launcher pid>`); stale sessions
  of killed launchers are stopped on the next `--etw` start, Ctrl+C stops
  the session too.

## Output: JSONL format

One JSON object per line. Stable record shapes (no nested objects beyond
arrays). Current protocol version is **4**. Sample lines:

```jsonl
{"event":"hello","ts_ns":0,"pid":36592,"protocol":4,"qpc_freq":10000000,"qpc_start":2849008433560,"exe":"…\\ModelViewer.exe"}
{"event":"module_loaded","ts_ns":0,"base":"0x7ff6c9060000","size":11304960,"timestamp":1780203981,"pdb_age":1,"pdb_guid":"8c3d35b8-e4f6-4d6e-8f7a-fd384f00bdbb","name":"…\\ModelViewer.exe","pdb_name":"…\\ModelViewer.pdb"}
{"event":"created","ts_ns":52606800,"id":12,"ptr":"0x22c3ca534b0","type":"Resource","alloc":"Committed","heap":"Default","dim":"Tex2D","format":28,"size":65536,"parent_heap_id":0,"name":"","stack":["0x7ff89ddc9228","0x7ff928dfb972",…]}
{"event":"renamed","ts_ns":53180300,"id":12,"name":"ShadowMap"}
{"event":"destroyed","ts_ns":99999,"id":12}
{"event":"goodbye","ts_ns":1234567890,"exit_code":0}
```

Event kinds: `hello` / `created` / `renamed` / `destroyed` /
`module_loaded` / `module_unloaded` / `goodbye` / `diag` (verbose mode).

See **[FORMAT.md](FORMAT.md)** for the full schema — per-event field
tables, a JSON-Schema draft-2020-12 document, versioning policy, and
consumer notes. The short version: addresses are quoted hex strings,
`ts_ns` is monotonic nanoseconds from 0 at attach time, `id` is unique
across a run, and unknown event kinds / unknown fields should be skipped.

## Callstacks

With `--callstacks` (launcher) or `capture_callstacks=TRUE` (in-process):

1. At startup, every currently-loaded module is enumerated and emitted as a
   `module_loaded` event with full PDB metadata: GUID + Age (for symsrv
   lookup), `TimeDateStamp` + `size` (image hash), the embedded PDB path, and
   the module's load base + size.
2. Live load/unload notifications via NT `LdrRegisterDllNotification` —
   anything the loader maps after startup also gets logged.
3. Every `created` event carries a `stack` array of up to 32 PC addresses
   captured via `RtlCaptureStackBackTrace`, starting at the dx12track hook
   frame and walking up into app code.

To resolve symbols offline:

- For each frame address, find the `module_loaded` whose `[base, base+size)`
  contains it. The PC inside the module is `addr - base`.
- Fetch the matching PDB via a symbol server using
  `<pdb_name>/<pdb_guid_no_dashes><pdb_age>/<pdb_name>` — Microsoft's
  `https://msdl.microsoft.com/download/symbols` covers all Windows / D3D
  components; private build artifacts need your own symstore.
- The launcher itself does not symbolicate — by design, capture stays cheap.

## Architecture notes

- **Injection**: `CreateProcess(CREATE_SUSPENDED)` →
  `CreateRemoteThread(LoadLibraryW)` → `ResumeThread`. Standard, requires no
  admin or driver, works on dev-machine games. No anti-cheat bypass.
- **Hooking**: [MinHook](https://github.com/TsudaKageyu/minhook) (vendored)
  for the one export hook on `D3D12CreateDevice`. Everything downstream is
  vtable patching — a single vtable per D3D12 COM class is shared across all
  instances, so we patch each vtable's create slots once.
- **Variant coverage**: every Create method on `ID3D12Device` through
  `ID3D12Device10` is hooked, including
  `CreateCommittedResource{,1,2,3}`, `CreatePlacedResource{,1,2}`,
  `CreateReservedResource{,1,2}`, `CreateHeap{,1}`, `CreateCommandQueue{,1}`,
  `CreateCommandList{,1}`, `CreatePipelineState`. Higher Device versions are
  probed at runtime and patched conditionally.
- **Destruction**: `IUnknown::Release` is patched on every object class's
  vtable as it's first encountered. The hook calls the real `Release` and
  emits `destroyed` when the returned refcount reaches zero — the pointer is
  used only as a map key after that point, never dereferenced.
- **Naming**: both `ID3D12Object::SetName` (slot 6) and `SetPrivateData`
  (slot 4) are hooked. `SetName` is internally a wrapper around
  `SetPrivateData(WKPDID_D3DDebugObjectNameW, …)`; the rename emit is
  idempotent so the double-fire collapses to one event. Apps that use
  `SetPrivateData` with `WKPDID_D3DDebugObjectName` (ANSI) are also caught.
- **IPC**: per-PID named pipe `\\.\pipe\dx12track-<pid>-<tick>`, fixed-size
  binary records. The launcher's `PipeServer` deserializes into a
  thread-safe `Model`; the `Renderer` reads snapshots and pushes one
  `WriteConsoleOutputW` per frame.
- **Threading**: `Tracker` uses a single `std::mutex` over the live-object
  map. Vtable patches are stored once per distinct vtable pointer, also
  under that mutex.
- **ETW (`--etw`)**: `src/etw/` (static lib `dx12track_etw`, C++20) owns the
  ETW session and the DxTimingCaptureLibrary handler; it is the only code
  that includes the library (and the Agility SDK's unprefixed `d3d12.h`)
  and talks to the launcher through plain types (`EtwMonitor.h`). The
  launcher's `Model` implements its callback interface and does the join
  under the same mutex as the pipe events; per-location byte totals are
  maintained incrementally so snapshots stay cheap.

## Out of scope (today)

- Attach to a running PID (launcher only does launch-and-inject).
- Reserved-resource physical (tile-mapped) memory accounting — we log virtual
  size only.
- D3D11 or DXGI swap-chain tracking.
- Symbol resolution at runtime (intentional — keeps capture cheap and
  detection-free; do it offline).
- ARM64 / Win32 targets — x64 only.

## License

The dx12track source under `src/` is provided as-is for use as a debugging
tool. Vendored MinHook (`src/third_party/minhook/`) is BSD-licensed; see its
own `LICENSE.txt`. The DirectX-Headers package fetched via vcpkg is MIT.
DxTimingCaptureLibrary (`external/DxTimingCaptureLibrary`, git submodule)
is MIT; see its `LICENSE` and `THIRD-PARTY-NOTICES.md`.
