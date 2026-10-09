# dx12track JSONL format (protocol v4)

The log file produced by `--callstacks` / `Dx12Track_StartLogging` is
[JSON Lines](https://jsonlines.org/): UTF-8 text, one JSON object per line,
`\n` line-terminator, no enclosing array. Each line is a single self-
contained event; there is no top-level document. Consumers stream-parse it
line-by-line.

With `--etw` the launcher additionally writes a second JSONL file, the
[ETW sidecar](#etw-sidecar-logetwjsonl), on the same timeline.

## Versioning

The first line of every file is a `hello` event whose `protocol` field is
the wire-format version (currently **4**). Consumers should refuse files
whose `protocol` they don't understand; we bump the field whenever an
existing event's fields change shape or are removed. Adding new event
kinds or appending fields to an existing event is a non-breaking change
for JSONL consumers.

`protocol` also versions the binary pipe format between `dx12track.dll`
and `dx12track.exe` (fixed-size structs in `src/common/EventTypes.h`), so a
field added to one of those structs bumps it even though the JSONL change
is additive. History:

| Protocol | Change |
| --- | --- |
| 3 | `residency_priority` events, `parent_heap_ptr` on placed resources |
| 4 | `created.ptr` (the app-visible interface pointer, the ETW join key) and `hello.qpc_start` (the QPC value at `ts_ns` 0). All `ts_ns` in a file now come from one clock (before v4 each DLL subsystem lazily started its own, so `ts_ns` of different event kinds could be offset from each other by a few ms). A v3 JSONL reader keeps working on v4 files. |

## File-level structure

```
hello                       (always first; one per file)
zero or more events in arrival order:
  module_loaded             (only when callstacks are on)
  module_unloaded
  diag                      (only when --verbose is on)
  created
  renamed
  destroyed
goodbye                      (last; one per file — absent if the host was killed)
```

Order is loose: events from different threads are serialized as they hit
the writer, not by any logical ordering. The only firm constraint is that
`hello` is line 1 and `goodbye` (when present) is the last line.

## Common fields

Every event has these two fields:

| Field | Type | Description |
| --- | --- | --- |
| `event` | string enum | One of the kinds in the table below |
| `ts_ns` | integer | Monotonic time since the DLL attached, in nanoseconds: `(QPC - qpc_start) * 1e9 / qpc_freq`, with both values from the `hello` event. Resolution = `qpc_freq` |

## How to write a schema

JSON Lines has no single root schema — each line is its own JSON document.
The conventional approach is one JSON Schema per event kind, dispatched by
the `event` discriminator. The full schema below is a single
[draft 2020-12](https://json-schema.org/) document with a top-level
`oneOf` that branches on `event`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/jonasmr/dx12track/blob/main/FORMAT.md",
  "title": "dx12track JSONL line",
  "type": "object",
  "required": ["event", "ts_ns"],
  "properties": {
    "event":  { "type": "string" },
    "ts_ns":  { "type": "integer", "minimum": 0 }
  },
  "oneOf": [
    { "$ref": "#/$defs/hello" },
    { "$ref": "#/$defs/created" },
    { "$ref": "#/$defs/renamed" },
    { "$ref": "#/$defs/destroyed" },
    { "$ref": "#/$defs/module_loaded" },
    { "$ref": "#/$defs/module_unloaded" },
    { "$ref": "#/$defs/goodbye" },
    { "$ref": "#/$defs/diag" },
    { "$ref": "#/$defs/residency_priority" }
  ],

  "$defs": {
    "HexAddress": { "type": "string", "pattern": "^0x[0-9a-f]+$" },

    "hello": {
      "type": "object",
      "required": ["event", "ts_ns", "pid", "protocol", "qpc_freq", "qpc_start", "exe"],
      "properties": {
        "event":     { "const": "hello" },
        "ts_ns":     { "type": "integer", "const": 0 },
        "pid":       { "type": "integer", "minimum": 1 },
        "protocol":  { "type": "integer", "const": 4 },
        "qpc_freq":  { "type": "integer", "minimum": 1 },
        "qpc_start": { "type": "integer", "minimum": 0 },
        "exe":       { "type": "string" }
      }
    },

    "created": {
      "type": "object",
      "required": [
        "event","ts_ns","id","ptr","type","alloc","heap","dim","format",
        "size","parent_heap_id","name"
      ],
      "properties": {
        "event":          { "const": "created" },
        "id":             { "type": "integer", "minimum": 1 },
        "ptr":            { "$ref": "#/$defs/HexAddress" },
        "type":           { "enum": ["Resource","Heap","DescriptorHeap",
                                     "CommandQueue","CommandAllocator",
                                     "CommandList","PipelineState",
                                     "RootSignature","Fence","QueryHeap",
                                     "CommandSignature","Device","Unknown"] },
        "alloc":          { "enum": ["None","Committed","Placed","Reserved","Heap"] },
        "heap":           { "enum": ["None","Default","Upload","Readback",
                                     "Custom","GpuUpload"] },
        "dim":            { "enum": ["Unknown","Buffer","Tex1D","Tex2D","Tex3D"] },
        "format":         { "type": "integer", "minimum": 0 },
        "size":           { "type": "integer", "minimum": 0 },
        "parent_heap_id": { "type": "integer", "minimum": 0 },
        "parent_heap_ptr": { "$ref": "#/$defs/HexAddress" },
        "name":           { "type": "string" },
        "stack": {
          "type": "array",
          "maxItems": 32,
          "items":   { "$ref": "#/$defs/HexAddress" }
        }
      }
    },

    "residency_priority": {
      "type": "object",
      "required": ["event","ts_ns","id","object_ptr","priority","priority_name"],
      "properties": {
        "event":         { "const": "residency_priority" },
        "id":            { "type": "integer", "minimum": 0 },
        "object_ptr":    { "$ref": "#/$defs/HexAddress" },
        "priority":      { "type": "integer" },
        "priority_name": { "enum": ["Minimum","Low","Normal","High","Maximum","Custom"] }
      }
    },

    "renamed": {
      "type": "object",
      "required": ["event","ts_ns","id","name"],
      "properties": {
        "event": { "const": "renamed" },
        "id":    { "type": "integer", "minimum": 1 },
        "name":  { "type": "string" }
      }
    },

    "destroyed": {
      "type": "object",
      "required": ["event","ts_ns","id"],
      "properties": {
        "event": { "const": "destroyed" },
        "id":    { "type": "integer", "minimum": 1 }
      }
    },

    "module_loaded": {
      "type": "object",
      "required": ["event","ts_ns","base","size","timestamp",
                   "pdb_age","pdb_guid","name","pdb_name"],
      "properties": {
        "event":     { "const": "module_loaded" },
        "base":      { "$ref": "#/$defs/HexAddress" },
        "size":      { "type": "integer", "minimum": 0 },
        "timestamp": { "type": "integer", "minimum": 0 },
        "pdb_age":   { "type": "integer", "minimum": 0 },
        "pdb_guid":  { "type": "string",
                       "pattern": "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$" },
        "name":      { "type": "string" },
        "pdb_name":  { "type": "string" }
      }
    },

    "module_unloaded": {
      "type": "object",
      "required": ["event","ts_ns","base"],
      "properties": {
        "event": { "const": "module_unloaded" },
        "base":  { "$ref": "#/$defs/HexAddress" }
      }
    },

    "goodbye": {
      "type": "object",
      "required": ["event","ts_ns","exit_code"],
      "properties": {
        "event":     { "const": "goodbye" },
        "exit_code": { "type": "integer", "minimum": 0 }
      }
    },

    "diag": {
      "type": "object",
      "required": ["event","ts_ns","msg"],
      "properties": {
        "event": { "const": "diag" },
        "msg":   { "type": "string" }
      }
    }
  }
}
```

The schema is non-normative: it's a description of what dx12track emits, not
something the producer cross-validates against at runtime.

## Event reference

### `hello`
First line of every file. Identifies the run.

```jsonl
{"event":"hello","ts_ns":0,"pid":50528,"protocol":4,"qpc_freq":10000000,"qpc_start":2849008433560,"exe":"D:\\game\\game.exe"}
```

- **`pid`**: process id of the target the DLL was injected into.
- **`protocol`**: the wire-format version. Currently **4**.
- **`qpc_freq`**: ticks per second of `QueryPerformanceCounter`.
- **`qpc_start`** *(v4)*: the raw `QueryPerformanceCounter` value that
  corresponds to `ts_ns` = 0. QPC is machine-wide, so anything else stamped
  with raw QPC (ETW in raw-timestamp mode, PIX, your own logging) maps onto
  this file's timeline as `ts_ns = (qpc - qpc_start) * 1e9 / qpc_freq`.
- **`exe`**: absolute path of the host process at attach time.

### `created`
A D3D12 object was successfully created and registered.

```jsonl
{"event":"created","ts_ns":52606800,"id":12,"ptr":"0x22c3ca534b0","type":"Resource","alloc":"Committed","heap":"Default","dim":"Tex2D","format":28,"size":65536,"parent_heap_id":0,"name":"","stack":["0x7ff89ddc9228","0x7ff928dfb972"]}
```

- **`id`**: monotonic, unique across the run. The same id appears in any
  subsequent `renamed` and `destroyed` for the same object.
- **`ptr`** *(v4)*: the interface pointer the app received from `Create*`
  (`*ppv` exactly as returned, not `QueryInterface(IUnknown)`), as a quoted
  hex string. This is the object address D3D12's ETW provider reports, so
  it's the key for joining with ETW data (the `--etw` sidecar does this for
  you). Addresses are reused after `destroyed`.
- **`type`**: high-level D3D12 object class. `Unknown` is reserved and
  should never appear in practice.
- **`alloc`**: how memory was sourced. `None` for non-memory-bearing
  objects (Fence, CommandList, PipelineState, …).
- **`heap`**: heap-property type. `None` when `alloc == "None"`.
- **`dim`**: only meaningful for Resources. `Unknown` for non-resources.
- **`format`**: raw `DXGI_FORMAT` integer. Map via the official table.
- **`size`**: bytes. For buffers this is `D3D12_RESOURCE_DESC.Width`; for
  textures it's `ID3D12Device::GetResourceAllocationInfo().SizeInBytes`;
  for heaps it's `D3D12_HEAP_DESC.SizeInBytes`. `0` for non-memory-bearing
  objects.
- **`parent_heap_id`**: only non-zero for `alloc == "Placed"` — refers to
  the `id` of the heap the resource was placed into. `0` otherwise.
- **`parent_heap_ptr`** *(optional)*: only present for `alloc == "Placed"` —
  the raw `IUnknown*` of the parent heap as a quoted hex string. Useful for
  cross-referencing with the app's own heap pointers when the tracker `id`
  isn't visible to the app.
- **`name`**: empty at creation; populated by subsequent `renamed` events
  that mirror the most-recent `ID3D12Object::SetName` /
  `SetPrivateData(WKPDID_D3DDebugObjectName{,W})` call.
- **`stack`** *(optional)*: present only when `--callstacks` /
  `Dx12Track_StartLogging(..., TRUE)` was active. Up to 32 PC addresses as
  hex strings; index 0 is the deepest dx12track-side frame, then the app's
  call site, then up the stack. Resolve with the `module_loaded` events
  (see below).

### `renamed`
A SetName / SetPrivateData(WKPDID_D3DDebugObjectName{,W}) call landed on a
tracked object.

```jsonl
{"event":"renamed","ts_ns":53180300,"id":12,"name":"ShadowMap"}
```

Idempotent: dx12track collapses duplicate names (e.g., when `SetName`
forwards to `SetPrivateData`) into a single event.

### `destroyed`
The last reference on a tracked object's refcount was released.

```jsonl
{"event":"destroyed","ts_ns":99999,"id":12}
```

### `module_loaded` / `module_unloaded`
Emitted only when `--callstacks` is active. At attach time dx12track
enumerates every already-loaded module; thereafter it registers with the
NT loader (`LdrRegisterDllNotification`) to catch every load and unload.

```jsonl
{"event":"module_loaded","ts_ns":0,"base":"0x7ff6c9060000","size":11304960,"timestamp":1780203981,"pdb_age":1,"pdb_guid":"8c3d35b8-e4f6-4d6e-8f7a-fd384f00bdbb","name":"D:\\game\\game.exe","pdb_name":"D:\\game\\game.pdb"}
{"event":"module_unloaded","ts_ns":12345,"base":"0x7ff6c9060000"}
```

- **`base`**, **`size`**: address range. A PC in `[base, base+size)` is
  inside this module.
- **`timestamp`**: `IMAGE_FILE_HEADER.TimeDateStamp`. Combine with `size`
  to fetch the binary itself from a symbol server (image hash).
- **`pdb_guid`**, **`pdb_age`**: identifiers for the matching PDB. Fetch
  via `https://msdl.microsoft.com/download/symbols` (or your own symstore)
  at the path
  `<pdb_filename>/<pdb_guid_with_dashes_removed><pdb_age>/<pdb_filename>`.
- **`name`**: full module path as loaded.
- **`pdb_name`**: PDB path baked into the PE's debug directory at link
  time.

A PC in a `stack` array resolves to `module_name + (pc - module_base)`
inside the matching PDB. dx12track itself never symbolicates — the JSONL
is enough to do it offline.

### `goodbye`
Last line of a normal run. Absent when the host was force-killed.

```jsonl
{"event":"goodbye","ts_ns":1234567890,"exit_code":0}
```

### `residency_priority`
The app called `ID3D12Device1::SetResidencyPriority` on one or more
pageables. Emitted **once per object in the call** — a single call setting
priorities for N objects becomes N events.

```jsonl
{"event":"residency_priority","ts_ns":123456,"id":42,"object_ptr":"0x1f0a1b2c0","priority":2013265920,"priority_name":"Normal"}
```

- **`id`**: tracker id of the pageable, or `0` if its vtable wasn't
  registered (rare — only happens if the app calls SetResidencyPriority on
  a pageable that came from a code path we don't intercept).
- **`object_ptr`**: raw `IUnknown*` of the pageable as a hex string. Always
  populated, useful as a backup id when `id == 0`.
- **`priority`**: the raw `D3D12_RESIDENCY_PRIORITY` enum value (an
  `int32`). Microsoft assigns the named tiers at very high numeric values
  (`Minimum = 0x28000000`, `Normal = 0x78000000`, `Maximum = 0xc8000000`)
  with the low 16 bits free for app-defined within-tier ordering.
- **`priority_name`**: convenience string. Apps that use a tier-aligned
  value get `Minimum`/`Low`/`Normal`/`High`/`Maximum`; anything else maps
  to `Custom` (you'll want to read `priority` directly in that case).

### `diag`
Verbose-mode trace events for diagnosing injection/hook-install issues.
Only present when `--verbose` was on. Stable shape, but the `msg` content
is freeform English text and not parser-friendly.

```jsonl
{"event":"diag","ts_ns":48200,"msg":"InstallExportHooks: D3D12CreateDevice export at 0x7ff..."}
```

## Notes for consumers

- **Treat addresses as opaque strings.** `base`, `module_unloaded.base`,
  and entries inside `stack` are quoted hex (`"0x..."`) to preserve all 64
  bits through JSON parsers that decode numbers as IEEE 754 doubles.
- **String escaping** follows standard JSON: `\\`, `\"`, `\n`, `\r`,
  `\t`, and `\u00XX` for other control chars. UTF-16 wide-char names are
  encoded as UTF-8 in the JSON string.
- **No size cap on the file.** A long-running session can produce gigabytes
  of events; consume the file as a stream.
- **Repeated `name`s are normal.** Two different objects often share a
  debug name. Use `id` as the unique key.
- **Negative timestamps don't exist.** `ts_ns` is monotonic from 0 at
  attach time. If you see decreases, the file was truncated or interleaved
  with another run.
- **Robustness.** A consumer that skips unknown event kinds and ignores
  unknown fields on known events will keep working across non-breaking
  format additions.

## ETW sidecar (`<log>.etw.jsonl`)

With `dx12track.exe --etw` the launcher runs a real-time ETW session
(Direct3D12 + DxgKrnl providers, decoded by
[DxTimingCaptureLibrary](external/DxTimingCaptureLibrary)) for the target
process and writes what it learns to a second JSONL file next to the main
log: `-o run.jsonl` produces `run.etw.jsonl` (a path that doesn't end in
`.jsonl` gets `.etw.jsonl` appended). The main log is unchanged; the sidecar
only refers to main-log objects by their `id`.

Same conventions as the main log: UTF-8 JSON Lines, every line has `event`
and `ts_ns`, addresses are quoted hex strings, unknown events/fields should
be skipped. `ts_ns` is on the **main log's timeline**: ETW records carry raw
QPC timestamps, converted with the main log's `hello.qpc_start` /
`qpc_freq`. Events that describe something ETW observed (`etw_object`,
`etw_bind`, `location`, `driver_size`, `residency`,
`etw_object_destroyed`) carry the ETW event's time (or, for a bind triggered
by the pipe, the main-log time of the triggering event); `counters`,
`etw_diag` and `etw_stats` carry the time the launcher wrote them. ETW
typically runs ~1 s behind the pipe, so lines are **not** sorted by `ts_ns`.

### Structure

```
etw_hello                       (always first)
etw_object / etw_bind / location / driver_size / residency /
etw_object_destroyed / counters (1 per second) / etw_diag   (any order)
etw_stats                       (last; absent if the launcher was killed)
```

### How objects are joined

The library reports each D3D12 object with its own id (`lib_id`) and its
address in the target process, which is exactly the main log's
`created.ptr`. The launcher binds an ETW object to the live main-log object
at that address if the types match (Resource, Heap, PipelineState,
DescriptorHeap, CommandAllocator) and the ETW object was created after the
previous occupant of that address was destroyed. Either side may arrive
first. Details a consumer may notice:

- **Rundowns.** The launcher asks D3D12 for a rundown (capture state) every
  10 s; the library then re-reports every live object under a *new*
  `lib_id`. Those re-reports are folded into the existing object (no new
  `etw_object` / `etw_bind` lines), only updating its size/location if they
  changed. `lib_id` in the sidecar is always the first id the object got.
- **Short-lived objects.** An object that was created and destroyed before
  its ETW record arrived can't be bound to a live object; it is attributed
  to the destroyed main-log object instead: `etw_bind` with `"late":true`
  (no `location` / `driver_size` lines follow for it).
- **Never bound**: objects D3D12 creates that the app never got from a
  hooked `Create*` (swap-chain buffers, runtime-internal resources and
  pipeline states, the device itself), CPU-only descriptor heaps (ETW never
  reports them, so they have no location or size), root signatures, command
  lists, queues, fences, query heaps, command signatures (no ETW object
  type).

### `etw_hello`
First line. Identifies the session and the main log it belongs to.

```jsonl
{"event":"etw_hello","ts_ns":157200,"etw_format":1,"pid":9376,"qpc_freq":10000000,"qpc_start":2849217653886,"session":"dx12track-etw-57540","rundown_interval_ms":10000,"pending_timeout_ms":10000,"main_log":"D:\\runs\\run.jsonl"}
```

- **`etw_format`**: version of the sidecar format (currently 1).
- **`pid`**, **`qpc_freq`**, **`qpc_start`**: copied from the main log's
  `hello`.
- **`session`**: the ETW session name (`dx12track-etw-<launcher pid>`).
- **`rundown_interval_ms`**: how often a rundown is requested.
- **`pending_timeout_ms`**: how long an ETW object waits for its main-log
  object before it counts as `pending_expired` in `etw_stats`.

### `etw_object`
The library reported a new D3D12 object (rundown re-reports excluded).

```jsonl
{"event":"etw_object","ts_ns":2559545700,"lib_id":2,"type":"Device","ptr":"0x2e0a14214e0","size":0,"group":"unknown"}
```

- **`type`**: `Device`, `Resource`, `Heap`, `PipelineState`, `StateObject`,
  `CommandAllocator`, `DescriptorHeap` or `MetaCommand`.
- **`ptr`**: the object's address in the target (= main-log `created.ptr`).
- **`size`**: size reported at creation: the GPU VA range for resources and
  heaps; the driver allocation for pipeline states, state objects,
  descriptor heaps and command allocators (often 0 until a rundown).
- **`group`**: memory location at creation, `vram` / `sys` / `unknown`.

### `etw_bind`
An ETW object was matched to a main-log object.

```jsonl
{"event":"etw_bind","ts_ns":2662730200,"id":4,"lib_id":5,"type":"CommandAllocator","ptr":"0x2e0a14ca920"}
{"event":"etw_bind","ts_ns":2464318600,"id":9,"lib_id":13,"type":"Resource","ptr":"0x28afd88ab30","late":true}
```

- **`id`**: main-log object id. **`lib_id`**, **`type`**, **`ptr`**: as in
  `etw_object`.
- **`late`** *(optional, `true`)*: the main-log object was already destroyed
  when the ETW record arrived (see above).

### `location`
Where a main-log object's memory is, written on its first bind and on every
change after that.

```jsonl
{"event":"location","ts_ns":2662730200,"id":4,"group":"unknown","via":"self"}
{"event":"location","ts_ns":2455319500,"id":3,"group":"vram","via":"heap"}
```

- **`group`**: `vram` (DXGK local segment group), `sys` (non-local: system
  memory) or `unknown` (not reported, or no GPU allocation, e.g. command
  allocators and pipeline states).
- **`via`**: `self` = the object's own allocation; `heap` = a placed
  resource following its parent heap (the kernel moves the heap's
  allocation as a whole, so placed resources take the heap's location when
  it is known, else their own creation-time location).

Location changes come from the library's segment-group reports (an
allocation paged out to system memory, or promoted back). Changes stamped
after the target process exited are ignored (process teardown).

### `driver_size`
The driver-side size of a pipeline state, descriptor heap or command
allocator, written when first known and whenever it changes to another
non-zero value.

```jsonl
{"event":"driver_size","ts_ns":2667757600,"id":9,"bytes":65536}
```

Shader-visible descriptor heaps report their size at creation. Pipeline
states only get a size from a rundown (or at destruction), and drivers pool
PSO memory, so many PSOs never get one (several PSOs can share an
allocation). Command allocators report 0 in practice.

### `residency`
A page-in or page-out of an object's allocation.

```jsonl
{"event":"residency","ts_ns":6969729600,"id":567,"lib_id":566,"type":"Resource","op":"page_in"}
```

- **`id`**: main-log id of the bound object, `0` if unbound.
- **`op`**: `page_in` or `page_out`. `MakeResident` / `Evict` operations are
  only counted (`etw_stats.make_resident` / `evict`): they fire for every
  residency-set update and would dominate the file.

### `counters`
The target process's video-memory counters as reported by DxgKrnl, once per
second (only after the first one was reported). All values in bytes, summed
over adapters; a key is present once the kernel has reported that counter.

```jsonl
{"event":"counters","ts_ns":4735187200,"local_budget":7874564096,"local_resident":7841062912,"local_usage":7838478336,"nonlocal_budget":108940925337,"nonlocal_resident":24920064,"nonlocal_usage":24920064,"demoted_high":117440512,"demoted":117440512,"page_ins":0,"page_outs":0}
```

| Key | Meaning (library counter name) |
| --- | --- |
| `local_budget` | OS budget for video memory ("Local Budget") |
| `local_resident` | memory actually placed in video memory ("Local Resident") |
| `local_usage` | memory the app asked to place in video memory ("Local Usage") |
| `nonlocal_budget` / `nonlocal_resident` / `nonlocal_usage` | the same for system memory ("Non-Local ...") |
| `demoted_min` .. `demoted_max` | memory of that residency priority that wanted video memory but didn't get it ("Minimum Priority" .. "Maximum Priority") |
| `demoted` | sum of the five `demoted_*` |
| `paging_local_to_nonlocal` / `paging_nonlocal_to_local` | kernel paging activity counters, if reported |
| `page_ins` / `page_outs` | running count of `residency` page-in / page-out operations |

The library reports in MB of 10^6 bytes; the launcher converts to bytes.
Values are the kernel's: transient dips (e.g. `local_resident` dropping to
~1 MB for one sample while the budget is recomputed) do occur.

### `etw_object_destroyed`
The library reported an object's destruction.

```jsonl
{"event":"etw_object_destroyed","ts_ns":15838032200,"lib_id":9}
```

### `etw_diag`
A diagnostic from the library (`DiagnosticsSink`). Freeform `msg`.

```jsonl
{"event":"etw_diag","ts_ns":4198069400,"severity":"warning","code":5,"msg":"Never received API Object Id for previous Pipeline State Compilation Event. Dropping the event (...)"}
```

- **`severity`**: `warning` or `error`. **`code`**: the library's
  `DiagnosticCode` value (e.g. 5 = pipeline-state compilation without
  object id, which is harmless; 7/8 = events/buffers lost).

### `etw_stats`
Last line: session and join statistics.

```jsonl
{"event":"etw_stats","ts_ns":46334872700,"events":1199247,"events_lost":0,"buffers_lost":0,"exceptions":0,"rundowns":4,"rundown_failures":0,"etw_objects_created":2813,"rundown_duplicates":2020,"binds":788,"late_matches":0,"bound_alive":0,"unbound_alive":0,"pending_expired":4,"group_changes":211,"make_resident":773,"evict":0,"page_ins":211,"page_outs":0,"post_exit_ignored":0,"diagnostics":1,"unbound_device":0,"unbound_resource":0,"unbound_heap":0,"unbound_pipeline_state":0,"unbound_state_object":0,"unbound_command_allocator":0,"unbound_descriptor_heap":0,"unbound_meta_command":0}
```

- **`events`**, **`events_lost`**, **`buffers_lost`**: ETW records
  delivered / dropped by the session. Any loss means the sidecar is
  incomplete.
- **`exceptions`**: records the library failed to decode (dropped).
- **`rundowns`**, **`rundown_failures`**: capture-state requests.
- **`etw_objects_created`**: library creation callbacks, including
  **`rundown_duplicates`** (re-reports folded into an existing object).
- **`binds`**, **`late_matches`**: see `etw_bind`.
- **`bound_alive`**, **`unbound_alive`**, **`unbound_<type>`**: ETW objects
  still alive at the end, bound or not, by type.
- **`pending_expired`**: ETW objects that found no main-log object within
  `pending_timeout_ms` (they may still have bound later).
- **`group_changes`**: segment-group change reports processed.
- **`make_resident`**, **`evict`**, **`page_ins`**, **`page_outs`**:
  residency operations.
- **`post_exit_ignored`**: group changes / residency operations stamped
  after the target exited (ignored).
- **`diagnostics`**: number of `etw_diag` lines.